/*
 * linux/fs/transaction.c
 * 
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1998
 *
 * Copyright 1998 Red Hat corp --- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Generic filesystem transaction handling code; part of the ext2fs
 * journaling system.  
 *
 * This file manages transactions (compound commits managed by the
 * journaling code) and handles (individual atomic operations by the
 * filesystem).
 */

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/jfs.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/locks.h>
#include <linux/timer.h>


/* 
 * Forward declarations for internal use by this file only.
 */

static inline void 
blist_add_buffer(struct buffer_head **, struct buffer_head *);
static inline void
blist_del_buffer(struct buffer_head **, struct buffer_head *);


/*
 * Journal locking.
 *
 * We need to lock the journal during transaction state changes so that
 * nobody ever tries to take a handle on the running transaction while
 * we are in the middle of moving it to the commit phase.  
 *
 * Note that the locking is completely interrupt unsafe.  We never touch
 * journal structures from interrupts.
 */

void __wait_on_journal (journal_t * journal)
{
	while (journal->j_locked)
		sleep_on (&journal->j_wait_lock);
}


/*
 * get_transaction: obtain a new transaction_t object.
 *
 * Simply allocate and initialise a new transaction.  Create it in
 * RUNNING state and add it to the current journal (which should not
 * have an existing running transaction: we only make a new transaction
 * once we have started to commit the old one).
 *
 * Preconditions:
 *	The journal MUST be locked.  We don't perform atomic mallocs on the
 *	new transaction	and we can't block without protecting against other
 *	processes trying to touch the journal while it is in transition.
 */

transaction_t * get_transaction (journal_t * journal)
{
	transaction_t * transaction;

	J_ASSERT (journal->j_locked);
	
	transaction = kmalloc (sizeof (transaction_t), GFP_KERNEL);
	if (!transaction)
		return NULL;
	
	memset (transaction, 0, sizeof (transaction_t));
	
	transaction->t_journal = journal;
	transaction->t_state = T_RUNNING;
	transaction->t_tid = journal->j_transaction_sequence++;
	transaction->t_expires = jiffies + journal->j_commit_interval;

	/* Set up the commit timer for the new transaction. */
	J_ASSERT (!journal->j_commit_timer_active);
	journal->j_commit_timer_active = 1;
	journal->j_commit_timer->expires = transaction->t_expires;
	add_timer(journal->j_commit_timer);
	
	J_ASSERT (journal->j_running_transaction == NULL);
	journal->j_running_transaction = transaction;

	return transaction;
}



/*
 * Handle management.
 *
 * A handle_t is an object which represents a single atomic update to a
 * filesystem, and which tracks all of the modifications which form part
 * of that one update.
 */

/*
 * start_this_handle: Given a handle, deal with any locking or stalling
 * needed to make sure that there is enough journal space for the handle
 * to begin.  Attach the handle to a transaction and set up the
 * transaction's buffer credits.  
 */

static int start_this_handle(journal_t *journal, handle_t *handle)
{
	transaction_t *transaction;
	int needed;
	int nblocks = handle->h_buffer_credits;
	
	jfs_debug(4, "New handle %p going live.\n", handle);

repeat:

	lock_journal(journal);

	if ((journal->j_flags & JFS_ABORT) ||
	    (journal->j_errno != 0 && !(journal->j_flags & JFS_ACK_ERR))) {
		unlock_journal(journal);
		return -EROFS; 
	}

	/* Wait on the journal's transaction barrier if necessary */
	if (journal->j_barrier_count) {
		unlock_journal(journal);
		sleep_on(&journal->j_wait_transaction_locked);
		goto repeat;
	}
	
repeat_locked:
	if (!journal->j_running_transaction)
		get_transaction(journal);
	/* @@@ Error? */
	J_ASSERT(journal->j_running_transaction);
	
	transaction = journal->j_running_transaction;

	/* If the current transaction is locked down for commit, wait
	 * for the lock to be released. */

	if (transaction->t_state == T_LOCKED) {
		unlock_journal(journal);
		jfs_debug(3, "Handle %p stalling...\n", handle);
		sleep_on(&journal->j_wait_transaction_locked);
		goto repeat;
	}
	
	/* If there is not enough space left in the log to write all
	 * potential buffers requested by this operation, we need to
	 * stall pending a log checkpoint to free some more log
	 * space. */

	needed = transaction->t_outstanding_credits + nblocks;

	if (needed > journal->j_max_transaction_buffers) {
		/* If the current transaction is already too large, then
		 * start to commit it: we can then go back and attach
		 * this handle to a new transaction. */
		
		jfs_debug(2, "Handle %p starting new commit...\n", handle);
		log_start_commit(journal, transaction);
		unlock_journal(journal);
		sleep_on(&journal->j_wait_transaction_locked);
		lock_journal(journal);
		goto repeat_locked;
	}

	/* 
	 * The commit code assumes that it can get enough log space
	 * without forcing a checkpoint.  This is *critical* for
	 * correctness: a checkpoint of a buffer which is also
	 * associated with a committing transaction creates a deadlock,
	 * so commit simply cannot force through checkpoints.
	 *
	 * We must therefore ensure the necessary space in the journal
	 * *before* starting to dirty potentially checkpointed buffers
	 * in the new transaction. 
	 *
	 * The worst part is, any transaction currently committing can
	 * reduce the free space arbitrarily.  Be careful to account for
	 * those buffers when checkpointing.
	 */

	needed = journal->j_max_transaction_buffers;
	if (journal->j_committing_transaction) 
		needed += journal->j_committing_transaction->t_outstanding_credits;
	
	if (log_space_left(journal) < needed) {
		jfs_debug(2, "Handle %p waiting for checkpoint...\n", handle);
		log_wait_for_space(journal, needed);
		goto repeat_locked;
	}

	/* OK, account for the buffers that this operation expects to
	 * use and add the handle to the running transaction. */

	handle->h_transaction = transaction;
	transaction->t_outstanding_credits += nblocks;
	transaction->t_updates++;
	jfs_debug(4, "Handle %p given %d credits (total %d, free %d)\n",
		  handle, nblocks, transaction->t_outstanding_credits,
		  log_space_left(journal));

	unlock_journal(journal);
	
	return 0;
}


