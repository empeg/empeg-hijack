/*
 * linux/fs/journal.c
 * 
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1998
 *
 * Copyright 1998 Red Hat corp --- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Generic filesystem journal-writing code; part of the ext2fs
 * journaling system.  
 *
 * This file manages journals: areas of disk reserved for logging
 * transactional updates.  This includes the kernel journaling thread
 * which is responsible for scheduling updates to the log.
 *
 * We do not actually manage the physical storage of the journal in this
 * file: that is left to a per-journal policy function, which allows us
 * to store the journal within a filesystem-specified area for ext2
 * journaling (ext2 can use a reserved inode for storing the log).
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/jfs.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/locks.h>
#include <linux/buffer.h>
#include <linux/smp_lock.h>
#include <linux/sched.h>

#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>

/*
 * Tunable parameters
 */

/* Number of buffers to flush out at once before starting the disk up */
int journal_flush_nr_buffers = 64;


#ifdef JFS_DEBUG
int journal_enable_debug = 0;
EXPORT_SYMBOL(journal_enable_debug);
#endif

EXPORT_SYMBOL(journal_start);
EXPORT_SYMBOL(journal_restart);
EXPORT_SYMBOL(journal_extend);
EXPORT_SYMBOL(journal_stop);
EXPORT_SYMBOL(journal_get_write_access);
EXPORT_SYMBOL(journal_get_create_access);
EXPORT_SYMBOL(journal_get_undo_access);
EXPORT_SYMBOL(journal_dirty_data);
EXPORT_SYMBOL(journal_dirty_metadata);
EXPORT_SYMBOL(journal_release_buffer);
EXPORT_SYMBOL(journal_forget);
EXPORT_SYMBOL(journal_sync_buffer);
EXPORT_SYMBOL(journal_flush);
EXPORT_SYMBOL(journal_revoke);

EXPORT_SYMBOL(journal_init_dev);
EXPORT_SYMBOL(journal_init_inode);
EXPORT_SYMBOL(journal_update_format);
EXPORT_SYMBOL(journal_check_used_features);
EXPORT_SYMBOL(journal_check_available_features);
EXPORT_SYMBOL(journal_set_features);
EXPORT_SYMBOL(journal_create);
EXPORT_SYMBOL(journal_load);
EXPORT_SYMBOL(journal_release);
EXPORT_SYMBOL(journal_recover);
EXPORT_SYMBOL(journal_update_superblock);
EXPORT_SYMBOL(__journal_abort);
EXPORT_SYMBOL(journal_abort);
EXPORT_SYMBOL(journal_errno);
EXPORT_SYMBOL(journal_ack_err);
EXPORT_SYMBOL(journal_clear_err);

static int journal_convert_superblock_v1(journal_t *, journal_superblock_t *);

/*
 * Helper function used to manage commit timeouts
 */

static void commit_timeout(unsigned long __data)
{
	struct task_struct * p = (struct task_struct *) __data;

	wake_up_process(p);
}

/* Static check for data structure consistency.  There's no code
 * invoked --- we'll just get a linker failure if things aren't right.
 */
void __journal_internal_check(void)
{
	extern void journal_bad_superblock_size(void);
	if (sizeof(struct journal_superblock_s) != 1024)
		journal_bad_superblock_size();
}

/* 
 * kjournald: The main thread function used to manage a logging device
 * journal.
 *
 * This kernel thread is responsible for two things:
 *
 * 1) COMMIT:  Every so often we need to commit the current state of the
 *    filesystem to disk.  The journal thread is responsible for writing
 *    all of the metadata buffers to disk.
 *
 * 2) CHECKPOINT: We cannot reuse a used section of the log file until all
 *    of the data in that part of the log has been rewritten elsewhere on 
 *    the disk.  Flushing these old buffers to reclaim space in the log is
 *    known as checkpointing, and this thread is responsible for that job.
 */

int kjournald(void *arg)
{
	journal_t *journal = (journal_t *) arg;
	transaction_t *transaction;
	struct timer_list timer;
	
	lock_kernel();
	
	exit_files(current);
	exit_mm(current);

	spin_lock_irq(&current->sigmask_lock);
	sigfillset(&current->blocked);
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	current->session = 1;
	current->pgrp = 1;
	sprintf(current->comm, "kjournald");

	/* Set up an interval timer which can be used to trigger a
           commit wakeup after the commit interval expires */
	init_timer(&timer);
	timer.data = (unsigned long) current;
	timer.function = commit_timeout;
	journal->j_commit_timer = &timer;
	
	/* Record that the journal thread is running */
	journal->j_task = current;
	wake_up(&journal->j_wait_done_commit);

	jfs_debug(1, "Journal thread starting.\n");
	
	/* And now, wait forever for commit wakeup events. */
	while (1) {
		if (journal->j_flags & JFS_UNMOUNT)
			break;

		if (journal->j_commit_sequence != journal->j_commit_request) {
			if (journal->j_commit_timer_active) {
				journal->j_commit_timer_active = 0;
				del_timer(journal->j_commit_timer);
			}

			journal_commit_transaction(journal);
			continue;
		}
		
		wake_up(&journal->j_wait_done_commit);
		interruptible_sleep_on(&journal->j_wait_commit);

		/* Were we woken up by a commit wakeup event? */
		if ((transaction = journal->j_running_transaction) != NULL &&
		    time_after_eq(jiffies, transaction->t_expires))
			journal->j_commit_request = transaction->t_tid;
	}
	
	if (journal->j_commit_timer_active) {
		journal->j_commit_timer_active = 0;
		del_timer(journal->j_commit_timer);
	}

	journal->j_task = NULL;
	wake_up(&journal->j_wait_done_commit);
	jfs_debug(1, "Journal thread exiting.\n");
	return 0;
}

