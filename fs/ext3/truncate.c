/*
 *  linux/fs/ext3/truncate.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/truncate.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *
 *  General cleanup and race fixes, wsh, 1998
 */

/*
 * Real random numbers for secure rm added 94/02/18
 * Idea from Pierre del Perugia <delperug@gla.ecoledoc.ibp.fr>
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/ext3_fs.h>
#include <linux/ext3_jfs.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/locks.h>
#include <linux/string.h>

#if 0

/*
 * Secure deletion currently doesn't work. It interacts very badly
 * with buffers shared with memory mappings, and for that reason
 * can't be done in the truncate() routines. It should instead be
 * done separately in "release()" before calling the truncate routines
 * that will release the actual file blocks.
 *
 *		Linus
 */
static int ext3_secrm_seed = 152;	/* Random generator base */

#define RANDOM_INT (ext3_secrm_seed = ext3_secrm_seed * 69069l +1)
#endif

/*
 * Macros to return the block number for the inode size and offset.
 * Currently we always hold the inode semaphore during truncate, so
 * there's no need to test for changes during the operation.
 */
#define DIRECT_BLOCK(inode) \
	((inode->i_size + inode->i_sb->s_blocksize - 1) / \
			  inode->i_sb->s_blocksize)
#define INDIRECT_BLOCK(inode,offset) ((int)DIRECT_BLOCK(inode) - offset)
#define DINDIRECT_BLOCK(inode,offset) \
	(INDIRECT_BLOCK(inode,offset) / addr_per_block)
#define TINDIRECT_BLOCK(inode,offset) \
	(INDIRECT_BLOCK(inode,offset) / (addr_per_block*addr_per_block))

/*
 * Truncate has the most races in the whole filesystem: coding it is
 * a pain in the a**. Especially as I don't do any locking...
 *
 * The code may look a bit weird, but that's just because I've tried to
 * handle things like file-size changes in a somewhat graceful manner.
 * Anyway, truncating a file at the same time somebody else writes to it
 * is likely to result in pretty weird behaviour...
 *
 * The new code handles normal truncates (size = 0) as well as the more
 * general case (size = XXX). I hope.
 *
 *
 * Truncate operations have been rewritten to avoid various races. The
 * previous code was allowing blocking operations to precede a call to
 * bforget(), possible allowing the buffer to be used again.
 *
 * We now ensure that b_count == 1 before calling bforget() and that the
 * parent buffer (if any) is unlocked before clearing the block pointer.
 * The operations are always performed in this order:
 *	(1) Make sure that the parent buffer is unlocked.
 *	(2) Use find_buffer() to find the block buffer without blocking,
 *	    and set 'retry' if the buffer is locked or b_count > 1.
 *	(3) Clear the block pointer in the parent (buffer or inode).
 *	(4) Update the inode block count and mark the inode dirty.
 *	(5) Forget the block buffer, if any. This call won't block, as
 *	    we know the buffer is unlocked from (2).
 *	(6) If the block pointer is in a (parent) buffer, mark the buffer
 *	    dirty. (Note that this can block on a loop device.)
 *	(7) Accumulate the blocks to free and/or update the block bitmap.
 *	    (This operation will frequently block.)
 *
 * The requirement that parent buffers be unlocked follows from the general
 * principle of not modifying a buffer that may be undergoing I/O. With the
 * the present kernels there's no problem with modifying a locked inode, as
 * the I_DIRTY bit is cleared before setting I_LOCK.
 *		-- WSH, 1998
 */

/* The ext3 forget function must perform a revoke if we are freeing data
 * which has been journaled.  Metadata (eg. indirect blocks) must be
 * revoked in all cases. 
 *
 * "bh" may be NULL: a metadata block may have been freed from memory
 * but there may still be a record of it in the journal, and that record
 * still needs to be revoked.
 */