/*
 * Obtain a new handle.  
 *
 * We make sure that the transaction can guarantee at least nblocks of
 * modified buffers in the log.  We block until the log can guarantee
 * that much space.  
 *
 * This function is visible to journal users (like ext2fs), so is not
 * called with the journal already locked.
 *
 * Return a pointer to a newly allocated handle, or NULL on failure
 */

handle_t *journal_start (journal_t *journal, int nblocks)
{
	handle_t *handle;
	int err;
	
	if (!journal)
		return ERR_PTR(-EROFS);
	
	if (current->j_handle) {
		handle = current->j_handle;
		J_ASSERT(handle->h_transaction->t_journal == journal);
		handle->h_ref++;
		return handle;
	}
	
	handle = kmalloc (sizeof (handle_t), GFP_KERNEL);
	if (!handle)
		return ERR_PTR(-ENOMEM);
	memset (handle, 0, sizeof (handle_t));

	handle->h_buffer_credits = nblocks;
	handle->h_ref = 1;
	current->j_handle = handle;
	/* Our outdated kernel doesn't have this locking support yet.  
	 * hope we don't need it. -mcomb
	 */
	// current->fs_locks++;
	
  	err = start_this_handle(journal, handle);
	if (err < 0) {
		kfree(handle);
		current->j_handle = NULL;
		// current->fs_locks--;
		return ERR_PTR(err);
	}

	return handle;
}


/*
 * journal_extend: extend buffer credits.
 *
 * Some transactions, such as large extends and truncates, can be done
 * atomically all at once or in several stages.  The operation requests
 * a credit for a number of buffer modications in advance, but can
 * extend its credit if it needs more.  
 *
 * journal_extend tries to give the running handle more buffer credits.
 * It does not guarantee that allocation: this is a best-effort only.
 * The calling process MUST be able to deal cleanly with a failure to
 * extend here.
 *
 * Return 0 on success, non-zero on failure.
 */

int journal_extend (handle_t *handle, int nblocks)
{
	transaction_t *transaction = handle->h_transaction;
	journal_t *journal = transaction->t_journal;
	int result = 1;
	int wanted;
	
	lock_journal (journal);

	/* Don't extend a locked-down transaction! */
	if (handle->h_transaction->t_state != T_RUNNING) {
		jfs_debug(3, "denied handle %p %d blocks: "
			  "transaction not running\n", handle, nblocks);
		goto error_out;
	}
	
	wanted = transaction->t_outstanding_credits + nblocks;
	
	if (wanted > journal->j_max_transaction_buffers) {
		jfs_debug(3, "denied handle %p %d blocks: "
			  "transaction too large\n", handle, nblocks);
		goto error_out;
	}

	if (wanted > log_space_left(journal)) {
		jfs_debug(3, "denied handle %p %d blocks: "
			  "insufficient log space\n", handle, nblocks);
		goto error_out;
	}
	
	handle->h_buffer_credits += nblocks;
	transaction->t_outstanding_credits += nblocks;
	result = 0;

	jfs_debug(3, "extended handle %p by %d\n", handle, nblocks);
	
error_out:
	unlock_journal (journal);
	return result;
}