static void journal_start_thread(journal_t *journal)
{
	kernel_thread(kjournald, (void *) journal, 
		      CLONE_VM | CLONE_FS | CLONE_FILES);
	while (!journal->j_task)
		sleep_on(&journal->j_wait_done_commit);
}
	
static void journal_kill_thread(journal_t *journal)
{
	journal->j_flags |= JFS_UNMOUNT;
	
	while (journal->j_task) {
		wake_up(&journal->j_wait_commit);
		sleep_on(&journal->j_wait_done_commit);
	}
}
	

/*
 * journal_clean_data_list: cleanup after data IO.
 *
 * Once the IO system has finished writing the buffers on the transaction's
 * data list, we can remove those buffers from the list.  This function 
 * scans the list for such buffers and removes them cleanly.
 *
 * We assume that the journal is already locked.  This function may block.
 */

void journal_clean_data_list(transaction_t *transaction)
{
	struct buffer_head *bh, *next, *last;
	
 repeat:
	next = transaction->t_datalist;
	if (!next)
		return;

	last = next->b_tprev;
	
	do {
		bh = next;
		transaction->t_datalist = next = bh->b_tnext;
		
		if (!buffer_locked(bh) && 
		    !buffer_dirty(bh)) {
			journal_unfile_buffer(bh);
			bh->b_transaction = NULL;
			unlock_journal(transaction->t_journal);
			refile_buffer(bh);
			/* refile buffer may block! */
			lock_journal(transaction->t_journal);
			goto repeat;
		}
	} while (bh != last);
}

/*
 * journal_write_metadata_buffer: write a metadata buffer to the journal.
 *
 * Writes a metadata buffer to a given disk block.  The actual IO is not
 * performed but a new buffer_head is constructed which labels the data
 * to be written with the correct destination disk block.
 *
 * Any magic-number escaping which needs to be done will cause a
 * copy-out here.  If the buffer happens to start with the
 * JFS_MAGIC_NUMBER, then we can't write it to the log directly: the
 * magic number is only written to the log for descripter blocks.  In
 * this case, we copy the data and replace the first word with 0, and we
 * return a result code which indicates that this buffer needs to be
 * marked as an escaped buffer in the corresponding log descriptor
 * block.  The missing word can then be restored when the block is read
 * during recovery.
 * 
 * If the source buffer has already been modified by a new transaction
 * since we took the last commit snapshot, we use the frozen copy of
 * that data for IO.  If we end up using the existing buffer_head's data
 * for the write, then we *have* to lock the buffer to prevent anyone
 * else from using and possibly modifying it while the IO is in
 * progress.
 *
 * The function returns a pointer to the buffer_heads to be used for IO.
 *
 * We assume that the journal has already been locked in this function.
 *
 * Return value:
 *  <0: Error
 * >=0: Finished OK
 * 
 * On success:
 * Bit 0 set == escape performed on the data
 * Bit 1 set == buffer copy-out performed (kfree the data after IO)
 */

