/*
 * linux/fs/commit.c
 * 
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1998
 *
 * Copyright 1998 Red Hat corp --- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Journal commit routines for the generic filesystem journaling code;
 * part of the ext2fs journaling system.
 */

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/jfs.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/locks.h>
#include <linux/buffer.h>

void wakeup_bdflush(int);

/*
 * journal_commit_transaction
 *
 * The primary function for committing a transaction to the log.  This
 * function is called by the journal thread to begin a complete commit.
 */

void journal_commit_transaction(journal_t *journal)
{
	transaction_t *commit_transaction;
	struct buffer_head *bh, *new_bh, *descriptor;
	struct buffer_head *wbuf[journal_flush_nr_buffers];
	int bufs;
	int flags;
	int blocknr;
	char *tagp = NULL;
	journal_header_t *header;
	journal_block_tag_t *tag = NULL;
	int space_left = 0;
	int first_tag = 0;
	int tag_flag;
	
	int i;
	
	/*
	 * First job: lock down the current transaction and wait for 
	 * all outstanding updates to complete. 
	 */
	
	lock_journal(journal);

	J_ASSERT (journal->j_running_transaction != NULL);
	J_ASSERT (journal->j_committing_transaction == NULL);
	
	commit_transaction = journal->j_running_transaction;
	J_ASSERT (commit_transaction->t_state == T_RUNNING);

	jfs_debug (1, "Starting commit of transaction %d\n",
		   commit_transaction->t_tid);
	
	commit_transaction->t_state = T_LOCKED;
	while (commit_transaction->t_updates != 0) {
		unlock_journal(journal);
		sleep_on(&journal->j_wait_updates);
		lock_journal(journal);
	}

	J_ASSERT (commit_transaction->t_outstanding_credits <= journal->j_max_transaction_buffers);

	/* Do we need to erase the effects of a prior journal_flush? */
	if (journal->j_flags & JFS_FLUSHED)
		journal_update_superblock(journal, 1);

	/*
	 * First thing we are allowed to do is to discard any remaining
	 * BJ_Reserved buffers.  Note, it is _not_ permissible to assume
	 * that there are no such buffers: if a large filesystem
	 * operation like a truncate needs to split itself over multiple
	 * transactions, then it may try to do a journal_restart() while
	 * there are still BJ_Reserved buffers outstanding.  These must
	 * be released cleanly from the current transaction.
	 *
	 * In this case, the filesystem must still reserve write access
	 * again before modifying the buffer in the new transaction, but
	 * we do not require it to remember exactly which old buffers it
	 * has reserved.  This is consistent with the existing behaviour
	 * that multiple journal_get_write_access() calls to the same
	 * buffer are perfectly permissable.
	 */

	while (commit_transaction->t_reserved_list) {
		bh = commit_transaction->t_reserved_list;
		journal_refile_buffer(bh);
	}
	
	/* First part of the commit: force the revoke list out to disk.
	 * The revoke code generates its own metadata blocks on disk for this.
	 *
	 * It is important that we do this while the transaction is
	 * still locked.  Generating the revoke records should not
	 * generate any IO stalls, so this should be quick; and doing
	 * the work while we have the transaction locked means that we
	 * only ever have to maintain the revoke list for one
	 * transaction at a time.
	 */

	jfs_debug (3, "commit phase 1\n");

	journal_write_revoke_records(journal, commit_transaction);
	

	/* 
	 * Now that we have built the revoke records, we can start
	 * reusing the revoke list for a new running transaction.  We
	 * can now safely start committing the old transaction: time to
	 * get a new running transaction for incoming filesystem updates 
	 */

	commit_transaction->t_state = T_FLUSH;
	wake_up(&journal->j_wait_transaction_locked);
	
	journal->j_committing_transaction = commit_transaction;
	journal->j_running_transaction = NULL;
	
	commit_transaction->t_log_start = journal->j_head;

	jfs_debug (3, "commit phase 2\n");

	/* 
	 * Now start flushing things to disk, in the order they appear
	 * on the transaction lists.  Data blocks go first.  
	 */
	
 wait_for_data:
	bufs = 0;
	
	/* Cleanup any flushed data buffers from the data list.  Even in
	 * abort mode, we want to flush this out as soon as possible. */
	journal_clean_data_list(commit_transaction);

	bh = commit_transaction->t_datalist;
	
#if 0 /* This version flushes buffers from within commit, but interacts
         badly with bdflush going on at the same time. */
	if (bh) do {
		if (buffer_dirty(bh) && !buffer_locked(bh)) {
			wbuf[bufs++] = bh;
		}
		bh = bh->b_tnext;
	} while (bufs < journal_flush_nr_buffers &&
		 bh != commit_transaction->t_datalist);

	if (bufs) {
		unlock_journal(journal);
		ll_rw_block(WRITE, bufs, wbuf);
		run_task_queue(&tq_disk);
		lock_journal(journal);
		goto wait_for_data;
	}
#else /* This version relies on bdflush, so provides better throughput
         for now at the cost of potentially flushing other filesystems'
         buffers to disk before we strictly need to. */
	if (bh) {
		if (buffer_locked(bh)) {
			bh->b_count++;
			unlock_journal(journal);
			wait_on_buffer(bh);
			brelse(bh);
			lock_journal(journal);
		} else {
			unlock_journal(journal);
			wakeup_bdflush(1);
			lock_journal(journal);
		}
		goto wait_for_data;
	}
#endif

	/*
	 * If we got through the write loop without submitting any new
	 * IO, then wait for all previously submitted IO on the data
	 * list to complete. 
	 */

	bh = commit_transaction->t_datalist;
	
	if (bh) do {
		if (buffer_locked(bh)) {
			unlock_journal(journal);
			wait_on_buffer(bh);
			lock_journal(journal);
			goto wait_for_data;
		}
		
		bh = bh->b_tnext;
	} while (bh != commit_transaction->t_datalist);

	/*
	 * If we found any dirty or locked buffers, then we should have
	 * looped back up to the wait_for_data label.  If there weren't
	 * any then journal_clean_data_list should have wiped the list
	 * clean by now, so check that it is in fact empty. 
	 */
	
	J_ASSERT (commit_transaction->t_datalist == NULL);

	jfs_debug (3, "Commit phase 3\n");

	/*
	 * Way to go: we have now written out all of the data for a
	 * transaction!  Now comes the tricky part: we need to write out
	 * metadata now.  Loop over the transaction's entire buffer list:
	 */

	commit_transaction->t_state = T_COMMIT;
	descriptor = 0;
	bufs = 0;
	
	while (commit_transaction->t_buffers) {

		/* Find the next buffer to be journaled... */

		bh = commit_transaction->t_buffers;

		/* If we're in abort mode, we just un-journal the buffer and 
		   release it for background writing. */
		
		if (is_journal_abort(journal)) {
			journal_refile_buffer(bh);
			continue;
		}
		
		/* Make sure we have a descriptor block in which to
		   record the metadata buffer. */

		if (!descriptor) {
			J_ASSERT (bufs == 0);
			
			jfs_debug(4, "get descriptor\n");

			descriptor = journal_get_descriptor_buffer(journal);
			jfs_debug(4, "JFS: got buffer %ld (%p)\n", 
				  descriptor->b_blocknr, descriptor->b_data);
			header = (journal_header_t *) &descriptor->b_data[0];
			header->h_magic     = htonl(JFS_MAGIC_NUMBER);
			header->h_blocktype = htonl(JFS_DESCRIPTOR_BLOCK);
			header->h_sequence  = htonl(commit_transaction->t_tid);

			tagp = &descriptor->b_data[sizeof(journal_header_t)];
			space_left = descriptor->b_size - sizeof(journal_header_t);
			first_tag = 1;
			set_bit(BH_JWrite, &descriptor->b_state);
			wbuf[bufs++] = descriptor;
			
			/* Record it so that we can wait for IO
                           completion later */
			journal_file_buffer(descriptor, 
					    commit_transaction,
					    BJ_LogCtl);
		}

		/* Where is the buffer to be written? */
		
		blocknr = journal_next_log_block(journal);

		/* Bump b_count to prevent truncate from stumbling over
                   the shadowed buffer!  @@@ This can go if we ever get
                   rid of the BJ_IO/BJ_Shadow pairing of buffers. */
		bh->b_count++;

		/* Make a temporary IO buffer with which to write it out
                   (this will requeue both the metadata buffer and the
                   temporary IO buffer). */

		set_bit(BH_JWrite, &bh->b_state);
		flags = journal_write_metadata_buffer(commit_transaction,
						      bh, 
						      &new_bh,
						      blocknr);
		set_bit(BH_JWrite, &new_bh->b_state);
		wbuf[bufs++] = new_bh;
		
		/* Record the new block's tag in the current descriptor
                   buffer */

		tag_flag = 0;
		if (flags & 1)
			tag_flag |= JFS_FLAG_ESCAPE;
		if (!first_tag)
			tag_flag |= JFS_FLAG_SAME_UUID;

		tag = (journal_block_tag_t *) tagp;
		tag->t_blocknr = htonl(bh->b_blocknr);
		tag->t_flags = htonl(tag_flag);
		tagp += sizeof(journal_block_tag_t);
		space_left -= sizeof(journal_block_tag_t);
		
		if (first_tag) {
			memcpy (tagp, journal->j_uuid, 16);
			tagp += 16;
			space_left -= 16;
			first_tag = 0;
		}
		
		/* If there's no more to do, or if the descriptor is full,
		   let the IO rip! */

		if (bufs == journal_flush_nr_buffers ||
		    commit_transaction->t_buffers == NULL ||		    
		    space_left < sizeof(journal_block_tag_t) + 16) {

			jfs_debug(4, "JFS: Submit %d IOs\n", bufs);
			
			/* Write an end-of-descriptor marker before
                           submitting the IOs.  "tag" still points to
                           the last tag we set up. */
			
			tag->t_flags |= htonl(JFS_FLAG_LAST_TAG);

			unlock_journal(journal);
			ll_rw_block (WRITE, bufs, wbuf);
			lock_journal(journal);
			
			/* Force a new descriptor to be generated next
                           time round the loop. */
			descriptor = NULL;
			bufs = 0;
		}
	}

	/* Lo and behold: we have just managed to send a transaction to
           the log.  Before we can commit it, wait for the IO so far to
           complete.  Control buffers being written are on the
           transaction's t_log_list queue, and metadata buffers are on
           the t_iobuf_list queue. 
	
	   Wait for the transactions in reverse order.  That way we are
	   less likely to be woken up until all IOs have completed, and
	   so we incur less scheduling load.
	*/

	jfs_debug(3, "JFS: commit phase 4\n");

 wait_for_iobuf:
	while (commit_transaction->t_iobuf_list != NULL) {
		struct buffer_head *bh = commit_transaction->t_iobuf_list->b_tprev;
		if (buffer_locked(bh)) {
			unlock_journal(journal); 
			wait_on_buffer(bh);
			lock_journal(journal); 
			goto wait_for_iobuf;
		}
		
		clear_bit(BH_JWrite, &bh->b_state);

		journal_unfile_buffer(bh);
		put_unused_buffer_head(bh);
		
		/* We also have to unlock and free the corresponding
                   shadowed buffer */

		bh = commit_transaction->t_shadow_list->b_tprev;
		clear_bit(BH_JWrite, &bh->b_state);
		J_ASSERT(buffer_jdirty(bh));
		
		/* The metadata is now released for reuse, but we need
                   to remember it against this transaction so that when
                   we finally commit, we can do any checkpointing
                   required. */

		journal_file_buffer(bh, commit_transaction, BJ_Forget);

		/* Wake up any transactions which were waiting for this
		   IO to complete */
		unlock_journal(journal);
		wake_up(&bh->b_wait);
		brelse(bh);
		lock_journal(journal);
	}

	J_ASSERT (commit_transaction->t_shadow_list == NULL);
	
	jfs_debug(3, "JFS: commit phase 5\n");

 wait_for_ctlbuf:
	while (commit_transaction->t_log_list != NULL) {
		struct buffer_head *bh = commit_transaction->t_log_list->b_tprev;
		
		if (buffer_locked(bh)) {
			unlock_journal(journal); 
			wait_on_buffer(bh);
			lock_journal(journal); 
			goto wait_for_ctlbuf;
		}
		
		clear_bit(BH_JWrite, &bh->b_state);	
		journal_unfile_buffer(bh);
		bh->b_transaction = NULL;
		brelse(bh);
	}
	
	jfs_debug(3, "JFS: commit phase 6\n");

	/* Done it all: now write the commit record.  We should have
	 * cleaned up our previous buffers by now, so if we are in abort
	 * mode we can now just skip the rest of the journal write
	 * entirely. */

	if (is_journal_abort(journal))
		goto skip_commit;

	descriptor = journal_get_descriptor_buffer(journal);

	for (i = 0; i < descriptor->b_size; i += 512) {
		journal_header_t *tmp = (journal_header_t*)descriptor->b_data;
		tmp->h_magic = htonl(JFS_MAGIC_NUMBER);
		tmp->h_blocktype = htonl(JFS_COMMIT_BLOCK);
		tmp->h_sequence = htonl(commit_transaction->t_tid);
	}

	unlock_journal(journal); 
	ll_rw_block(WRITE, 1, &descriptor);
	wait_on_buffer(descriptor);
	brelse(descriptor);
	lock_journal(journal); 
	
	/* End of a transaction!  Finally, we can do checkpoint
           processing: any buffers committed as a result of this
           transaction can be removed from any checkpoint list it was on
           before. */

skip_commit:

	jfs_debug(3, "JFS: commit phase 7\n");

	J_ASSERT(commit_transaction->t_datalist == NULL);
	J_ASSERT(commit_transaction->t_buffers == NULL);
	J_ASSERT(commit_transaction->t_checkpoint_list == NULL);
	J_ASSERT(commit_transaction->t_iobuf_list == NULL);
	J_ASSERT(commit_transaction->t_shadow_list == NULL);
	J_ASSERT(commit_transaction->t_log_list == NULL);

	while (commit_transaction->t_forget) {
		transaction_t *cp_transaction;
		
		bh = commit_transaction->t_forget;
		J_ASSERT (bh->b_transaction == commit_transaction || 
			  bh->b_transaction == journal->j_running_transaction);


		/* 
		 * If there is undo-protected committed data against
		 * this buffer, then we can remove it now.  If it is a
		 * buffer needing such protection, the old frozen_data
		 * field now points to a committed version of the
		 * buffer, so rotate that field to the new committed
		 * data.
		 *
		 * Otherwise, we can just throw away the frozen data now.  
		 */
		if (bh->b_committed_data) {
			kfree(bh->b_committed_data);
			bh->b_committed_data = NULL;
			if (bh->b_frozen_data) {
				bh->b_committed_data = bh->b_frozen_data;
				bh->b_frozen_data = NULL;
			}
		} else if (bh->b_frozen_data) {
			kfree(bh->b_frozen_data);
			bh->b_frozen_data = NULL;
		}

		cp_transaction = bh->b_cp_transaction;
		if (cp_transaction) {
			J_ASSERT (commit_transaction != cp_transaction);
			journal_remove_checkpoint(bh);
		}
		journal_insert_checkpoint(bh, commit_transaction);
       
		journal_refile_buffer(bh);
	}
	
	/* Done with this transaction! */

	jfs_debug(3, "JFS: commit phase 8\n");

	J_ASSERT (commit_transaction->t_state == T_COMMIT);
	commit_transaction->t_state = T_FINISHED;

	J_ASSERT (commit_transaction == journal->j_committing_transaction);
	journal->j_commit_sequence = commit_transaction->t_tid;
	journal->j_committing_transaction = NULL;

	if (commit_transaction->t_checkpoint_list == NULL) {
		journal_drop_transaction(journal, commit_transaction);
	} else {
		if (journal->j_checkpoint_transactions == NULL) {
			journal->j_checkpoint_transactions = commit_transaction;
			commit_transaction->t_cpnext = commit_transaction;
			commit_transaction->t_cpprev = commit_transaction;
		} else {
			commit_transaction->t_cpnext = journal->j_checkpoint_transactions;
			commit_transaction->t_cpprev = commit_transaction->t_cpnext->t_cpprev;
			commit_transaction->t_cpnext->t_cpprev = commit_transaction;
			commit_transaction->t_cpprev->t_cpnext = commit_transaction;
		}
	}
	
	jfs_debug(1, "Commit %d complete, head %d\n", 
		  journal->j_commit_sequence, 
		  journal->j_tail_sequence);

	unlock_journal(journal); 
	wake_up(&journal->j_wait_done_commit);
}