/*
 * journal_restart: restart a handle for a multi-transaction filesystem
 * operation.
 *
 * If the journal_extend() call above fails to grant new buffer credits
 * to a running handle, a call to journal_restart will commit the
 * handle's transaction so far and reattach the handle to a new
 * transaction capabable of guaranteeing the requested number of
 * credits.
 */

int journal_restart(handle_t *handle, int nblocks)
{
	transaction_t *transaction = handle->h_transaction;
	journal_t *journal = transaction->t_journal;
		
	/* First unlink the handle from its current transaction, and
	 * start the commit on that. */
	
	J_ASSERT (transaction->t_updates > 0);
	J_ASSERT (current->j_handle == handle);

	transaction->t_outstanding_credits -= handle->h_buffer_credits;
	transaction->t_updates--;

	if (!transaction->t_updates)
		wake_up(&journal->j_wait_updates);

	jfs_debug(2, "restarting handle %p\n", handle);
	log_start_commit(journal, transaction);

	handle->h_buffer_credits = nblocks;
	return start_this_handle(journal, handle);
}


/* 
 * Barrier operation: establish a transaction barrier. 
 *
 * This locks out any further updates from being started, and blocks
 * until all existing updates have completed, returning only once the
 * journal is in a quiescent state with no updates running.
 *
 * The journal lock should not be held on entry.
 */

void journal_lock_updates (journal_t *journal)
{
	lock_journal(journal);
	++journal->j_barrier_count;

	/* Wait until there are no running updates */
	while (1) {
		transaction_t *transaction = journal->j_running_transaction;
		if (!transaction)
			break;
		if (!transaction->t_updates)
			break;
		
		unlock_journal(journal);
		sleep_on(&journal->j_wait_updates);
		lock_journal(journal);
	}

	unlock_journal(journal);
	down(&journal->j_barrier);
}

/*
 * Release a transaction barrier obtained with journal_lock_updates().
 *
 * Should be called without the journal lock held.
 */

void journal_unlock_updates (journal_t *journal)
{
	lock_journal(journal);

	J_ASSERT (journal->j_barrier_count != 0);
	
	up(&journal->j_barrier);
	--journal->j_barrier_count;
	wake_up(&journal->j_wait_transaction_locked);
	unlock_journal(journal);
}



static void lock_journal_bh_wait(struct buffer_head *bh, journal_t *journal)
{
repeat:
	wait_on_buffer(bh);
	lock_journal(journal);
	if (buffer_locked(bh)) {
		unlock_journal(journal);
		goto repeat;
	}
}


/*
 * journal_get_write_access: notify intent to modify a buffer for metadata
 * (not data) update.
 *
 * If the buffer is already part of the current transaction, then there
 * is nothing we need to do.  If it is already part of a prior
 * transaction which we are still committing to disk, then we need to
 * make sure that we do not overwrite the old copy: we do copy-out to
 * preserve the copy going to disk.  We also account the buffer against
 * the handle's metadata buffer credits (unless the buffer is already
 * part of the transaction, that is).
 *
 * Returns an error code or 0 on success.  
 */