int journal_write_metadata_buffer(transaction_t *transaction,
				  struct buffer_head  *bh_in,
				  struct buffer_head **bh_out,
				  int blocknr)
{
	int need_copy_out = 0;
	int done_copy_out = 0;
	int do_escape = 0;
	char *new_data;
	struct buffer_head * new_bh;

	/* 
	 * The buffer really shouldn't be locked: only the current committing
	 * transaction is allowed to write it, so nobody else is allowed
	 * to do any IO.
	 */

	if (buffer_locked(bh_in)) {
		printk(KERN_EMERG "Buffer locked in " __FUNCTION__
		       ", flags 0x%08lx, count %d\n",
		       bh_in->b_state, bh_in->b_count);
		BUG();
	}
	
	J_ASSERT (buffer_jdirty(bh_in));

	/*
	 * If a new transaction has already done a buffer copy-out, then 
	 * we use that version of the data for the commit.
	 */

	if (bh_in->b_frozen_data) {
		done_copy_out = 1;
		new_data = bh_in->b_frozen_data;
	} else
		new_data = bh_in->b_data;
		
	/*
	 * Check for escaping 
	 */

	if (* ((unsigned int *) new_data) == htonl(JFS_MAGIC_NUMBER)) {
		need_copy_out = 1;
		do_escape = 1;
	}

	/*
	 * Do we need to do a data copy?
	 */

	if (need_copy_out && !done_copy_out) {
		char *tmp;
		do {
			tmp = kmalloc(bh_in->b_size, GFP_KERNEL);
			if (!tmp) {
				printk (KERN_ERR __FUNCTION__
					": ENOMEM getting frozen data, "
					"trying again.\n");
				sys_sched_yield();
			}
		} while (!tmp);

		bh_in->b_frozen_data = tmp;
		memcpy (tmp, new_data, bh_in->b_size);
		new_data = tmp;
		done_copy_out = 1;
	}
	
	/*
	 * Right, time to make up the new buffer_head.
	 */

	do {
		new_bh = get_unused_buffer_head(0);
		if (!new_bh) {
			printk (KERN_ERR __FUNCTION__
				": ENOMEM at get_unused_buffer_head, "
				"trying again.\n");
			sys_sched_yield();
			}
	} while (!new_bh);

	init_buffer(new_bh, transaction->t_journal->j_dev, blocknr, 
		    end_buffer_io_sync, NULL);

	new_bh->b_size    = bh_in->b_size;
	new_bh->b_data    = new_data;
	new_bh->b_state	  = (1 << BH_Temp) | (1 << BH_Dirty);
	new_bh->b_count	  = 1;

	*bh_out = new_bh;

	/* 
	 * Did we need to do an escaping?  Now we've done all the
	 * copying, we can finally do so.
	 */

	if (do_escape)
		* ((unsigned int *) new_data) = 0;

	/* 
	 * The to-be-written buffer needs to get moved to the io queue,
	 * and the original buffer whose contents we are shadowing or
	 * copying is moved to the transaction's shadow queue. 
	 */

	journal_file_buffer(bh_in, transaction, BJ_Shadow);
	journal_file_buffer(new_bh, transaction, BJ_IO);
	
	return do_escape | (done_copy_out << 1);
}


/* The call to lock_buffer() above should be the only place we ever lock
 * a buffer which is being journaled (ignoring the checkpoint lists).
 *
 * @@@ This is heavily dependent on the big kernel lock in 2.2! */

void jfs_prelock_buffer_check(struct buffer_head *bh)
{
	transaction_t *transaction = bh->b_transaction;
	journal_t *journal;

	if (bh->b_jlist == 0 && transaction == NULL)
		return;

	J_ASSERT(bh->b_jlist == 0 || bh->b_jlist == BJ_LogCtl || bh->b_jlist == BJ_IO || bh->b_jlist == BJ_Data);
	J_ASSERT(transaction != NULL);
	J_ASSERT(buffer_dirty(bh));

	journal = transaction->t_journal;
	if (bh->b_jlist == BJ_Data) {
		J_ASSERT(transaction == journal->j_running_transaction || transaction == journal->j_committing_transaction);
	} else {
		J_ASSERT(transaction == journal->j_running_transaction || transaction == journal->j_committing_transaction);
		J_ASSERT(test_bit(BH_JWrite, &bh->b_state));
	}
}

/* We are not allowed to forget the dirty status on any buffer which is
 * being journaled! */

void jfs_preclean_buffer_check(struct buffer_head *bh)
{
	jfs_prelock_buffer_check(bh);
}




/*
 * Allocation code for the journal file.  Manage the space left in the
 * journal, so that we can begin checkpointing when appropriate.
 */

/*
 * log_space_left: Return the number of free blocks left in the journal.
 *
 * Called with the journal already locked.
 */

int log_space_left (journal_t *journal)
{
	int left = journal->j_free;

	/* Be pessimistic here about the number of those free blocks
	 * which might be required for log descriptor control blocks. */

#define MIN_LOG_RESERVED_BLOCKS 32 /* Allow for rounding errors */

	left -= MIN_LOG_RESERVED_BLOCKS;
	
	if (left <= 0)
		return 0;
	left -= (left >> 3);
	return left;
}


void log_start_commit (journal_t *journal, transaction_t *transaction)
{
	/*
	 * Are we already doing a recent enough commit?
	 */
	if (tid_geq(journal->j_commit_request, transaction->t_tid))
		return;

	/* 
	 * We want a new commit: OK, mark the request and wakup the
	 * commit thread.  We do _not_ do the commit ourselves.  
	 */
	
	journal->j_commit_request = transaction->t_tid;
	jfs_debug(1, "JFS: requesting commit %d/%d\n", 
		  journal->j_commit_request,
		  journal->j_commit_sequence);
	wake_up(&journal->j_wait_commit);
}

/* Wait for a specified commit to complete. */
void log_wait_commit (journal_t *journal, tid_t tid)
{
	while (tid_gt(tid, journal->j_commit_sequence)) {
		wake_up(&journal->j_wait_commit);
		sleep_on(&journal->j_wait_done_commit);
	}
}