static void ext3_forget(handle_t *handle, int is_metadata,
			struct inode *inode, struct buffer_head *bh,
			int blocknr)
{
	int err;

	jfs_debug(4, "forgetting bh %p: is_metadata = %d, mode %o, "
		  "data mode %lx\n",
		  bh, is_metadata, inode->i_mode,
		  test_opt3(inode->i_sb, DATA_FLAGS));
	
	/* Never use the revoke function if we are doing full data
	 * journaling: there is no need to, and a V1 superblock won't
	 * support it.  Otherwise, only skip the revoke on un-journaled
	 * data blocks. */

	if (test_opt3(inode->i_sb, DATA_FLAGS) == EXT3_MOUNT_JOURNAL_DATA ||
	    (!is_metadata && !ext3_should_journal_data(inode))) {
		if (bh)
			journal_forget(handle, bh);
		return;
	}
	    
	err = journal_revoke(handle, blocknr, bh);
	if (err == -ENOMEM)
		ext3_abort(inode->i_sb, __FUNCTION__,
			   "out of memory during revoke");
	J_ASSERT (!err);
}


/* 
 * The journaling doesn't have to break the rules above, as long as we
 * do a journal_get_write_access() on the appropriate indirect blocks
 * before entering the find_buffer().   
 * 
 * However, there are some new complications.  truncate transactions can
 * be complex and unbounded in length, so we need to be able to restart
 * the transaction at a conventient checkpoint to make sure we don't
 * overflow the journal.
 *
 * start_transaction gets us a new handle for a truncate transaction,
 * and extend_transaction tries to extend the existing one a bit.  If
 * extend fails, we need to propagate the failure up and restart the
 * transaction in the top-level truncate loop. --sct 
 */

static handle_t *start_transaction(struct inode *inode) 
{
	long needed;
	needed = inode->i_blocks;
	if (needed > EXT3_MAX_TRANS_DATA) 
		needed = EXT3_MAX_TRANS_DATA;

	return ext3_journal_start(inode, EXT3_DATA_TRANS_BLOCKS + needed);
}

static int extend_transaction(handle_t *handle, struct inode *inode)
{
	long needed;
	
	if (handle->h_buffer_credits > EXT3_RESERVE_TRANS_BLOCKS)
		return 0;
	needed = inode->i_blocks;
	if (needed > EXT3_MAX_TRANS_DATA) 
		needed = EXT3_MAX_TRANS_DATA;
	if (!journal_extend(handle, EXT3_RESERVE_TRANS_BLOCKS + needed))
		return 0;

	/* If we are going to restart the handle, then we'd better flush
	 * any pending inode changes to disk first. */
	ext3_mark_inode_dirty(handle, inode);
	
	jfs_debug(2, "restarting handle %p\n", handle);
	journal_restart(handle, EXT3_DATA_TRANS_BLOCKS + needed);
	ext3_pin_inode(handle, inode);
	return 1;
}

	

/*
 * Check whether any of the slots in an indirect block are
 * still in use, and if not free the block.
 */
static int check_block_empty(handle_t *handle, 
			     struct inode *inode, struct buffer_head *bh,
			     u32 *p, struct buffer_head *ind_bh)
{
	int addr_per_block = EXT3_ADDR_PER_BLOCK(inode->i_sb);
	u32 * ind = (u32 *) bh->b_data;
	int i, retry;

	if (ind_bh)
		journal_get_write_access(handle, ind_bh);
	
	/* Make sure both buffers are unlocked */
	do {
		retry = 0;
		if (buffer_locked(bh)) {
			__wait_on_buffer(bh);
			retry = 1;
		}
		if (ind_bh && buffer_locked(ind_bh)) {
			__wait_on_buffer(ind_bh);
			retry = 1;
		}
	} while (retry);

	for (i = 0; i < addr_per_block; i++)
		if (*(ind++))
			goto in_use;

	if (!journal_is_buffer_shared(bh)) {
		int tmp;
		if (ind_bh)
			tmp = le32_to_cpu(*p);
		else
			tmp = *p;
		*p = 0;
		inode->i_blocks -= (inode->i_sb->s_blocksize / 512);

		/*
		 * Forget the buffer, then mark the parent buffer dirty.
		 */
		ext3_forget(handle, 1, inode, bh, bh->b_blocknr);
		if (ind_bh)
			journal_dirty_metadata(handle, ind_bh);
		ext3_free_blocks (handle, inode, tmp, 1);
		ext3_mark_inode_dirty(handle, inode);
		goto out;
	}
	jfs_debug(4, "retry\n");
	retry = 1;

in_use:
	brelse (bh);
	if (ind_bh)
		journal_release_buffer(handle, ind_bh);
	
out:
	return retry;
}