int do_get_write_access (handle_t *handle, struct buffer_head *bh, 
			 int force_copy) 
{
	transaction_t *transaction = handle->h_transaction;
	journal_t *journal = transaction->t_journal;
	int error = 0;
	char *frozen_buffer = NULL;

	jfs_debug(5, "buffer_head %p, force_copy %d\n", bh, force_copy);

repeat:
	/* @@@ Need to check for errors here at some point. */

	/* The caller must make sure that we enter here with the buffer
	 * unlocked (probably by calling lock_journal_bh_wait).  If we
	 * sleep in this function, we have to wait again for the buffer
	 * to make sure it is still unlocked.  We cannot journal a
	 * buffer if somebody else may already be in the process of
	 * writing it to disk! */

	J_ASSERT (buffer_uptodate(bh));
	J_ASSERT (!buffer_locked(bh));
	
	/* The buffer is already part of this transaction if
	 * b_transaction or b_next_transaction points to it. */

	if (bh->b_transaction == transaction ||
	    bh->b_next_transaction == transaction)
		goto done;

	/* If there is already a copy-out version of this buffer, then
	 * we don't need to make another one. */

	if (bh->b_frozen_data) {
		J_ASSERT(bh->b_next_transaction == NULL);
		bh->b_next_transaction = transaction;

		J_ASSERT(handle->h_buffer_credits > 0);
		handle->h_buffer_credits--;
		goto done;
	}
	
	/* Is there data here we need to preserve? */
	
	if (bh->b_transaction && bh->b_transaction != transaction) {
		J_ASSERT (bh->b_next_transaction == NULL);
		J_ASSERT (bh->b_transaction == journal->j_committing_transaction);
		J_ASSERT (bh->b_list == BUF_JOURNAL);

		/* There is one case we have to be very careful about.
		 * If the committing transaction is currently writing
		 * this buffer out to disk and has NOT made a copy-out,
		 * then we cannot modify the buffer contents at all
		 * right now.  The essence of copy-out is that it is the
		 * extra copy, not the primary copy, which gets
		 * journaled.  If the primary copy is already going to
		 * disk then we cannot do copy-out here. */

		if (bh->b_jlist == BJ_Shadow) {
			unlock_journal(journal);
			/* commit wakes up all shadow buffers after IO */
			sleep_on(&bh->b_wait);
			lock_journal_bh_wait(bh, journal);
			goto repeat;
		}
			
		/* Only do the copy if the currently-owning transaction
		 * still needs it.  If it is on the Forget list, the
		 * committing transaction is past that stage.  The
		 * buffer had better remain locked during the kmalloc,
		 * but that should be true --- we hold the journal lock
		 * still and the buffer is already on the BUF_JOURNAL
		 * list so won't be flushed. 
		 *
		 * Subtle point, though: if this is a get_undo_access,
		 * then we will be relying on the frozen_data to contain
		 * the new value of the committed_data record after the
		 * transaction, so we HAVE to force the frozen_data copy
		 * in that case. */

		if (bh->b_jlist != BJ_Forget || force_copy) {
			if (bh->b_jlist == BJ_Data)
				J_ASSERT(test_bit(BH_QuickFree, &bh->b_state));

			if (!frozen_buffer) {
				unlock_journal(journal);
				frozen_buffer = kmalloc(bh->b_size,GFP_KERNEL);
				lock_journal_bh_wait(bh, journal);
				if (!frozen_buffer) {
					error = -ENOMEM;
					goto done;
				}
				goto repeat;
			}
			
			bh->b_frozen_data = frozen_buffer;
			frozen_buffer = NULL;
			
			memcpy (bh->b_frozen_data, bh->b_data, bh->b_size);
		}

		bh->b_next_transaction = transaction;


	}

	J_ASSERT(handle->h_buffer_credits > 0);
	handle->h_buffer_credits--;
	
	/* Finally, if the buffer is not journaled right now, we need to
	 * make sure it doesn't get written to disk before the caller
	 * actually commits the new data. */

	if (!bh->b_transaction) {
		J_ASSERT (!bh->b_next_transaction);
		bh->b_transaction = transaction;
		journal_file_buffer(bh, transaction, BJ_Reserved);
	}
	
 done:
	if (bh->b_list != BUF_JOURNAL)
		refile_buffer(bh);
	clear_bit(BH_QuickFree, &bh->b_state);

	/* If we are about to journal a buffer, then any revoke pending
           on it is no longer valid. */
	journal_cancel_revoke(handle, bh);
	
	if (frozen_buffer)
		kfree(frozen_buffer);

	return error;
}

int journal_get_write_access (handle_t *handle, struct buffer_head *bh) 
{
	transaction_t *transaction = handle->h_transaction;
	journal_t *journal = transaction->t_journal;
	int rc;
	
	/* We do not want to get caught playing with fields which the
	 * log thread also manipulates.  Make sure that the buffer
	 * completes any outstanding IO before proceeding. */
	lock_journal_bh_wait(bh, journal);
	rc = do_get_write_access(handle, bh, 0);
	unlock_journal(journal);

	return rc;
}


/*
 * When the user wants to journal a newly created buffer_head
 * (ie. getblk() returned a new buffer and we are going to populate it
 * manually rather than reading off disk), then we need to keep the
 * buffer_head locked until it has been completely filled with new
 * data.  In this case, we should be able to make the assertion that
 * the bh is not already part of an existing transaction.  
 * 
 * The buffer should already be locked by the caller by this point.
 * There is no lock ranking violation: it was a newly created,
 * unlocked buffer beforehand. */

int journal_get_create_access (handle_t *handle, struct buffer_head *bh) 
{
	transaction_t *transaction = handle->h_transaction;
	journal_t *journal = transaction->t_journal;

	jfs_debug(5, "buffer_head %p\n", bh);
	lock_journal(journal);

	/* The buffer may already belong to this transaction due to
           pre-zeroing in the filesystem's new_block code */
	J_ASSERT (bh->b_transaction == transaction || bh->b_transaction == NULL);
	J_ASSERT (bh->b_next_transaction == NULL);
	J_ASSERT (buffer_locked(bh));

	J_ASSERT(handle->h_buffer_credits > 0);
	handle->h_buffer_credits--;
	
	bh->b_transaction = transaction;
	journal_file_buffer(bh, transaction, BJ_Reserved);
	refile_buffer(bh);
	unlock_journal(journal);
	return 0;
}