/* 
 * Log buffer allocation routines: 
 */

unsigned long journal_next_log_block(journal_t *journal)
{
	unsigned long blocknr;

	J_ASSERT(journal->j_free > 1);
	
	blocknr = journal->j_head;
	journal->j_head++;
	journal->j_free--;
	if (journal->j_head == journal->j_last)
		journal->j_head = journal->j_first;
	
	if (journal->j_inode) {
		blocknr = bmap(journal->j_inode, blocknr);
		J_ASSERT(blocknr != 0);
	}
	return blocknr;
}

/* 
 * We play buffer_head aliasing tricks to write data/metadata blocks to
 * the journal without copying their contents, but for journal
 * descriptor blocks we do need to generate bona fide buffers.
 */

struct buffer_head * journal_get_descriptor_buffer(journal_t *journal)
{
	struct buffer_head *bh;
	unsigned long blocknr = journal_next_log_block(journal);
	bh = getblk(journal->j_dev, blocknr, journal->j_blocksize);
	bh->b_state |= (1 << BH_Dirty);
	J_ASSERT(bh != NULL);
	return bh;
}




/*
 * Management for journal control blocks: functions to create and
 * destroy journal_t structures, and to initialise and read existing
 * journal blocks from disk.  */

/* First: create and setup a journal_t object in memory.  We initialise
 * very few fields yet: that has to wait until we have created the
 * journal structures from from scratch, or loaded them from disk. */

static journal_t * journal_init_common (void)
{
	journal_t *journal;
	int err;

	journal = kmalloc(sizeof(*journal), GFP_KERNEL);
	if (!journal)
		return 0;
	memset(journal, 0, sizeof(*journal));

	init_waitqueue(&journal->j_wait_lock);
	init_waitqueue(&journal->j_wait_transaction_locked);
	init_waitqueue(&journal->j_wait_logspace);
	init_waitqueue(&journal->j_wait_done_commit);
	init_waitqueue(&journal->j_wait_checkpoint);
	init_waitqueue(&journal->j_wait_commit);
	init_waitqueue(&journal->j_wait_updates);
	init_MUTEX(&journal->j_barrier);
	init_MUTEX(&journal->j_checkpoint_sem);

	/* The journal is marked for error until we succeed with recovery! */
	journal->j_flags = JFS_ABORT;

	/* Set up a default-sized revoke table for the new mount. */
	err = journal_init_revoke(journal, JOURNAL_REVOKE_DEFAULT_HASH);
	if (err) {
		kfree(journal);
		return 0;
	}
	
	return journal;
}

/* journal_init_dev and journal_init_inode:
 *
 * Create a journal structure assigned some fixed set of disk blocks to
 * the journal.  We don't actually touch those disk blocks yet, but we
 * need to set up all of the mapping information to tell the journaling
 * system where the journal blocks are.
 *
 * journal_init_dev creates a journal which maps a fixed contiguous
 * range of blocks on an arbitrary block device.
 * 
 * journal_init_inode creates a journal which maps an on-disk inode as
 * the journal.  The inode must exist already, must support bmap() and
 * must have all data blocks preallocated.  
 */

journal_t * journal_init_dev (kdev_t dev, int start, int len, int blocksize)
{
	journal_t *journal = journal_init_common();
	struct buffer_head *bh;
	
	if (!journal)
		return 0;
	
	journal->j_dev = dev;
	journal->j_blk_offset = start;
	journal->j_maxlen = len;
	journal->j_blocksize = blocksize;
	
	bh = getblk(journal->j_dev, start, journal->j_blocksize);
	J_ASSERT(bh != NULL);
	journal->j_sb_buffer = bh;
	journal->j_superblock = (journal_superblock_t *) bh->b_data;

	return journal;
}

journal_t * journal_init_inode (struct inode *inode)
{
	struct buffer_head *bh;
	journal_t *journal = journal_init_common();
	int blocknr;
	
	if (!journal)
		return 0;
	
	journal->j_dev = inode->i_dev;
	journal->j_inode = inode;
	jfs_debug(1, 
		  "journal %p: inode %s/%ld, size %ld, bits %d, blksize %ld\n",
		  journal,
		  kdevname(inode->i_dev), inode->i_ino, inode->i_size, 
		  inode->i_sb->s_blocksize_bits, inode->i_sb->s_blocksize);
	
	journal->j_maxlen = inode->i_size >> inode->i_sb->s_blocksize_bits;
	journal->j_blocksize = inode->i_sb->s_blocksize;

	blocknr = bmap(journal->j_inode, 0);
	J_ASSERT(blocknr != 0);
	bh = getblk(journal->j_dev, blocknr, journal->j_blocksize);
	J_ASSERT(bh != NULL);
	journal->j_sb_buffer = bh;
	journal->j_superblock = (journal_superblock_t *) bh->b_data;

	return journal;
}