static int trunc_direct (handle_t *handle, struct inode * inode)
{
	struct buffer_head * bh;
	int i, retry = 0;
	unsigned long block_to_free = 0, free_count = 0;
	int blocks = inode->i_sb->s_blocksize / 512;
	int direct_block = DIRECT_BLOCK(inode);
	int dirty = 0;
	
	for (i = direct_block ; i < EXT3_NDIR_BLOCKS ; i++) {
		u32 * p = inode->u.ext3_i.i_data_u.i_data + i;
		int tmp = *p;

		if (!tmp)
			continue;

		if (handle->h_buffer_credits <= EXT3_RESERVE_TRANS_BLOCKS) {
			if (free_count) {
				ext3_free_blocks (handle, inode, 
						  block_to_free, free_count);
				free_count = 0;
			}
			extend_transaction(handle, inode);
		}
		
		bh = find_buffer(inode->i_dev, tmp, inode->i_sb->s_blocksize);
		if (bh) {
			bh->b_count++;
			if(journal_is_buffer_shared(bh) || buffer_locked(bh)) {
				brelse(bh);
				retry = 1;
				continue;
			}
		}
		
		*p = 0;
		inode->i_blocks -= blocks;
		dirty = 1;
		ext3_forget(handle, 0, inode, bh, tmp);
		
		/* accumulate blocks to free if they're contiguous */
		if (free_count == 0)
			goto free_this;
		else if (block_to_free == tmp - free_count)
			free_count++;
		else {
			ext3_free_blocks (handle, inode, block_to_free, free_count);
		free_this:
			block_to_free = tmp;
			free_count = 1;
		}
	}
	if (free_count > 0)
		ext3_free_blocks (handle, inode, block_to_free, free_count);

	if (dirty)
		ext3_mark_inode_dirty(handle, inode);
	return retry;
}

static int trunc_indirect (handle_t *handle,
			   struct inode * inode, int offset, u32 * p,
			   struct buffer_head *dind_bh)
{
	struct buffer_head * ind_bh;
	int i, tmp, retry = 0;
	unsigned long block_to_free = 0, free_count = 0;
	int indirect_block, addr_per_block, blocks;
	int dirty = 0;
	
	tmp = dind_bh ? le32_to_cpu(*p) : *p;
	if (!tmp)
		return 0;
	ind_bh = bread (inode->i_dev, tmp, inode->i_sb->s_blocksize);
	if (tmp != (dind_bh ? le32_to_cpu(*p) : *p)) {
		brelse (ind_bh);
		return 1;
	}

	/* A read failure? Report error and clear slot (should be rare). */ 
	if (!ind_bh) {
		ext3_error(inode->i_sb, "trunc_indirect",
			"Read failure, inode=%ld, block=%d",
			inode->i_ino, tmp);
		if (dind_bh) {
			journal_get_write_access(handle, dind_bh);
			*p = 0;
			journal_dirty_metadata(handle, dind_bh);
		} else {
			*p = 0;
			ext3_mark_inode_dirty(handle, inode);
		}
		return 0;
	}

	journal_get_write_access(handle, ind_bh);