/*
 * journal_get_undo_access: Notify intent to modify metadata with non-
 * rewindable consequences
 *
 * Sometimes there is a need to distinguish between metadata which has
 * been committed to disk and that which has not.  The ext3fs code uses
 * this for freeing and allocating space: we have to make sure that we
 * do not reuse freed space until the deallocation has been committed,
 * since if we overwrote that space we would make the delete
 * un-rewindable in case of a crash.
 * 
 * To deal with that, journal_get_undo_access requests write access to a
 * buffer for parts of non-rewindable operations such as delete
 * operations on the bitmaps.  The journaling code must keep a copy of
 * the buffer's contents prior to the undo_access call until such time
 * as we know that the buffer has definitely been committed to disk.
 * 
 * We never need to know which transaction the committed data is part
 * of: buffers touched here are guaranteed to be dirtied later and so
 * will be committed to a new transaction in due course, at which point
 * we can discard the old committed data pointer.
 *
 * Returns error number or 0 on success.  
 */

int journal_get_undo_access (handle_t *handle, struct buffer_head *bh)
{
	journal_t *journal = handle->h_transaction->t_journal;
	int err;

	lock_journal_bh_wait(bh, journal);

	/* Do this first --- it can drop the journal lock, so we want to
	 * make sure that obtaining the committed_data is done
	 * atomically wrt. completion of any outstanding commits. */
	err = do_get_write_access (handle, bh, 1);

	if (!bh->b_committed_data) {

		/* Copy out the current buffer contents into the
		 * preserved, committed copy. */

		bh->b_committed_data = kmalloc(bh->b_size, GFP_KERNEL);
		if (!bh->b_committed_data) {
			unlock_journal(journal);
			return -ENOMEM;
		}
		
		memcpy (bh->b_committed_data, bh->b_data, bh->b_size);
	}
	
	unlock_journal(journal);
	if (!err)
		J_ASSERT(bh->b_committed_data);
	return err;
}



/* 
 * journal_dirty_data: mark a buffer as containing dirty data which
 * needs to be flushed before we can commit the current transaction.  
 *
 * The buffer is placed on the transaction's data list and is marked as
 * belonging to the transaction.
 * 
 * Returns error number or 0 on success.  
 */

int journal_dirty_data (handle_t *handle, struct buffer_head *bh)
{
	journal_t *journal = handle->h_transaction->t_journal;
	lock_journal(journal);

	mark_buffer_dirty(bh, 0);
	
	/*
	 * What if the buffer is already part of a running transaction?
	 * 
	 * There are two cases:
	 * 1) It is part of the current running transaction.  Refile it,
	 *    just in case we have allocated it as metadata, deallocated
	 *    it, then reallocated it as data. 
	 * 2) It is part of the previous, still-committing transaction.
	 *    If all we want to do is to guarantee that the buffer will be
	 *    written to disk before this new transaction commits, then
	 *    being sure that the *previous* transaction has this same 
	 *    property is sufficient for us!  Just leave it on its old
	 *    transaction.
	 *
	 * In case (2), the buffer must not already exist as metadata
	 * --- that would violate write ordering (a transaction is free
	 * to write its data at any point, even before the previous
	 * committing transaction has committed).  The caller must
	 * never, ever allow this to happen: there's nothing we can do
	 * about it in this layer.
	 */

	if (bh->b_transaction) {
		if (bh->b_transaction != handle->h_transaction) {
			J_ASSERT (bh->b_transaction == journal->j_committing_transaction);

			/* @@@ IS THIS TRUE  ? */
			J_ASSERT (bh->b_next_transaction == NULL);

			/* Special case --- the buffer might actually
                           have been allocated and then immediately
                           deallocated in the previous, committing
                           transaction, so might still be left on that
                           transaction's metadata lists. */
			if (bh->b_jlist != BJ_Data) {
				J_ASSERT (bh->b_jlist != BJ_Shadow);
				J_ASSERT (test_and_clear_bit(BH_QuickFree, &bh->b_state));
				journal_unfile_buffer(bh);
				bh->b_transaction = NULL;
				journal_file_buffer(bh, handle->h_transaction, BJ_Data);
				refile_buffer(bh);
			}
		}
	} else {
		journal_file_buffer(bh, handle->h_transaction, BJ_Data);
		refile_buffer(bh);
	}
	
	unlock_journal(journal);
	return 0;
}


/* 
 * journal_dirty_metadata: mark a buffer as containing dirty metadata
 * which needs to be journaled as part of the current transaction.
 *
 * The buffer is placed on the transaction's metadata list and is marked
 * as belonging to the transaction.  
 *
 * Special care needs to be taken if the buffer already belongs to the
 * current committing transaction (in which case we should have frozen
 * data present for that commit).  In that case, we don't relink the
 * buffer: that only gets done when the old transaction finally
 * completes its commit.
 * 
 * Returns error number or 0 on success.  
 */