/*
 * Given a journal_t structure, initialise the various fields for
 * startup of a new journaling session.  We use this both when creating
 * a journal, and after recovering an old journal to reset it for
 * subsequent use.  
 */

static int journal_reset (journal_t *journal)
{
	journal_superblock_t *sb = journal->j_superblock;
	unsigned int first, last;
	
	first = ntohl(sb->s_first);
	last = ntohl(sb->s_maxlen);

	journal->j_first = first;
	journal->j_last = last;
	
	journal->j_head = first;
	journal->j_tail = first;
	journal->j_free = last - first;
	
	journal->j_tail_sequence = journal->j_transaction_sequence;
	journal->j_commit_sequence = journal->j_transaction_sequence - 1;
	journal->j_commit_request = journal->j_commit_sequence;	

	journal->j_max_transaction_buffers = journal->j_maxlen / 4;
	journal->j_commit_interval = (HZ * 5);

	/* Add the dynamic fields and write it to disk. */
	journal_update_superblock(journal, 1);

	lock_journal(journal);
	journal_start_thread(journal);
	unlock_journal(journal);

	return 0;
}

/* 
 * Given a journal_t structure which tells us which disk blocks we can
 * use, create a new journal superblock and initialise all of the
 * journal fields from scratch.  */

int journal_create (journal_t *journal)
{
	int blocknr;
	struct buffer_head *bh;
	journal_superblock_t *sb;
	int i;
	
	if (journal->j_maxlen < JFS_MIN_JOURNAL_BLOCKS) {
		printk (KERN_ERR "Journal length (%d blocks) too short.\n",
			journal->j_maxlen);
		return -EINVAL;
	}

	/* Zero out the entire journal on disk.  We cannot afford to
	   have any blocks on disk beginning with JFS_MAGIC_NUMBER. */
	jfs_debug(1, "JFS: Zeroing out journal blocks...\n");
	for (i = 0; i < journal->j_maxlen; i++) {
		blocknr = i;
		if (journal->j_inode) {
			blocknr = bmap(journal->j_inode, blocknr);
			if (blocknr == 0) {
				printk (KERN_ERR 
					"Error writing journal block %d\n", i);
				return -EIO;
			}
		}
		bh = getblk(journal->j_dev, blocknr, journal->j_blocksize);
		if (buffer_locked(bh))
			wait_on_buffer(bh);
		memset (bh->b_data, 0, journal->j_blocksize);
		mark_buffer_dirty(bh, 0);
		mark_buffer_uptodate(bh, 1);
		brelse(bh);
	}
	sync_buffers(journal->j_dev, 1);
	jfs_debug(1, "JFS: journal cleared.\n");
	
	/* OK, fill in the initial static fields in the new superblock */
	sb = journal->j_superblock;

	sb->s_header.h_magic	 = htonl(JFS_MAGIC_NUMBER);
	sb->s_header.h_blocktype = htonl(JFS_SUPERBLOCK_V2);
	
	sb->s_blocksize	= htonl(journal->j_blocksize);
	sb->s_maxlen	= htonl(journal->j_maxlen);
	sb->s_first	= htonl(1);

	journal->j_transaction_sequence = 1;
	
	journal->j_flags &= ~JFS_ABORT;
	journal->j_format_version = 2;

	return journal_reset(journal);
}

/*
 * Update a journal's dynamic superblock fields and write it to disk,
 * optionally waiting for the IO to complete. 
*/

void journal_update_superblock(journal_t *journal, int wait)
{
	journal_superblock_t *sb = journal->j_superblock;
	struct buffer_head *bh = journal->j_sb_buffer;

	jfs_debug(1,"JFS: updating superblock (start %ld, seq %d, errno %d)\n",
		  journal->j_tail, journal->j_tail_sequence, 
		  journal->j_errno);
	
	sb->s_sequence = htonl(journal->j_tail_sequence);
	sb->s_start    = htonl(journal->j_tail);
	sb->s_errno    = htonl(journal->j_errno);

	mark_buffer_dirty(bh, 0);
	ll_rw_block(WRITE, 1, &bh);
	if (wait)
		wait_on_buffer(bh);

	/* If we have just flushed the log (by marking s_start==0), then
	 * any future commit will have to be careful to update the
	 * superblock again to re-record the true start of the log. */

	if (sb->s_start)
		journal->j_flags &= ~JFS_FLUSHED;
	else
		journal->j_flags |= JFS_FLUSHED;
}
	

/* 
 * Read the superblock for a given journal, performing initial
 * validation of the format.
 */