	blocks = inode->i_sb->s_blocksize / 512;
	addr_per_block = EXT3_ADDR_PER_BLOCK(inode->i_sb);
	indirect_block = INDIRECT_BLOCK(inode, offset);
	if (indirect_block < 0)
		indirect_block = 0;
	for (i = indirect_block ; i < addr_per_block ; i++) {
		u32 * ind = i + (u32 *) ind_bh->b_data;
		struct buffer_head * bh;

		wait_on_buffer(ind_bh);
		tmp = le32_to_cpu(*ind);
		if (!tmp)
			continue;

		if (handle->h_buffer_credits <= EXT3_RESERVE_TRANS_BLOCKS) {
			if (free_count) {
				ext3_free_blocks (handle, inode, 
						  block_to_free, free_count);
				free_count = 0;
			}
			if (extend_transaction(handle, inode))
				journal_get_write_access(handle, ind_bh);
		}
		
		/*
		 * Use find_buffer so we don't block here.
		 */
		bh = find_buffer(inode->i_dev, tmp, inode->i_sb->s_blocksize);
		if (bh) {
			bh->b_count++;
			if (journal_is_buffer_shared(bh) || buffer_locked(bh)) {
				brelse (bh);
				retry = 1;
				continue;
			}
		}

		*ind = 0;
		inode->i_blocks -= blocks;
		dirty = 1;
		ext3_forget(handle, 0, inode, bh, tmp);

		/* accumulate blocks to free if they're contiguous */
		if (free_count == 0)
			goto free_this;
		else if (block_to_free == tmp - free_count)
			free_count++;
		else {
			ext3_free_blocks (handle, inode, block_to_free, free_count);
		free_this:
			block_to_free = tmp;
			free_count = 1;
		}
	}
	if (free_count > 0)
		ext3_free_blocks (handle, inode, block_to_free, free_count);

	if (dirty) {
		journal_dirty_metadata(handle, ind_bh);
		ext3_mark_inode_dirty(handle, inode);
	}
	
	/*
	 * Check the block and dispose of the ind_bh buffer.
	 */
	retry |= check_block_empty(handle, inode, ind_bh, p, dind_bh);

	return retry;
}

static int trunc_dindirect (handle_t *handle,
			    struct inode * inode, int offset, u32 * p,
			    struct buffer_head * tind_bh)
{
	struct buffer_head * dind_bh;
	int i, tmp, retry = 0;
	int dindirect_block, addr_per_block;

	tmp = tind_bh ? le32_to_cpu(*p) : *p;
	if (!tmp)
		return 0;
	dind_bh = bread (inode->i_dev, tmp, inode->i_sb->s_blocksize);
	if (tmp != (tind_bh ? le32_to_cpu(*p) : *p)) {
		brelse (dind_bh);
		return 1;
	}
	/* A read failure? Report error and clear slot (should be rare). */ 
	if (!dind_bh) {
		ext3_error(inode->i_sb, "trunc_dindirect",
			"Read failure, inode=%ld, block=%d",
			inode->i_ino, tmp);
		if (tind_bh) {
			journal_get_write_access(handle, tind_bh);
			*p = 0;
			journal_dirty_metadata(handle, tind_bh);
		} else {
			*p = 0;
			ext3_mark_inode_dirty(handle, inode);
		}
		return 0;
	}

	addr_per_block = EXT3_ADDR_PER_BLOCK(inode->i_sb);
	dindirect_block = DINDIRECT_BLOCK(inode, offset);
	if (dindirect_block < 0)
		dindirect_block = 0;
	for (i = dindirect_block ; i < addr_per_block ; i++) {
		u32 * dind = i + (u32 *) dind_bh->b_data;

		extend_transaction(handle, inode);
		retry |= trunc_indirect(handle, inode,
					offset + (i * addr_per_block),
					dind, dind_bh);
	}
	/*
	 * Check the block and dispose of the dind_bh buffer.
	 */
	retry |= check_block_empty(handle, inode, dind_bh, p, tind_bh);

	return retry;
}