int journal_dirty_metadata (handle_t *handle, struct buffer_head *bh)
{
	transaction_t *transaction = handle->h_transaction;
	journal_t *journal = transaction->t_journal;

	jfs_debug(5, "buffer_head %p\n", bh);

	lock_journal(journal);
	mark_buffer_jdirty(bh);

	J_ASSERT(bh->b_transaction != NULL);
	
	/* 
	 * Metadata already on the current transaction list doesn't
	 * need to be filed.  Metadata on another transaction's list must
	 * be committing, and will be refiled once the commit completes:
	 * leave it alone for now. 
	 */

	if (bh->b_transaction != transaction) {
		J_ASSERT (bh->b_transaction == journal->j_committing_transaction);
		J_ASSERT (bh->b_next_transaction == transaction);
		/* And this case is illegal: we can't reuse another
		 * transaction's data buffer, ever. */
		J_ASSERT (bh->b_jlist != BJ_Data);
		goto done;
	}
	
	/* That test should have eliminated the following case: */
	J_ASSERT (bh->b_frozen_data == 0);

	journal_file_buffer (bh, handle->h_transaction, BJ_Metadata);

 done:
	unlock_journal(journal);
	return 0;
}


/* 
 * journal_release_buffer: undo a get_write_access without any buffer
 * updates, if the update decided in the end that it didn't need access.
 *
 * journal_get_write_access() can block, so it is quite possible for a
 * journaling component to decide after the write access is returned
 * that global state has changed and the update is no longer required.  */

void journal_release_buffer (handle_t *handle, struct buffer_head *bh)
{
	transaction_t *transaction = handle->h_transaction;
	journal_t *journal = transaction->t_journal;
	lock_journal(journal);

	/* If the buffer is reserved but not modified by this
	 * transaction, then it is safe to release it.  In all other
	 * cases, just leave the buffer as it is. */

	if (bh->b_jlist == BJ_Reserved && bh->b_transaction == transaction &&
	    !buffer_jdirty(bh)) {
		handle->h_buffer_credits++;
		journal_refile_buffer(bh);
	}
	
	unlock_journal(journal);
}


/* 
 * journal_forget: bforget() for potentially-journaled buffers.  We can
 * only do the bforget if there are no commits pending against the
 * buffer.  If the buffer is dirty in the current running transaction we
 * can safely unlink it. 
 *
 * The prime requirement here is never to discard any buffer after we
 * have blocked.
 */

void journal_forget (handle_t *handle, struct buffer_head *bh)
{
	journal_t * journal = handle->h_transaction->t_journal;

	J_ASSERT (!test_and_set_bit(BH_Freed, &bh->b_state));
	clear_bit(BH_Alloced, &bh->b_state);

	/* @@@ DEBUG: This is just buffer debug state: we can do this
	   without a journal lock... */
	if (handle->h_transaction->t_tid == bh->b_alloc_transaction) {
		bh->b_alloc_transaction = 0;
		set_bit(BH_QuickFree, &bh->b_state);
	}
	bh->b_free2_transaction = bh->b_free_transaction;
	bh->b_free_transaction = handle->h_transaction->t_tid;

	/* If we can't lock the journal because somebody else already
	 * has the lock, then just release the buffer: that's better
	 * than changing the semantics of bforget() to include a possible
	 * context switch. */
	if (try_lock_journal(journal))
		goto nolock;

 	if (bh->b_transaction == handle->h_transaction) {
		J_ASSERT(!bh->b_frozen_data);

		/* If we are forgetting a buffer which is already part
		 * of this transaction, then we can just drop it from
		 * the transaction immediately. */
		clear_bit(BH_Dirty, &bh->b_state);
		clear_bit(BH_JDirty, &bh->b_state);
		journal_unfile_buffer(bh);
		bh->b_transaction = 0;
	} else if (bh->b_transaction) {
		/* However, if the buffer is still owned by a prior
		 * (committing) transaction, we can't do anything with
		 * it right now. */
		unlock_journal(journal);
		brelse(bh);
		return;
	}

	J_ASSERT(!bh->b_frozen_data);
	J_ASSERT(!bh->b_committed_data);

	unlock_journal(journal);

	/* @@@ DEBUG ONLY.  Eventually we will indeed want to be able to
	 * discard forgotten buffers a bit more intelligently. */

 nolock:
	brelse(bh);
	return;

#if 0	
	/* Now we know that the buffer isn't being committed anywhere,
	 * but it might still be on a transaction's checkpoint list.  If
	 * so, we want to place it on the new transaction's forget list:
	 * on commit it will undo the old checkpoint.  Remember, we have
	 * to be able to unwind the forget() if we take a crash before
	 * the commit! */
	
	if (bh->b_cp_transaction) {
		journal_file_buffer(bh, handle->h_transaction, BJ_Forget);
		bh->b_transaction = handle->h_transaction;
		brelse(bh);
	} else
		__bforget(bh);	
#endif
}