static int journal_get_superblock(journal_t *journal)
{
	struct buffer_head *bh;
	journal_superblock_t *sb;
	
	bh = journal->j_sb_buffer;

	J_ASSERT(bh != NULL);
	if (!buffer_uptodate(bh)) {
		ll_rw_block(READ, 1, &bh);
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh)) {
			printk (KERN_ERR
				"JFS: IO error reading journal superblock\n");
			return -EIO;
		}
	}

	sb = journal->j_superblock;

	if (sb->s_header.h_magic != htonl(JFS_MAGIC_NUMBER) ||
	    sb->s_blocksize != htonl(journal->j_blocksize)) {
		printk (KERN_WARNING "JFS: no valid journal superblock found\n");
		return -EINVAL;
	}

	switch(ntohl(sb->s_header.h_blocktype)) {
	case JFS_SUPERBLOCK_V1:
		journal->j_format_version = 1;
		break;
	case JFS_SUPERBLOCK_V2:
		journal->j_format_version = 2;
		break;
	default:
		printk (KERN_WARNING "JFS: unrecognised superblock format ID\n");
		return -EINVAL;
	}

	if (ntohl(sb->s_maxlen) < journal->j_maxlen)
		journal->j_maxlen = ntohl(sb->s_maxlen);
	else if (ntohl(sb->s_maxlen) > journal->j_maxlen) {
		printk (KERN_WARNING "JFS: journal file too short\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * Load the on-disk journal superblock and read the key fields into the
 * journal_t. 
 */

static int load_superblock(journal_t *journal)
{
	int err;
	journal_superblock_t *sb;
	
	err = journal_get_superblock(journal);
	if (err)
		return err;
	
	sb = journal->j_superblock;
	
	journal->j_tail_sequence = ntohl(sb->s_sequence);
	journal->j_tail = ntohl(sb->s_start);
	journal->j_first = ntohl(sb->s_first);
	journal->j_last = ntohl(sb->s_maxlen);
	journal->j_errno = ntohl(sb->s_errno);

	return 0;
}


/*
 * Given a journal_t structure which tells us which disk blocks contain
 * a journal, read the journal from disk to initialise the in-memory
 * structures.
 */

int journal_load (journal_t *journal)
{
	unsigned long blocknr;
	int err;
	
	blocknr = 0;
	if (journal->j_inode) {
		blocknr = bmap(journal->j_inode, blocknr);
		J_ASSERT(blocknr != 0);
	}

	err = load_superblock(journal);
	if (err)
		return err;

	/* If this is a V2 superblock, then we have to check the
	 * features flags on it. */

	if (journal->j_format_version >= 2) {
		journal_superblock_t *sb = journal->j_superblock;

		if ((sb->s_feature_ro_compat & 
		     ~cpu_to_be32(JFS_KNOWN_ROCOMPAT_FEATURES)) ||
		    (sb->s_feature_incompat & 
		     ~cpu_to_be32(JFS_KNOWN_INCOMPAT_FEATURES))) {
			printk (KERN_WARNING
				"JFS: Unrecognised features on journal\n");
			return -EINVAL;
		}
	}
			
	/* Let the recovery code check whether it needs to recover any
	 * data from the journal. */
	if (journal_recover(journal))
		goto recovery_error;
	
	/* OK, we've finished with the dynamic journal bits:
	 * reinitialise the dynamic contents of the superblock in memory
	 * and reset them on disk. */
	if (journal_reset(journal))
		goto recovery_error;

	journal->j_flags &= ~JFS_ABORT;
	journal->j_flags |= JFS_LOADED;
	return 0;

recovery_error:
	printk (KERN_WARNING "JFS: recovery failed\n");
	return -EIO;
}

/* 
 * Release a journal_t structure once it is no longer in use by the
 * journaled object.  
 */

void journal_release (journal_t *journal)
{
	/* Wait for the commit thread to wake up and die. */
	journal_kill_thread(journal);

	/* Force a final log commit */
	if (journal->j_running_transaction)
		journal_commit_transaction(journal);

	/* Force any old transactions to disk */
	lock_journal(journal);
	while (journal->j_checkpoint_transactions != NULL)
		log_do_checkpoint(journal, 1);

	J_ASSERT(journal->j_running_transaction == NULL);	
	J_ASSERT(journal->j_committing_transaction == NULL);
	J_ASSERT(journal->j_checkpoint_transactions == NULL);

	/* We can now mark the journal as empty. */
	journal->j_tail = 0;
	journal->j_tail_sequence = ++journal->j_transaction_sequence;
	journal_update_superblock(journal, 1);

	if (journal->j_inode)
		iput(journal->j_inode);
	if (journal->j_revoke)
		journal_destroy_revoke(journal);
	
	unlock_journal(journal);
	brelse(journal->j_sb_buffer);
	kfree(journal);
}


/* Published API: Check whether the journal uses all of a given set of
 * features.  Return true (non-zero) if it does. */

int journal_check_used_features (journal_t *journal, 
				 unsigned long compat, 
				 unsigned long ro, 
				 unsigned long incompat)
{
	journal_superblock_t *sb;
	
	if (!compat && !ro && !incompat)
		return 1;
	if (journal->j_format_version == 1)
		return 0;
	
	sb = journal->j_superblock;
	
	if (((be32_to_cpu(sb->s_feature_compat) & compat) == compat) &&
	    ((be32_to_cpu(sb->s_feature_ro_compat) & ro) == ro) &&
	    ((be32_to_cpu(sb->s_feature_incompat) & incompat) == incompat))
		return 1;
	
	return 0;
}

/* Published API: Check whether the journaling code supports the use of
 * all of a given set of features on this journal.  Return true
 * (non-zero) if it can. */

int journal_check_available_features (journal_t *journal, 
				      unsigned long compat, 
				      unsigned long ro, 
				      unsigned long incompat)
{
	journal_superblock_t *sb;
	
	if (!compat && !ro && !incompat)
		return 1;
	
	sb = journal->j_superblock;
	
	/* We can support any known requested features iff the
	 * superblock is in version 2.  Otherwise we fail to support any
	 * extended sb features. */
	
	if (journal->j_format_version != 2)
		return 0;
	
	if ((compat   & JFS_KNOWN_COMPAT_FEATURES) == compat &&
	    (ro       & JFS_KNOWN_ROCOMPAT_FEATURES) == ro &&
	    (incompat & JFS_KNOWN_INCOMPAT_FEATURES) == incompat)
		return 1;

	return 0;
}

/* Published API: Mark a given journal feature as present on the
 * superblock.  Returns true if the requested features could be set. */

int journal_set_features (journal_t *journal, 
			  unsigned long compat, 
			  unsigned long ro, 
			  unsigned long incompat)
{
	journal_superblock_t *sb;
	
	if (journal_check_used_features(journal, compat, ro, incompat))
		return 1;
	
	if (!journal_check_available_features(journal, compat, ro, incompat))
		return 0;
	
	jfs_debug(1, "Setting new features 0x%lx/0x%lx/0x%lx\n",
		  compat, ro, incompat);
	
	sb = journal->j_superblock;
	
	sb->s_feature_compat    |= cpu_to_be32(compat);
	sb->s_feature_ro_compat |= cpu_to_be32(ro);
	sb->s_feature_incompat  |= cpu_to_be32(incompat);

	return 1;
}


/* 
 * Published API:
 * Given an initialised but unloaded journal struct, poke about in the
 * on-disk structure to update it to the most recent supported version.
 */

int journal_update_format (journal_t *journal)
{
	journal_superblock_t *sb;
	int err;
	
	err = journal_get_superblock(journal);
	if (err)
		return err;

	sb = journal->j_superblock;

	switch (ntohl(sb->s_header.h_blocktype)) {
	case JFS_SUPERBLOCK_V2:
		return 0;
	case JFS_SUPERBLOCK_V1:
		return journal_convert_superblock_v1(journal, sb);
	default:
	}
	return -EINVAL;
}

static int journal_convert_superblock_v1(journal_t *journal,
					 journal_superblock_t *sb)
{
	int offset, blocksize;
	struct buffer_head *bh;

	printk(KERN_WARNING "JFS: Converting superblock from version 1 to 2.\n");
	
	/* Pre-initialise new fields to zero */
	offset = ((char *) &(sb->s_feature_compat)) - ((char *) sb);
	blocksize = ntohl(sb->s_blocksize);
	memset(&sb->s_feature_compat, 0, blocksize-offset);

	sb->s_nr_users = cpu_to_be32(1);
	sb->s_header.h_blocktype = cpu_to_be32(JFS_SUPERBLOCK_V2);
	journal->j_format_version = 2;

	bh = journal->j_sb_buffer;
	mark_buffer_dirty(bh, 0);
	ll_rw_block(WRITE, 1, &bh);
	wait_on_buffer(bh);
	return 0;
}


/* 
 * Flush all data for a given journal to disk and empty the journal.
 * Filesystems can use this when remounting readonly to ensure that
 * recovery does not need to happen on remount.  
 */

int journal_flush (journal_t *journal)
{
	int err = 0;
	transaction_t *transaction = NULL;
	unsigned long old_tail;
	
	/* Force everything buffered to the log... */
	if (journal->j_running_transaction) {
		transaction = journal->j_running_transaction;
		log_start_commit(journal, transaction);
	} else if (journal->j_committing_transaction)
		transaction = journal->j_committing_transaction;

	/* Wait for the log commit to complete... */
	if (transaction)
		log_wait_commit(journal, transaction->t_tid);
	
	/* ...and flush everything in the log out to disk. */
	lock_journal(journal);
	while (!err && journal->j_checkpoint_transactions != NULL)
		err = log_do_checkpoint(journal, journal->j_maxlen);
	cleanup_journal_tail(journal);

	/* Finally, mark the journal as really needing no recovery.
	 * This sets s_start==0 in the underlying superblock, which is
	 * the magic code for a fully-recovered superblock.  Any future
	 * commits of data to the journal will restore the current
	 * s_start value. */
	old_tail = journal->j_tail;
	journal->j_tail = 0;
	journal_update_superblock(journal, 1);
	journal->j_tail = old_tail;
	
	unlock_journal(journal);

	J_ASSERT(!journal->j_running_transaction);
	J_ASSERT(!journal->j_committing_transaction);
	J_ASSERT(!journal->j_checkpoint_transactions);
	J_ASSERT(journal->j_head == journal->j_tail);
	J_ASSERT(journal->j_tail_sequence == journal->j_transaction_sequence);
	
	return err;
}

/* 
 * Wipe out all of the contents of a journal, safely.  This will produce
 * a warning if the journal contains any valid recovery information.
 * Must be called between journal_init_*() and journal_load().
 *
 * If (write) is non-zero, then we wipe out the journal on disk; otherwise
 * we merely suppress recovery.
 */

int journal_wipe (journal_t *journal, int write)
{
	journal_superblock_t *sb;
	int err = 0;

	J_ASSERT (!(journal->j_flags & JFS_LOADED));
	
	err = load_superblock(journal);
	if (err)
		return err;
	
	sb = journal->j_superblock;

	if (!journal->j_tail)
		goto no_recovery;

	printk (KERN_WARNING "JFS: %s recovery information on journal\n",
		write ? "Clearing" : "Ignoring");

	err = journal_skip_recovery(journal);
	if (write)
		journal_update_superblock(journal, 1);

 no_recovery:
	return err;
}

/*
 * journal_dev_name: format a character string to describe on what
 * device this journal is present.  
 */

char * journal_dev_name(journal_t *journal)
{
	kdev_t dev;
	
	if (journal->j_inode)
		dev = journal->j_inode->i_dev;
	else
		dev = journal->j_dev;
	
	return kdevname(dev);
}

/* 
 * journal_abort: perform a complete, immediate shutdown of the ENTIRE
 * journal (not of a single transaction).  This operation cannot be
 * undone without closing and reopening the journal.
 *
 * The journal_abort function is intended to support higher level error
 * recovery mechanisms such as the ext2/ext3 remount-readonly error
 * mode.
 *
 * Journal abort has very specific semantics.  Any existing dirty,
 * unjournaled buffers in the main filesystem will still be written to
 * disk by bdflush, but the journaling mechanism will be suspended
 * immediately and no further transaction commits will be honoured.  
 * 
 * Any dirty, journaled buffers will be written back to disk without
 * hitting the journal.  Atomicity cannot be guaranteed on an aborted
 * filesystem, but we _do_ attempt to leave as much data as possible
 * behind for fsck to use for cleanup.
 *
 * Any attempt to get a new transaction handle on a journal which is in
 * ABORT state will just result in an -EROFS error return.  A
 * journal_stop on an existing handle will return -EIO if we have
 * entered abort state during the update.  
 *
 * Recursive transactions are not disturbed by journal abort until the
 * final journal_stop, which will receive the -EIO error.
 *
 * Finally, the journal_abort call allows the caller to supply an errno
 * which will be recored (if possible) in the journal superblock.  This
 * allows a client to record failure conditions in the middle of a
 * transaction without having to complete the transaction to record the
 * failure to disk.  ext3_error, for example, now uses this
 * functionality.
 *
 * Errors which originate from within the journaling layer will NOT
 * supply an errno; a null errno implies that absolutely no further
 * writes are done to the journal (unless there are any already in
 * progress).
 */

/* Quick version for internal journal use (doesn't lock the journal) */
void __journal_abort (journal_t *journal)
{
	transaction_t *transaction;

	printk (KERN_ERR "Aborting journal on device %s.\n", 
		journal_dev_name(journal));

	journal->j_flags |= JFS_ABORT;
	transaction = journal->j_running_transaction;
	if (transaction)
		log_start_commit(journal, transaction);
}

/* Full version for external use */
void journal_abort (journal_t *journal, int errno)
{
	lock_journal(journal);
	
	if (journal->j_flags & JFS_ABORT)
		goto out;
	
	if (!journal->j_errno)
		journal->j_errno = errno;

	__journal_abort(journal);

	if (errno)
		journal_update_superblock(journal, 1);

 out:
	unlock_journal(journal);
}

int journal_errno (journal_t *journal)
{
	int err;
	
	lock_journal(journal);
	if (journal->j_flags & JFS_ABORT)
		err = -EROFS;
	else 
		err = journal->j_errno;
	unlock_journal(journal);
	return err;
}

int journal_clear_err (journal_t *journal)
{
	int err = 0;
	
	lock_journal(journal);
	if (journal->j_flags & JFS_ABORT)
		err = -EROFS;
	else 
		journal->j_errno = 0;
	unlock_journal(journal);
	return err;
}

void journal_ack_err (journal_t *journal)
{
	lock_journal(journal);
	if (journal->j_errno)
		journal->j_flags |= JFS_ACK_ERR;
	unlock_journal(journal);
}