static int trunc_tindirect (handle_t *handle, struct inode * inode)
{
	u32 * p = inode->u.ext3_i.i_data_u.i_data + EXT3_TIND_BLOCK;
	struct buffer_head * tind_bh;
	int i, tmp, retry = 0;
	int tindirect_block, addr_per_block, offset;

	if (!(tmp = *p))
		return 0;
	tind_bh = bread (inode->i_dev, tmp, inode->i_sb->s_blocksize);
	if (tmp != *p) {
		brelse (tind_bh);
		return 1;
	}
	/* A read failure? Report error and clear slot (should be rare). */ 
	if (!tind_bh) {
		ext3_error(inode->i_sb, "trunc_tindirect",
			"Read failure, inode=%ld, block=%d",
			inode->i_ino, tmp);
		*p = 0;
		ext3_mark_inode_dirty(handle, inode);
		return 0;
	}

	addr_per_block = EXT3_ADDR_PER_BLOCK(inode->i_sb);
	offset = EXT3_NDIR_BLOCKS + addr_per_block + 
		(addr_per_block * addr_per_block);
	tindirect_block = TINDIRECT_BLOCK(inode, offset);
	if (tindirect_block < 0)
		tindirect_block = 0;
	for (i = tindirect_block ; i < addr_per_block ; i++) {
		u32 * tind = i + (u32 *) tind_bh->b_data;

		extend_transaction(handle, inode);
		retry |= trunc_dindirect(handle, inode,
				offset + (i * addr_per_block * addr_per_block),
				tind, tind_bh);
	}
	/*
	 * Check the block and dispose of the tind_bh buffer.
	 */
	retry |= check_block_empty(handle, inode, tind_bh, p, NULL);

	return retry;
}

void ext3_truncate (struct inode * inode)
{
	int err, offset, retry;
	handle_t *handle;
	
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	    S_ISLNK(inode->i_mode)))
		return;
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		return;
	
	ext3_discard_prealloc(inode);

	handle = start_transaction(inode);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	
	/* Add inode to orphan list, so that if this truncate spans multiple
	 * transactions, and we crash and don't recover the last transaction
	 * we will resume the truncate when the filesystem recovers.
	 */
	ext3_orphan_add(handle, inode);

	/* The orphan list will now protect us from a crash before the
	 * truncate completes, so it is finally safe to propagate the
	 * new inode size (held for now in i_size) into the on-disk
	 * inode. */
	inode->u.ext3_i.i_disksize = inode->i_size;

	while (1) {
		retry = trunc_direct(handle, inode);
		retry |= trunc_indirect (handle, inode, 
				EXT3_IND_BLOCK,
				(u32 *) &inode->u.ext3_i.i_data_u.i_data[EXT3_IND_BLOCK],
				NULL);
		retry |= trunc_dindirect (handle, inode,
				EXT3_IND_BLOCK+EXT3_ADDR_PER_BLOCK(inode->i_sb),
				(u32 *)&inode->u.ext3_i.i_data_u.i_data[EXT3_DIND_BLOCK],
				NULL);
		retry |= trunc_tindirect (handle, inode);
		if (!retry)
			break;
		
		ext3_mark_inode_dirty(handle, inode);
		ext3_journal_stop(handle, inode);
		current->counter = 0;
		run_task_queue(&tq_disk);
		current->policy |= SCHED_YIELD;
		schedule ();
		handle = start_transaction(inode);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			handle = NULL;
			goto done;
		}
	}
	/*
	 * If the file is not being truncated to a block boundary, the
	 * contents of the partial block following the end of the file
	 * must be zeroed in case it ever becomes accessible again due
	 * to subsequent file growth.
	 */
	offset = inode->i_size & (inode->i_sb->s_blocksize - 1);
	if (offset) {
		struct buffer_head * bh;
		bh = ext3_bread (NULL, inode,
				 inode->i_size >> EXT3_BLOCK_SIZE_BITS(inode->i_sb),
				 0, &err);
		if (bh) {
			if (ext3_should_journal_data(inode))
				journal_get_write_access(handle, bh);

			memset (bh->b_data + offset, 0,
				inode->i_sb->s_blocksize - offset);

			if (ext3_should_journal_data(inode))
				journal_dirty_metadata(handle, bh);
			else if (test_opt3(inode->i_sb, DATA_FLAGS) == EXT3_MOUNT_ORDERED_DATA)
				journal_dirty_data(handle, bh);
			else 
				mark_buffer_dirty(bh, 0);
				
			brelse (bh);
		}
	}
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;

done:
	if (inode->i_nlink)
		ext3_orphan_del(handle, inode);
	ext3_mark_inode_dirty(handle, inode);
	if (handle)
		ext3_journal_stop(handle, inode);
}