/*
 * journal_sync_buffer: flush a potentially-journaled buffer to disk.
 *
 * Used for O_SYNC filesystem operations.  If the buffer is journaled,
 * we need to complete the O_SYNC by waiting for the transaction to
 * complete.  It is an error to call journal_sync_buffer before
 * journal_stop!
 */

void journal_sync_buffer(struct buffer_head *bh)
{
	transaction_t *transaction;
	journal_t *journal;
	long sequence;
	
	/* If the buffer isn't journaled, this is easy: just sync it to
	 * disk.  */

	if (bh->b_transaction == NULL) {
		/* If the buffer has already been journaled, then this
		 * is a noop. */
		if (bh->b_cp_transaction == NULL) 
			return;
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
		return;
	}
	
	/* Otherwise, just wait until the transaction is synced to disk. */
	transaction = bh->b_transaction;
	journal = transaction->t_journal;
	sequence = transaction->t_tid;
	
	jfs_debug(2, "requesting commit for bh %p\n", bh);
	log_start_commit (journal, transaction);
	
	while (tid_gt(sequence, journal->j_commit_sequence)) {
		wake_up(&journal->j_wait_done_commit);
		sleep_on(&journal->j_wait_done_commit);
	}
}



/*
 * All done for a particular handle.
 *
 * There is not much action needed here.  We just return any remaining
 * buffer credits to the transaction and remove the handle.  The only
 * complication is that we need to start a commit operation if the
 * filesystem is marked for synchronous update.
 *
 * journal_stop itself will not usually return an error, but it may
 * do so in unusual circumstances.  In particular, expect it to 
 * return -EIO if a journal_abort has been executed since the
 * transaction began.
 */

int journal_stop (handle_t *handle)
{
	transaction_t *transaction = handle->h_transaction;
	journal_t *journal = transaction->t_journal;
	int force_sync;
	
	if (!handle)
		return 0;
	
	J_ASSERT (transaction->t_updates > 0);
	J_ASSERT (current->j_handle == handle);
	
	if (--handle->h_ref > 0)
		return 0;

	jfs_debug(4, "Handle %p going down\n", handle);
	
	current->j_handle = NULL;
	// current->fs_locks--;
	transaction->t_outstanding_credits -= handle->h_buffer_credits;
	transaction->t_updates--;
	if (!transaction->t_updates) {
		wake_up(&journal->j_wait_updates);
		if (journal->j_barrier_count)
			wake_up(&journal->j_wait_transaction_locked);
	}

	/* 
	 * If the journal is marked SYNC, we need to set another commit
	 * going!  We also want to force a commit if the current
	 * transaction is occupying too much of the log, or if the
	 * transaction is too old now.
	 */

	force_sync = (journal->j_flags & JFS_SYNC) || handle->h_sync;
	
	if (force_sync ||
	    transaction->t_outstanding_credits > journal->j_max_transaction_buffers ||
	    time_after_eq(jiffies, transaction->t_expires)) {
		tid_t tid = transaction->t_tid;
		
		jfs_debug(2, "transaction too old, requesting commit for handle %p\n", handle);
		log_start_commit(journal, transaction);
		
		/*
		 * Special case: JFS_SYNC synchronous updates require us
		 * to wait for the commit to complete.  
		 */
		if (force_sync) 
			log_wait_commit(journal, tid);
	}
	
	kfree(handle);
	
	return 0;
}



/*
 *
 * List management code snippets: various functions for manipulating the
 * transaction buffer lists.
 *
 */

/*
 * Add a buffer to a transaction list, given the transaction's list head
 * pointer
 */

static inline void 
blist_add_buffer(struct buffer_head **list, struct buffer_head *buf)
{
	if (!*list) {
		buf->b_tnext = buf->b_tprev = buf;
		*list = buf;
	} else {
		/* Insert at the tail of the list to preserve order */
		struct buffer_head *first = *list, *last = first->b_tprev;
		buf->b_tprev = last;
		buf->b_tnext = first;
		last->b_tnext = first->b_tprev = buf;
	}
}

/* 
 * Remove a buffer from a transaction list, given the transaction's list
 * head pointer
 */

