/*
 * linux/include/linux/ext3_jfs.h
 *
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1999
 *
 * Copyright 1998--1999 Red Hat corp --- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Ext3-specific journaling extensions.
 */

#ifndef _LINUX_EXT3_JFS_H
#define _LINUX_EXT3_JFS_H

#include <linux/fs.h>
#include <linux/jfs.h>
#include <linux/ext3_fs.h>

#define EXT3_JOURNAL(inode)	(EXT3_SB((inode)->i_sb)->s_journal)

/* Define the number of blocks we need to account to a transaction to
 * modify one block of data.
 * 
 * We may have to touch one inode, one bitmap buffer, up to three
 * indirection blocks, the group and superblock summaries, and the data
 * block to complete the transaction.  */

#define EXT3_SINGLEDATA_TRANS_BLOCKS	8

/* Define the minimum size for a transaction which modifies data.  This
 * needs to take into account the fact that we may end up modifying two
 * quota files too (one for the group, one for the user quota).  The
 * superblock only gets updated once, of course, so don't bother
 * counting that again for the quota updates. */

#define EXT3_DATA_TRANS_BLOCKS		(3 * EXT3_SINGLEDATA_TRANS_BLOCKS - 2)

/* Delete operations potentially hit one directory's namespace plus an
 * entire inode, plus arbitrary amounts of bitmap/indirection data.  Be
 * generous.  We can grow the delete transaction later if necessary. */

#define EXT3_DELETE_TRANS_BLOCKS	(2 * EXT3_DATA_TRANS_BLOCKS + 64)

/* Define an arbitrary limit for the amount of data we will anticipate
 * writing to any given transaction.  For unbounded transactions such as
 * write(2) and truncate(2) we can write more than this, but we always
 * start off at the maximum transaction size and grow the transaction
 * optimistically as we go. */

#define EXT3_MAX_TRANS_DATA		64

/* We break up a large truncate or write transaction once the handle's
 * buffer credits gets this low, we need either to extend the
 * transaction or to start a new one.  Reserve enough space here for
 * inode, bitmap, superblock, group and indirection updates for at least
 * one block, plus two quota updates.  Quota allocations are not
 * needed. */

#define EXT3_RESERVE_TRANS_BLOCKS	12

static inline int
ext3_mark_iloc_dirty(handle_t *handle, 
		     struct inode *inode,
		     struct ext3_iloc *iloc)
{
	if (handle) {
		/* the do_update_inode consumes one bh->b_count */
		iloc->bh->b_count++;
		ext3_do_update_inode(handle, inode, iloc, 0); /* @@@ ERROR */
		journal_dirty_metadata(handle, iloc->bh); /* @@@ ERROR */
		brelse(iloc->bh);
	} else
		mark_inode_dirty(inode);		
	return 0;
}

/* 
 * On success, We end up with an outstanding reference count against
 * iloc->bh.  This _must_ be cleaned up later. 
 */

static inline int
ext3_reserve_inode_write(handle_t *handle, struct inode *inode, 
			 struct ext3_iloc *iloc)
{
	int err = 0;
	if (handle) {
		err = ext3_get_inode_loc(inode, iloc); /* @@@ ERROR */
		if (!err) {
			err = journal_get_write_access(handle, iloc->bh);
			if (err) {
				brelse(iloc->bh);
				iloc->bh = NULL;
			}
		}
	}
	return err;
}

static inline int
ext3_mark_inode_dirty(handle_t *handle, 
		      struct inode *inode)
{
	struct ext3_iloc iloc;
	int err;

	err = ext3_reserve_inode_write(handle, inode, &iloc);
	if (!err)
		err = ext3_mark_iloc_dirty(handle, inode, &iloc);
	return err;
}

/* 
 * Bind an inode's backing buffer_head into this transaction, to prevent
 * it from being flushed to disk early.  Unlike
 * ext3_reserve_inode_write, this leaves behind no bh reference and
 * returns no iloc structure, so the caller needs to repeat the iloc
 * lookup to mark the inode dirty later.
 */

static inline int
ext3_pin_inode(handle_t *handle, struct inode *inode)
{
	struct ext3_iloc iloc;
	
	int err = 0;
	if (handle) {
		err = ext3_get_inode_loc(inode, &iloc);
		if (!err) {
			err = journal_get_write_access(handle, iloc.bh);
			if (!err)
				journal_dirty_metadata(handle, iloc.bh);
			brelse(iloc.bh);
		}
	}
	return err;
}

/* 
 * Wrappers for journal_start/end.
 *
 * The only special thing we need to do here is to make sure that all
 * journal_end calls result in the superblock being marked dirty, so
 * that sync() will call the filesystem's write_super callback if
 * appropriate. 
 */

static inline handle_t *ext3_journal_start (struct inode *inode, int nblocks)
{
	handle_t *handle;

	if (inode->i_sb->s_flags & MS_RDONLY)
		return ERR_PTR(-EROFS);

	handle = journal_start(EXT3_JOURNAL(inode), nblocks);
	if (handle)
		ext3_pin_inode(handle, inode);
	return handle;
}

static inline int ext3_journal_stop (handle_t *handle, struct inode *inode)
{
	int rc = journal_stop(handle);
	inode->i_sb->s_dirt = 1;
	return rc;
}

#endif	/* _LINUX_EXT3_JFS_H */
