/*
 * linux/fs/checkpoint.c
 * 
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1999
 *
 * Copyright 1999 Red Hat Software --- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Checkpoint routines for the generic filesystem journaling code.  
 * Part of the ext2fs journaling system.  
 *
 * Checkpointing is the process of ensuring that a section of the log is
 * committed fully to disk, so that that portion of the log can be
 * reused.
 */

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/jfs.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/locks.h>
#include <linux/buffer.h>

/*
 * Unlink a buffer from a transaction. 
 */

static inline void buffer_unlink(struct buffer_head *bh)
{
	transaction_t *transaction;
	
	transaction = bh->b_cp_transaction;
	bh->b_cp_transaction = NULL;

	bh->b_cpnext->b_cpprev = bh->b_cpprev;
	bh->b_cpprev->b_cpnext = bh->b_cpnext;
	if (transaction->t_checkpoint_list == bh)
		transaction->t_checkpoint_list = bh->b_cpnext;
	if (transaction->t_checkpoint_list == bh)
		transaction->t_checkpoint_list = NULL;
}
 

/*
 * log_wait_for_space: wait until there is space in the journal.
 *
 * Called with the journal already locked, but it will be unlocked if we have
 * to wait for a checkpoint to free up some space in the log.
 */

void log_wait_for_space(journal_t *journal, int nblocks)
{
	while (log_space_left(journal) < nblocks) {
		if (journal->j_flags & JFS_ABORT)
			return;
		unlock_journal(journal);
		down(&journal->j_checkpoint_sem);
		lock_journal(journal);
		
		/* Test again, another process may have checkpointed
		 * while we were waiting for the checkpoint lock */
		if (log_space_left(journal) < nblocks) {
			log_do_checkpoint(journal, nblocks);
		}
		up(&journal->j_checkpoint_sem);
	}
}


/*
 * Clean up a transaction's checkpoint list.  
 *
 * We wait for any pending IO to complete and make sure any clean
 * buffers are removed from the transaction. 
 *
 * Return 1 if we performed any actions which might have destroyed the
 * checkpoint (refile_buffer deletes the transaction when the last
 * checkpoint buffer is cleansed)
 */

static int cleanup_transaction(journal_t *journal, transaction_t *transaction)
{
	struct buffer_head *bh;
	
	bh = transaction->t_checkpoint_list;
	if (!bh)
		return 0;
	
	do {
		if (buffer_locked(bh)) {
			unlock_journal(journal);
			bh->b_count++;
			wait_on_buffer(bh);
			brelse(bh);
			lock_journal(journal);
			return 1;
		}
		
		if (bh->b_transaction != NULL) {
			unlock_journal(journal);
			log_wait_commit (journal,bh->b_transaction->t_tid);
			lock_journal(journal);
			return 1;
		}
		
		if (!buffer_dirty(bh) && !buffer_jdirty(bh) &&
		    bh->b_list != BUF_CLEAN) {
			unlock_journal(journal);
			refile_buffer(bh);
			lock_journal(journal);
			return 1;
		}
		
		bh = bh->b_cpnext;
	} while (bh != transaction->t_checkpoint_list);
	
	return 0;
}


/*
 * Try to flush one buffer from the checkpoint list to disk.
 *
 * Return 1 if something happened which requires us to abort the current
 * scan of the checkpoint list.  
 */
static inline int flush_buffer(journal_t *journal, transaction_t *transaction,
			       struct buffer_head *bh)
{
	/* 
	 * Flush out only dirty, writable buffers.  
	 */
	
	if (buffer_dirty(bh) && !buffer_locked(bh) && 
	    bh->b_jlist == 0) {
		J_ASSERT(bh->b_transaction == NULL);
		
		/*
		 * Important: we are about to write the buffer, and
		 * possibly block, while still holding the journal lock.
		 * We cannot afford to let the transaction logic start
		 * messing around with this buffer before we write it to
		 * disk, as that would break recoverability.  
		 */
		
		bh->b_count++;

		J_ASSERT(!test_bit(BH_JWrite, &bh->b_state));
		set_bit(BH_JWrite, &bh->b_state);
		ll_rw_block(WRITE, 1, &bh);
		clear_bit(BH_JWrite, &bh->b_state);
				
		/* We may have blocked, so check whether our context is
		 * still valid and start over the transaction list again
		 * if not. */
				
		if (transaction != journal->j_checkpoint_transactions ||
		    bh->b_cp_transaction != transaction) {
			brelse(bh);
			return 1;
		}
		bh->b_count--; /* NOT brelse: don't block! */
	}
	return 0;
}

	
/*
 * Perform an actual checkpoint.  We don't write out only enough to
 * satisfy the current blocked requests: rather we submit a reasonbly
 * sized chunk of the outstanding data to disk at once for
 * efficiency.  log_wait_for_space() will retry if we didn't free enough.
 * 
 * However, we _do_ take into account the amount requested so that once
 * the IO has been queued, we can return as soon as enough of it has
 * completed to disk.  
 *
 * The journal should be locked before calling this function.
 */