static inline void
blist_del_buffer(struct buffer_head **list, struct buffer_head *buf)
{
	if (*list == buf) {
		*list = buf->b_tnext;
		if (*list == buf)
			*list = 0;
	}
	buf->b_tprev->b_tnext = buf->b_tnext;
	buf->b_tnext->b_tprev = buf->b_tprev;
}

/* 
 * Remove a buffer from the appropriate transaction list. 
 */

void journal_unfile_buffer(struct buffer_head *buf)
{
	struct buffer_head **list = 0;
	transaction_t * transaction;

	transaction = buf->b_transaction;
	
#ifdef __SMP__
	J_ASSERT (current->lock_depth >= 0);
#endif
	J_ASSERT (transaction->t_journal->j_locked);
	J_ASSERT (buf->b_jlist < BJ_Types);
	
	if (buf->b_jlist != BJ_None)
		J_ASSERT (transaction != 0);
	
	switch (buf->b_jlist) {
	case BJ_None:
		return;
	case BJ_Data:
		list = &transaction->t_datalist;
		break;
	case BJ_Metadata:
		transaction->t_nr_buffers--;
		J_ASSERT(transaction->t_nr_buffers >= 0);
		list = &transaction->t_buffers;
		break;
	case BJ_Forget:
		list = &transaction->t_forget;
		break;
	case BJ_IO:
		list = &transaction->t_iobuf_list;
		break;
	case BJ_Shadow:
		list = &transaction->t_shadow_list;
		break;
	case BJ_LogCtl:
		list = &transaction->t_log_list;
		break;
	case BJ_Reserved:
		list = &transaction->t_reserved_list;
		break;
	}
	
	blist_del_buffer(list, buf);
	buf->b_jlist = BJ_None;
	if (buffer_jdirty(buf)) {
		set_bit(BH_Dirty, &buf->b_state);
		clear_bit(BH_JDirty, &buf->b_state);
	}
}


/* 
 * File a buffer on the given transaction list. 
 */

void journal_file_buffer(struct buffer_head *buf, 
			 transaction_t *transaction, 
			 int jlist)
{
	struct buffer_head **list = 0;
	
#ifdef __SMP__
	J_ASSERT (current->lock_depth >= 0);
#endif
	J_ASSERT (transaction->t_journal->j_locked);
	J_ASSERT (buf->b_jlist < BJ_Types);
	J_ASSERT (buf->b_transaction == transaction || buf->b_transaction ==0);

	if (buf->b_transaction) {
		if (buf->b_jlist == jlist)
			return;
		journal_unfile_buffer(buf);
	} else
		buf->b_transaction = transaction;

	switch (jlist) {
	case BJ_None:
		J_ASSERT(!buf->b_committed_data);
		J_ASSERT(!buf->b_frozen_data);
		return;
	case BJ_Data:
		list = &transaction->t_datalist;
		break;
	case BJ_Metadata:
		transaction->t_nr_buffers++;
		list = &transaction->t_buffers;
		break;
	case BJ_Forget:
		list = &transaction->t_forget;
		break;
	case BJ_IO:
		list = &transaction->t_iobuf_list;
		break;
	case BJ_Shadow:
		list = &transaction->t_shadow_list;
		break;
	case BJ_LogCtl:
		list = &transaction->t_log_list;
		break;
	case BJ_Reserved:
		list = &transaction->t_reserved_list;
		break;
	}
	
	blist_add_buffer(list, buf);
	buf->b_jlist = jlist;

	if (jlist == BJ_Metadata || jlist == BJ_Reserved || 
	    jlist == BJ_Shadow || jlist == BJ_Forget) {
		if (buffer_dirty(buf)) {
			set_bit(BH_JDirty, &buf->b_state);
			clear_bit(BH_Dirty, &buf->b_state);
		}
	}
}


/* 
 * Remove a buffer from its current buffer list in preparation for
 * dropping it from its current transaction entirely.  If the buffer has
 * already started to be used by a subsequent transaction, refile the
 * buffer on that transaction's metadata list.
 */

void journal_refile_buffer(struct buffer_head *bh)
{
#ifdef __SMP__
	J_ASSERT (current->lock_depth >= 0);
#endif
	journal_unfile_buffer(bh);

	/* If the buffer is now unused, just drop it.  If it has been
	   modified by a later transaction, add it to the new
	   transaction's metadata list. */

	bh->b_transaction = bh->b_next_transaction;
	bh->b_next_transaction = NULL;
	
	if (bh->b_transaction != NULL) {
		int tstate;
		journal_file_buffer(bh, bh->b_transaction, BJ_Metadata);
		tstate = bh->b_transaction->t_state;
		J_ASSERT(tstate == T_RUNNING);
	}
		
	/* If necessary, remove it from the global journaled
	   buffer list and replace it back on the main dirty
	   buffer list. */
	refile_buffer(bh);
}