int log_do_checkpoint (journal_t *journal, int nblocks)
{
	transaction_t * transaction;
	int result;
	int target;
	struct buffer_head *bh;

	jfs_debug(1, "Start checkpoint\n");

	/* How many buffers will we try to write at once? */
	#define MAXBUFS 128
	
	/* 
	 * First thing: if there are any transactions in the log which
	 * don't need checkpointing, just eliminate them from the
	 * journal straight away.  
	 */
	
	result = cleanup_journal_tail(journal);
	jfs_debug(1, "cleanup_journal_tail returned %d\n", result);
	if (result <= 0)
		return result;

	/*
	 * OK, we need to start writing disk blocks.  Try to free up a
	 * quarter of the log in a single checkpoint if we can.
	 */

	target = (journal->j_last - journal->j_first) / 4;
	
 repeat:
	while (target > 0 && 
	       (transaction = journal->j_checkpoint_transactions) != NULL) {

		/* 
		 * We'll remove all of the buffers from the transaction
		 * first, and then flush them to disk once we are no
		 * longer worried about the effect of refile_buffer
		 * trying to checkpoint-unlink the buffers.
		 */
		
		bh = transaction->t_checkpoint_list;
		J_ASSERT (bh != NULL);
		
		do {
			if (flush_buffer(journal, transaction, bh))
				goto repeat;
			bh = bh->b_cpnext;
		} while (bh != transaction->t_checkpoint_list);

		/*
		 * We have walked the whole transaction list without
		 * finding anything to write to disk.  We had better be
		 * able to make some progress or we are in trouble. 
		 */
			
		J_ASSERT(cleanup_transaction(journal, transaction));
	}

	result = cleanup_journal_tail(journal);
	if (result < 0)
		return result;
	
	return 0;
}


/*
 * Check the list of checkpoint transactions for the journal to see if
 * we have already got rid of any since the last update of the log tail
 * in the journal superblock.  If so, we can instantly roll the
 * superblock forward to remove those transactions from the log.
 * 
 * Return <0 on error, 0 on success, 1 if there was nothing to clean up.
 * 
 * Called with the journal lock held.
 *
 * This is the only part of the journaling code which really needs to be
 * aware of transaction aborts.  Checkpointing involves writing to the
 * main filesystem area rather than to the journal, so it can proceed
 * even in abort state, but we must not update the journal superblock if
 * we have an abort error outstanding.
 */


int cleanup_journal_tail(journal_t *journal)
{
	transaction_t * transaction;
	tid_t		first_tid;
	unsigned long	blocknr, freed;

	/* OK, work out the oldest transaction remaining in the log, and
	 * the log block it starts at. 
	 * 
	 * If the log is now empty, we need to work out which is the
	 * next transaction ID we will write, and where it will
	 * start. */

	transaction = journal->j_checkpoint_transactions;
	if (transaction) {
		first_tid = transaction->t_tid;
		blocknr = transaction->t_log_start;
	} else if ((transaction = journal->j_committing_transaction) != NULL) {
		first_tid = transaction->t_tid;
		blocknr = transaction->t_log_start;
	} else if ((transaction = journal->j_running_transaction) != NULL) {
		first_tid = transaction->t_tid;
		blocknr = journal->j_head;
	} else {
		first_tid = journal->j_transaction_sequence;
		blocknr = journal->j_head;
	}
	J_ASSERT (blocknr != 0);

	/* If the oldest pinned transaction is at the tail of the log
           already then there's not much we can do right now. */
	if (journal->j_tail_sequence == first_tid)
		return 1;

	if (journal->j_flags & JFS_ABORT)
		return -EROFS;
	
	/* OK, update the superblock to recover the freed space.
	 * Physical blocks come first: have we wrapped beyond the end of
	 * the log?  */
	freed = blocknr - journal->j_tail;
	if (blocknr < journal->j_tail)
		freed = freed + journal->j_last - journal->j_first;

	jfs_debug(1,
		  "Cleaning journal tail from %d to %d (offset %lu), "
		  "freeing %lu\n",
		  journal->j_tail_sequence, first_tid, blocknr, freed);

	journal->j_free += freed;
	journal->j_tail_sequence = first_tid;
	journal->j_tail = blocknr;
	journal_update_superblock(journal, 1);
	return 0;
}


/* Checkpoint list management */

/* 
 * journal_remove_checkpoint: called after a buffer has been committed
 * to disk (either by being write-back flushed to disk, or being
 * committed to the log).
 *
 * We cannot safely clean a transaction out of the log until all of the
 * buffer updates committed in that transaction have safely been stored
 * elsewhere on disk.  To achieve this, all of the buffers in a
 * transaction need to be maintained on the transaction's checkpoint
 * list until they have been rewritten, at which point this function is
 * called to remove the buffer from the existing transaction's
 * checkpoint list.  
 */

void journal_remove_checkpoint(struct buffer_head *bh)
{
	transaction_t *transaction;
	journal_t *journal;

	
	if ((transaction = bh->b_cp_transaction) == NULL)
		return;

	journal = transaction->t_journal;

	buffer_unlink(bh);

	if (transaction->t_checkpoint_list != NULL)
		return;

	/* There is one special case to worry about: if we have just
           pulled the buffer off a committing transaction's forget list,
           then even if the checkpoint list is empty, the transaction
           obviously cannot be dropped! */

	if (transaction == journal->j_committing_transaction)
		return;

	/* OK, that was the last buffer for the transaction: we can now
	   safely remove this transaction from the log */

	journal_drop_transaction(journal, transaction);
	
	/* Just in case anybody was waiting for more transactions to be
           checkpointed... */
	wake_up(&journal->j_wait_logspace);
}

/*
 * journal_insert_checkpoint: put a committed buffer onto a checkpoint
 * list so that we know when it is safe to clean the transaction out of
 * the log. 
 */

void journal_insert_checkpoint(struct buffer_head *bh, 
			       transaction_t *transaction)
{
	J_ASSERT (buffer_dirty(bh) || buffer_jdirty(bh));
	J_ASSERT (bh->b_cp_transaction == NULL);

	bh->b_cp_transaction = transaction;

	if (!transaction->t_checkpoint_list) {
		bh->b_cpnext = bh->b_cpprev = bh;
	} else {
		bh->b_cpnext = transaction->t_checkpoint_list;
		bh->b_cpprev = transaction->t_checkpoint_list->b_cpprev;
		bh->b_cpprev->b_cpnext = bh;
		bh->b_cpnext->b_cpprev = bh;
	}
	transaction->t_checkpoint_list = bh;
}

/*
 * We've finished with this transaction structure: adios...
 * 
 * The transaction must have no links except for the checkpoint by this
 * point.  */

void journal_drop_transaction(journal_t *journal, transaction_t *transaction)
{
	if (transaction->t_cpnext) {
		transaction->t_cpnext->t_cpprev = transaction->t_cpprev;
		transaction->t_cpprev->t_cpnext = transaction->t_cpnext;
		if (journal->j_checkpoint_transactions == transaction)
			journal->j_checkpoint_transactions = transaction->t_cpnext;
		if (journal->j_checkpoint_transactions == transaction)
			journal->j_checkpoint_transactions = NULL;
	}

	J_ASSERT (transaction->t_ilist == NULL);
	J_ASSERT (transaction->t_buffers == NULL);
	J_ASSERT (transaction->t_datalist == NULL);
	J_ASSERT (transaction->t_forget == NULL);
	J_ASSERT (transaction->t_iobuf_list == NULL);
	J_ASSERT (transaction->t_shadow_list == NULL);
	J_ASSERT (transaction->t_log_list == NULL);
	J_ASSERT (transaction->t_checkpoint_list == NULL);
	J_ASSERT (transaction->t_updates == 0);
	
	J_ASSERT (transaction->t_journal->j_committing_transaction != transaction);
	
	jfs_debug (1, "Dropping transaction %d, all done\n", 
		   transaction->t_tid);
	kfree (transaction);
}

