/*
 *  linux/fs/ext3/inode.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Goal-directed block allocation by Stephen Tweedie
 * 	(sct@dcs.ed.ac.uk), 1993, 1998
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 * 	(jj@sunsite.ms.mff.cuni.cz)
 */

#include <asm/uaccess.h>
#include <asm/system.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ext3_fs.h>
#include <linux/ext3_jfs.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/mm.h>

/*
 * Called at each iput()
 */
void ext3_put_inode (struct inode * inode)
{
	ext3_discard_prealloc (inode);
}

/*
 * ext3_orphan_del() removes an unlinked or truncated inode from the list
 * of such inodes stored on disk, because it is finally being cleaned up.
 */
void ext3_orphan_del(handle_t *handle, struct inode *inode)
{
	struct list_head *prev;
	struct ext3_sb_info *sbi;
	ino_t ino_next; 
	struct ext3_iloc iloc;

	lock_super(inode->i_sb);
	if (list_empty(&inode->u.ext3_i.i_orphan)) {
		unlock_super(inode->i_sb);
		return;
	}

	ino_next = NEXT_ORPHAN(inode);
	prev = inode->u.ext3_i.i_orphan.prev;
	sbi = EXT3_SB(inode->i_sb);

	jfs_debug(4, "remove inode %ld from orphan list\n", inode->i_ino);

	list_del(&inode->u.ext3_i.i_orphan);
	INIT_LIST_HEAD(&inode->u.ext3_i.i_orphan);

	if (prev == &sbi->s_orphan) {
		jfs_debug(4, "superblock will point to %ld\n", ino_next);
		journal_get_write_access(handle, sbi->s_sbh);
		sbi->s_es->s_last_orphan = cpu_to_le32(ino_next);
		journal_dirty_metadata(handle, sbi->s_sbh);
	} else {
		struct inode *i_prev =
			list_entry(prev, struct inode, u.ext3_i.i_orphan);
		
		jfs_debug(4, "orphan inode %ld will point to %ld\n",
			  i_prev->i_ino, ino_next);
		ext3_reserve_inode_write(handle, i_prev, &iloc);
		NEXT_ORPHAN(i_prev) = ino_next;
		ext3_mark_iloc_dirty(handle, i_prev, &iloc);
	}
	ext3_reserve_inode_write(handle, inode, &iloc);
	NEXT_ORPHAN(inode) = 0;
	ext3_mark_iloc_dirty(handle, inode, &iloc);
	unlock_super(inode->i_sb);
}

/*
 * Called at the last iput() if i_nlink is zero.
 */
void ext3_delete_inode (struct inode * inode)
{
	handle_t *handle;
	struct ext3_iloc iloc;

	if (inode->i_ino == EXT3_ACL_IDX_INO ||
	    inode->i_ino == EXT3_ACL_DATA_INO)
		return;

	/* When we delete an inode, we increment its i_version. If it
	   is ever read in from disk again, it will have a different
	   i_version. */
	inode->u.ext3_i.i_version++;

	handle = ext3_journal_start(inode, EXT3_DELETE_TRANS_BLOCKS);
	if (IS_ERR(handle))
		return;
	
	if (IS_SYNC(inode))
		handle->h_sync = 1;
	inode->i_size = 0;
	if (inode->i_blocks)
		ext3_truncate (inode);

	ext3_reserve_inode_write(handle, inode, &iloc);
	ext3_orphan_del(handle, inode);
	inode->u.ext3_i.i_dtime	= CURRENT_TIME;
	ext3_mark_iloc_dirty(handle, inode, &iloc);
	ext3_free_inode(handle, inode);
	ext3_journal_stop(handle, inode);
}

#define inode_bmap(inode, nr) ((inode)->u.ext3_i.i_data_u.i_data[(nr)])

static inline int block_bmap (struct buffer_head * bh, int nr)
{
	int tmp;

	if (!bh)
		return 0;
	tmp = le32_to_cpu(((u32 *) bh->b_data)[nr]);
	brelse (bh);
	return tmp;
}

/* 
 * ext3_discard_prealloc and ext3_alloc_block are atomic wrt. the
 * superblock in the same manner as are ext3_free_blocks and
 * ext3_new_block.  We just wait on the super rather than locking it
 * here, since ext3_new_block will do the necessary locking and we
 * can't block until then.
 */
void ext3_discard_prealloc (struct inode * inode)
{
#ifdef EXT3_PREALLOCATE
	unsigned short total;

	if (inode->u.ext3_i.i_prealloc_count) {
		total = inode->u.ext3_i.i_prealloc_count;
		inode->u.ext3_i.i_prealloc_count = 0;
		ext3_free_blocks (inode, inode->u.ext3_i.i_prealloc_block, total);
	}
#endif
}

static struct buffer_head * ext3_alloc_block (handle_t *handle, 
					      struct inode * inode, 
					      unsigned long goal, int * err)
{
#ifdef EXT3FS_DEBUG
	static unsigned long alloc_hits = 0, alloc_attempts = 0;
#endif
	struct buffer_head * bh;

	wait_on_super (inode->i_sb);

#ifdef EXT3_PREALLOCATE
	if (inode->u.ext3_i.i_prealloc_count &&
	    (goal == inode->u.ext3_i.i_prealloc_block ||
	     goal + 1 == inode->u.ext3_i.i_prealloc_block))
	{		
		result = inode->u.ext3_i.i_prealloc_block++;
		inode->u.ext3_i.i_prealloc_count--;
		ext3_debug ("preallocation hit (%lu/%lu).\n",
			    ++alloc_hits, ++alloc_attempts);

		/* It doesn't matter if we block in getblk() since
		   we have already atomically allocated the block, and
		   are only clearing it now. */
		if (!(bh = getblk (inode->i_sb->s_dev, result,
				   inode->i_sb->s_blocksize))) {
			ext3_error (inode->i_sb, "ext3_alloc_block",
				    "cannot get block %lu", result);
			return 0;
		}
		if (!buffer_uptodate(bh))
			wait_on_buffer(bh);
		/* @@@ Once we start journaling data separately, this
                   needs to become dependent on the type of inode we
                   are allocating inside */
		journal_get_write_access(handle, bh);
		memset(bh->b_data, 0, inode->i_sb->s_blocksize);
		mark_buffer_uptodate(bh, 1);
		journal_dirty_metadata(handle, bh);
		brelse (bh);
	} else {
		ext3_discard_prealloc (inode);
		ext3_debug ("preallocation miss (%lu/%lu).\n",
			    alloc_hits, ++alloc_attempts);
		if (S_ISREG(inode->i_mode))
			result = ext3_new_block (inode, goal, 
				 &inode->u.ext3_i.i_prealloc_count,
				 &inode->u.ext3_i.i_prealloc_block, err);
		else
			result = ext3_new_block (handle, inode, goal, 0, 0, err);
	}
#else
	bh = ext3_new_block (handle, inode, goal, 0, 0, err);
#endif

	return bh;
}


int ext3_bmap (struct inode * inode, int block)
{
	int i;
	int addr_per_block = EXT3_ADDR_PER_BLOCK(inode->i_sb);
	int addr_per_block_bits = EXT3_ADDR_PER_BLOCK_BITS(inode->i_sb);

	if (block < 0) {
		ext3_warning (inode->i_sb, "ext3_bmap", "block < 0");
		return 0;
	}
	if (block >= EXT3_NDIR_BLOCKS + addr_per_block +
		(1 << (addr_per_block_bits * 2)) +
		((1 << (addr_per_block_bits * 2)) << addr_per_block_bits)) {
		ext3_warning (inode->i_sb, "ext3_bmap", "block > big");
		return 0;
	}
	if (block < EXT3_NDIR_BLOCKS)
		return inode_bmap (inode, block);
	block -= EXT3_NDIR_BLOCKS;
	if (block < addr_per_block) {
		i = inode_bmap (inode, EXT3_IND_BLOCK);
		if (!i)
			return 0;
		return block_bmap (bread (inode->i_dev, i,
					  inode->i_sb->s_blocksize), block);
	}
	block -= addr_per_block;
	if (block < (1 << (addr_per_block_bits * 2))) {
		i = inode_bmap (inode, EXT3_DIND_BLOCK);
		if (!i)
			return 0;
		i = block_bmap (bread (inode->i_dev, i,
				       inode->i_sb->s_blocksize),
				block >> addr_per_block_bits);
		if (!i)
			return 0;
		return block_bmap (bread (inode->i_dev, i,
					  inode->i_sb->s_blocksize),
				   block & (addr_per_block - 1));
	}
	block -= (1 << (addr_per_block_bits * 2));
	i = inode_bmap (inode, EXT3_TIND_BLOCK);
	if (!i)
		return 0;
	i = block_bmap (bread (inode->i_dev, i, inode->i_sb->s_blocksize),
			block >> (addr_per_block_bits * 2));
	if (!i)
		return 0;
	i = block_bmap (bread (inode->i_dev, i, inode->i_sb->s_blocksize),
			(block >> addr_per_block_bits) & (addr_per_block - 1));
	if (!i)
		return 0;
	return block_bmap (bread (inode->i_dev, i, inode->i_sb->s_blocksize),
			   block & (addr_per_block - 1));
}

static struct buffer_head * inode_getblk (handle_t *handle,
					  struct inode * inode, int nr,
					  int create, int new_block, int * err)
{
	u32 * p;
	int tmp, goal = 0;
	struct buffer_head * result;
	int blocks = inode->i_sb->s_blocksize / 512;

	p = inode->u.ext3_i.i_data_u.i_data + nr;
repeat:
	tmp = *p;
	if (tmp) {
		struct buffer_head * result = getblk (inode->i_dev, tmp, inode->i_sb->s_blocksize);
		if (tmp == *p)
			return result;
		brelse (result);
		goto repeat;
	}
	*err = -EFBIG;
	if (!create)
		return NULL;

	if (inode->u.ext3_i.i_next_alloc_block == new_block)
		goal = inode->u.ext3_i.i_next_alloc_goal;

	ext3_debug ("hint = %d,", goal);

	if (!goal) {
		for (tmp = nr - 1; tmp >= 0; tmp--) {
			if (inode->u.ext3_i.i_data_u.i_data[tmp]) {
				goal = inode->u.ext3_i.i_data_u.i_data[tmp];
				break;
			}
		}
		if (!goal)
			goal = (inode->u.ext3_i.i_block_group * 
				EXT3_BLOCKS_PER_GROUP(inode->i_sb)) +
			       le32_to_cpu(inode->i_sb->u.ext3_sb.s_es->s_first_data_block);
	}

	ext3_debug ("goal = %d.\n", goal);

	result = ext3_alloc_block (handle, inode, goal, err);
	if (!result)
		return NULL;
	tmp = result->b_blocknr;
	
	if (*p) {
		ext3_free_blocks (handle, inode, tmp, 1);
		brelse (result);
		goto repeat;
	}
	*p = tmp;
	inode->u.ext3_i.i_next_alloc_block = new_block;
	inode->u.ext3_i.i_next_alloc_goal = tmp;
	inode->i_ctime = CURRENT_TIME;
	inode->i_blocks += blocks;
	if (IS_SYNC(inode) || inode->u.ext3_i.i_osync)
		handle->h_sync = 1;
	ext3_mark_inode_dirty(handle, inode);
	return result;
}

static struct buffer_head * block_getblk (handle_t *handle, 
					  struct inode * inode,
					  struct buffer_head * bh, int nr,
					  int create, int blocksize, 
					  int new_block, int * err)
{
	int tmp, goal = 0;
	u32 * p;
	struct buffer_head * result;
	int blocks = inode->i_sb->s_blocksize / 512;
	
	if (!bh)
		return NULL;
	if (!buffer_uptodate(bh)) {
		ll_rw_block (READ, 1, &bh);
		wait_on_buffer (bh);
		if (!buffer_uptodate(bh)) {
			brelse (bh);
			return NULL;
		}
	}
	p = (u32 *) bh->b_data + nr;
repeat:
	tmp = le32_to_cpu(*p);
	if (tmp) {
		result = getblk (bh->b_dev, tmp, blocksize);
		if (tmp == le32_to_cpu(*p)) {
			brelse (bh);
			return result;
		}
		brelse (result);
		goto repeat;
	}
	*err = -EFBIG;
	if (!create) {
		brelse (bh);
		return NULL;
	}

	if (inode->u.ext3_i.i_next_alloc_block == new_block)
		goal = inode->u.ext3_i.i_next_alloc_goal;
	if (!goal) {
		for (tmp = nr - 1; tmp >= 0; tmp--) {
			if (le32_to_cpu(((u32 *) bh->b_data)[tmp])) {
				goal = le32_to_cpu(((u32 *)bh->b_data)[tmp]);
				break;
			}
		}
		if (!goal)
			goal = bh->b_blocknr;
	}
	journal_get_write_access(handle, bh);
	result = ext3_alloc_block (handle, inode, goal, err);
	if (!result) {
		journal_dirty_metadata(handle, bh); /* release bh */
		brelse (bh);
		return NULL;
	}
	tmp = result->b_blocknr;

	if (le32_to_cpu(*p)) {
		journal_dirty_metadata(handle, bh); /* release bh */

		/* @@@ Major danger here: we are using up more and more
		 * buffer credits against this transaction by undoing
		 * the allocation when we get a collision here.  We need
		 * to be able to extend the transaction if possible, and
		 * return a retry failure code if we can't. */
		ext3_free_blocks (handle, inode, tmp, 1);
		brelse (result);
		goto repeat;
	}
	*p = le32_to_cpu(tmp);
	journal_dirty_metadata(handle, bh);
	if (IS_SYNC(inode) || inode->u.ext3_i.i_osync)
		handle->h_sync = 1;
	inode->i_ctime = CURRENT_TIME;
	inode->i_blocks += blocks;
	inode->u.ext3_i.i_next_alloc_block = new_block;
	inode->u.ext3_i.i_next_alloc_goal = tmp;
	ext3_mark_inode_dirty(handle, inode);
	brelse (bh);
	return result;
}

struct buffer_head * ext3_getblk (handle_t *handle, 
				  struct inode * inode, long block,
				  int create, int * err)
{
	struct buffer_head * bh;
	unsigned long b;
	unsigned long addr_per_block = EXT3_ADDR_PER_BLOCK(inode->i_sb);
	int addr_per_block_bits = EXT3_ADDR_PER_BLOCK_BITS(inode->i_sb);

	*err = -EIO;
	if (block < 0) {
		ext3_warning (inode->i_sb, "ext3_getblk", "block < 0");
		return NULL;
	}
	if (block > EXT3_NDIR_BLOCKS + addr_per_block +
		(1 << (addr_per_block_bits * 2)) +
		((1 << (addr_per_block_bits * 2)) << addr_per_block_bits)) {
		ext3_warning (inode->i_sb, "ext3_getblk", "block > big");
		return NULL;
	}
	/*
	 * If this is a sequential block allocation, set the next_alloc_block
	 * to this block now so that all the indblock and data block
	 * allocations use the same goal zone
	 */

	ext3_debug ("block %lu, next %lu, goal %lu.\n", block, 
		    inode->u.ext3_i.i_next_alloc_block,
		    inode->u.ext3_i.i_next_alloc_goal);

	if (block == inode->u.ext3_i.i_next_alloc_block + 1) {
		inode->u.ext3_i.i_next_alloc_block++;
		inode->u.ext3_i.i_next_alloc_goal++;
	}

	*err = -ENOSPC;
	b = block;
	if (block < EXT3_NDIR_BLOCKS)
		return inode_getblk (handle, inode, block, create, b, err);
	block -= EXT3_NDIR_BLOCKS;
	if (block < addr_per_block) {
		bh = inode_getblk (handle, inode, EXT3_IND_BLOCK, create, b, err);
		return block_getblk (handle, inode, bh, block, create,
				     inode->i_sb->s_blocksize, b, err);
	}
	block -= addr_per_block;
	if (block < (1 << (addr_per_block_bits * 2))) {
		bh = inode_getblk (handle, inode, EXT3_DIND_BLOCK, create, b, err);
		bh = block_getblk (handle, inode, bh, block >> addr_per_block_bits,
				   create, inode->i_sb->s_blocksize, b, err);
		return block_getblk (handle, inode, bh, block & (addr_per_block - 1),
				     create, inode->i_sb->s_blocksize, b, err);
	}
	block -= (1 << (addr_per_block_bits * 2));
	bh = inode_getblk (handle, inode, EXT3_TIND_BLOCK, create, b, err);
	bh = block_getblk (handle, inode, bh, block >> (addr_per_block_bits * 2),
			   create, inode->i_sb->s_blocksize, b, err);
	bh = block_getblk (handle, inode, bh, (block >> addr_per_block_bits) & (addr_per_block - 1),
			   create, inode->i_sb->s_blocksize, b, err);
	return block_getblk (handle, inode, bh, block & (addr_per_block - 1), create,
			     inode->i_sb->s_blocksize, b, err);
}

/* For ext3_bread, we need a transaction handle iff create is true. */

struct buffer_head * ext3_bread (handle_t *handle, 
				 struct inode * inode, int block, 
				 int create, int *err)
{
	struct buffer_head * bh;
	int prev_blocks;
	
	prev_blocks = inode->i_blocks;
	
	bh = ext3_getblk (handle, inode, block, create, err);
	if (!bh)
		return bh;
	
	/*
	 * If the inode has grown, and this is a directory, then perform
	 * preallocation of a few more blocks to try to keep directory
	 * fragmentation down.
	 */
	if (create && 
	    S_ISDIR(inode->i_mode) && 
	    inode->i_blocks > prev_blocks &&
	    EXT3_HAS_COMPAT_FEATURE(inode->i_sb,
				    EXT3_FEATURE_COMPAT_DIR_PREALLOC)) {
		int i;
		struct buffer_head *tmp_bh;
		
		for (i = 1;
		     i < EXT3_SB(inode->i_sb)->s_es->s_prealloc_dir_blocks;
		     i++) {
			/* 
			 * ext3_getblk will zero out the contents of the
			 * directory for us
			 */
			tmp_bh = ext3_getblk(handle, inode, block+i, create, err);
			if (!tmp_bh) {
				brelse (bh);
				return 0;
			}
			brelse (tmp_bh);
		}
	}
	
	if (buffer_uptodate(bh))
		return bh;
	ll_rw_block (READ, 1, &bh);
	wait_on_buffer (bh);
	if (buffer_uptodate(bh))
		return bh;
	brelse (bh);
	*err = -EIO;
	return NULL;
}

/* 
 * ext3_get_inode_loc returns with an extra refcount against the
 * inode's underlying buffer_head on success. 
 */

int ext3_get_inode_loc (struct inode *inode, struct ext3_iloc *iloc)
{
	struct buffer_head *bh = 0;
	unsigned long block;
	unsigned long block_group;
	unsigned long group_desc;
	unsigned long desc;
	unsigned long offset;
	struct ext3_group_desc * gdp;
		
	if ((inode->i_ino != EXT3_ROOT_INO && inode->i_ino != EXT3_ACL_IDX_INO &&
	     inode->i_ino != EXT3_ACL_DATA_INO && inode->i_ino != EXT3_JOURNAL_INO &&
	     inode->i_ino < EXT3_FIRST_INO(inode->i_sb)) ||
	    inode->i_ino > le32_to_cpu(inode->i_sb->u.ext3_sb.s_es->s_inodes_count)) {
		ext3_error (inode->i_sb, "ext3_get_inode_loc",
			    "bad inode number: %lu", inode->i_ino);
		goto bad_inode;
	}
	block_group = (inode->i_ino - 1) / EXT3_INODES_PER_GROUP(inode->i_sb);
	if (block_group >= inode->i_sb->u.ext3_sb.s_groups_count) {
		ext3_error (inode->i_sb, "ext3_get_inode_loc",
			    "group >= groups count");
		goto bad_inode;
	}
	group_desc = block_group >> EXT3_DESC_PER_BLOCK_BITS(inode->i_sb);
	desc = block_group & (EXT3_DESC_PER_BLOCK(inode->i_sb) - 1);
	bh = inode->i_sb->u.ext3_sb.s_group_desc[group_desc];
	if (!bh) {
		ext3_error (inode->i_sb, "ext3_get_inode_loc",
			    "Descriptor not loaded");
		goto bad_inode;
	}

	gdp = (struct ext3_group_desc *) bh->b_data;
	/*
	 * Figure out the offset within the block group inode table
	 */
	offset = ((inode->i_ino - 1) % EXT3_INODES_PER_GROUP(inode->i_sb)) *
		EXT3_INODE_SIZE(inode->i_sb);
	block = le32_to_cpu(gdp[desc].bg_inode_table) +
		(offset >> EXT3_BLOCK_SIZE_BITS(inode->i_sb));
	if (!(bh = bread (inode->i_dev, block, inode->i_sb->s_blocksize))) {
		ext3_error (inode->i_sb, "ext3_get_inode_loc",
			    "unable to read inode block - "
			    "inode=%lu, block=%lu", inode->i_ino, block);
		goto bad_inode;
	}
	offset &= (EXT3_BLOCK_SIZE(inode->i_sb) - 1);

	iloc->bh = bh;
	iloc->raw_inode = (struct ext3_inode *) (bh->b_data + offset);
	iloc->block_group = block_group;
	
	return 0;
	
 bad_inode:
	return -EIO;
}

void ext3_read_inode (struct inode * inode)
{
	struct ext3_iloc iloc;
	int block;
	
	if(ext3_get_inode_loc(inode, &iloc))
		goto bad_inode;

	inode->i_mode = le16_to_cpu(iloc.raw_inode->i_mode);
	inode->i_uid = le16_to_cpu(iloc.raw_inode->i_uid);
	inode->i_gid = le16_to_cpu(iloc.raw_inode->i_gid);
	inode->i_nlink = le16_to_cpu(iloc.raw_inode->i_links_count);
	inode->i_size = le32_to_cpu(iloc.raw_inode->i_size);
	inode->i_atime = le32_to_cpu(iloc.raw_inode->i_atime);
	inode->i_ctime = le32_to_cpu(iloc.raw_inode->i_ctime);
	inode->i_mtime = le32_to_cpu(iloc.raw_inode->i_mtime);
	inode->u.ext3_i.i_dtime = le32_to_cpu(iloc.raw_inode->i_dtime);
	inode->i_blksize = PAGE_SIZE;	/* This is the optimal IO size (for stat), not the fs block size */
	inode->i_blocks = le32_to_cpu(iloc.raw_inode->i_blocks);
	inode->i_version = ++global_event;
	inode->u.ext3_i.i_new_inode = 0;
	inode->u.ext3_i.i_flags = le32_to_cpu(iloc.raw_inode->i_flags);
	inode->u.ext3_i.i_faddr = le32_to_cpu(iloc.raw_inode->i_faddr);
	inode->u.ext3_i.i_frag_no = iloc.raw_inode->i_frag;
	inode->u.ext3_i.i_frag_size = iloc.raw_inode->i_fsize;
	inode->u.ext3_i.i_osync = 0;
	inode->u.ext3_i.i_file_acl = le32_to_cpu(iloc.raw_inode->i_file_acl);
	if (S_ISDIR(inode->i_mode))
		inode->u.ext3_i.i_dir_acl = le32_to_cpu(iloc.raw_inode->i_dir_acl);
	else {
		inode->u.ext3_i.i_dir_acl = 0;
		inode->u.ext3_i.i_high_size =
			le32_to_cpu(iloc.raw_inode->i_size_high);
#if BITS_PER_LONG < 64
		if (iloc.raw_inode->i_size_high)
			inode->i_size = (__u32)-1;
#else
		inode->i_size |= ((__u64)le32_to_cpu(iloc.raw_inode->i_size_high))
			<< 32;
#endif
	}
	inode->u.ext3_i.i_disksize = inode->i_size;
	inode->u.ext3_i.i_version = le32_to_cpu(iloc.raw_inode->i_version);
	inode->i_generation = inode->u.ext3_i.i_version;
	inode->u.ext3_i.i_block_group = iloc.block_group;
	inode->u.ext3_i.i_next_alloc_block = 0;
	inode->u.ext3_i.i_next_alloc_goal = 0;
	INIT_LIST_HEAD(&inode->u.ext3_i.i_orphan);
	if (inode->u.ext3_i.i_prealloc_count)
		ext3_error (inode->i_sb, "ext3_read_inode",
			    "New inode has non-zero prealloc count!");
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		inode->i_rdev = to_kdev_t(le32_to_cpu(iloc.raw_inode->i_block[0]));
	else if (S_ISLNK(inode->i_mode) && !inode->i_blocks)
		for (block = 0; block < EXT3_N_BLOCKS; block++)
			inode->u.ext3_i.i_data_u.i_data[block] = iloc.raw_inode->i_block[block];
	else for (block = 0; block < EXT3_N_BLOCKS; block++)
		inode->u.ext3_i.i_data_u.i_data[block] = le32_to_cpu(iloc.raw_inode->i_block[block]);
	brelse (iloc.bh);
	inode->i_op = NULL;
	if (inode->i_ino == EXT3_ACL_IDX_INO ||
	    inode->i_ino == EXT3_ACL_DATA_INO)
		/* Nothing to do */ ;
	else if (S_ISREG(inode->i_mode))
		inode->i_op = &ext3_file_inode_operations;
	else if (S_ISDIR(inode->i_mode))
		inode->i_op = &ext3_dir_inode_operations;
	else if (S_ISLNK(inode->i_mode))
		inode->i_op = &ext3_symlink_inode_operations;
	else if (S_ISCHR(inode->i_mode))
		inode->i_op = &chrdev_inode_operations;
	else if (S_ISBLK(inode->i_mode))
		inode->i_op = &blkdev_inode_operations;
	else if (S_ISFIFO(inode->i_mode))
		init_fifo(inode);
	inode->i_attr_flags = 0;
	if (inode->u.ext3_i.i_flags & EXT3_SYNC_FL) {
		inode->i_attr_flags |= ATTR_FLAG_SYNCRONOUS;
		inode->i_flags |= MS_SYNCHRONOUS;
	}
	if (inode->u.ext3_i.i_flags & EXT3_APPEND_FL) {
		inode->i_attr_flags |= ATTR_FLAG_APPEND;
		inode->i_flags |= S_APPEND;
	}
	if (inode->u.ext3_i.i_flags & EXT3_IMMUTABLE_FL) {
		inode->i_attr_flags |= ATTR_FLAG_IMMUTABLE;
		inode->i_flags |= S_IMMUTABLE;
	}
	if (inode->u.ext3_i.i_flags & EXT3_NOATIME_FL) {
		inode->i_attr_flags |= ATTR_FLAG_NOATIME;
		inode->i_flags |= MS_NOATIME;
	}
	return;
	
bad_inode:
	make_bad_inode(inode);
	return;
}

/*
 * Post the struct inode info into an on-disk inode location in the
 * buffer-cache.  This gobbles the caller's reference to the
 * buffer_head in the inode location struct.  
 */

int ext3_do_update_inode(handle_t *handle, 
			 struct inode *inode, 
			 struct ext3_iloc *iloc,
			 int do_sync)
{
	struct ext3_inode *raw_inode = iloc->raw_inode;
	struct buffer_head *bh = iloc->bh;
	int block;
	int err = 0;

	if (handle)
		journal_get_write_access(handle, bh);
	raw_inode->i_mode = cpu_to_le16(inode->i_mode);
	raw_inode->i_uid = cpu_to_le16(inode->i_uid);
	raw_inode->i_gid = cpu_to_le16(inode->i_gid);
	raw_inode->i_links_count = cpu_to_le16(inode->i_nlink);
	raw_inode->i_size = cpu_to_le32(inode->u.ext3_i.i_disksize);
	raw_inode->i_atime = cpu_to_le32(inode->i_atime);
	raw_inode->i_ctime = cpu_to_le32(inode->i_ctime);
	raw_inode->i_mtime = cpu_to_le32(inode->i_mtime);
	raw_inode->i_blocks = cpu_to_le32(inode->i_blocks);
	raw_inode->i_dtime = cpu_to_le32(inode->u.ext3_i.i_dtime);
	raw_inode->i_flags = cpu_to_le32(inode->u.ext3_i.i_flags);
	raw_inode->i_faddr = cpu_to_le32(inode->u.ext3_i.i_faddr);
	raw_inode->i_frag = inode->u.ext3_i.i_frag_no;
	raw_inode->i_fsize = inode->u.ext3_i.i_frag_size;
	raw_inode->i_file_acl = cpu_to_le32(inode->u.ext3_i.i_file_acl);
	if (S_ISDIR(inode->i_mode))
		raw_inode->i_dir_acl = cpu_to_le32(inode->u.ext3_i.i_dir_acl);
	else { 
#if BITS_PER_LONG < 64
		raw_inode->i_size_high =
			cpu_to_le32(inode->u.ext3_i.i_high_size);
#else
		raw_inode->i_size_high = cpu_to_le32(inode->u.ext3_i.i_disksize >> 32);
#endif
	}
	raw_inode->i_version = cpu_to_le32(inode->u.ext3_i.i_version);
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		raw_inode->i_block[0] = cpu_to_le32(kdev_t_to_nr(inode->i_rdev));
	else if (S_ISLNK(inode->i_mode) && !inode->i_blocks)
		for (block = 0; block < EXT3_N_BLOCKS; block++)
			raw_inode->i_block[block] = inode->u.ext3_i.i_data_u.i_data[block];
	else for (block = 0; block < EXT3_N_BLOCKS; block++)
		raw_inode->i_block[block] = cpu_to_le32(inode->u.ext3_i.i_data_u.i_data[block]);

	if (handle) {
		journal_dirty_metadata(handle, bh);
		if (do_sync) 
			handle->h_sync = 1;
	} else {
		mark_buffer_dirty(bh, 1);
		if (do_sync) {
			ll_rw_block (WRITE, 1, &bh);
			wait_on_buffer (bh);
			if (buffer_req(bh) && !buffer_uptodate(bh)) {
				printk ("IO error syncing ext3 inode ["
					"%s:%08lx]\n",
					bdevname(inode->i_dev), inode->i_ino);
				err = -EIO;
			}
		}
	}
		
	brelse (bh);
	return err;
}

/* ext3_write_inode is only used for flushing in-core un-journaled
 * dirty inodes from the inode cache to the buffer cache when
 * reclaiming memory or during a sync().
 *
 * We don't actually have to flush the resulting buffer cache entry to
 * disk: for sync(), that will happen during the write_super() or the
 * sync_buffers() (depending on whether or not we perform the write as
 * a journaled operation or not).
 *
 * CAUTION: This routine has the potential to interact badly with
 * journaling.  An existing transaction may call get_empty_inode() in
 * the VFS, and that may in turn result in a call to ext3_write_inode if
 * there are too many dirty inodes already in the inode cache.  We
 * cannot use the existing transaction handle here: it may not be for
 * the correct filesystem.
 *
 * Neither can we start a new transaction: that may deadlock as we
 * already have a lock on the inode by this point, and starting a new
 * transaction may require us to wait for the existing one to complete
 * (which our inode lock may obstruct).
 *
 * However, a dirty ext3 inode can only result from an update to a
 * timestamp or something equally benign: any inode update which
 * requires journaling will be journaled immediately rather than being
 * flushed asynchronously from the inode cache.  We are saved: it is
 * safe to simply update the buffer cache directly: if the buffer is
 * already being journaled then the existing journaling write ordering
 * will do the right thing, but if it is not, then it can safely be
 * flushed to disk immediately.
 */

void ext3_write_inode(struct inode * inode)
{
	struct ext3_iloc iloc;
	struct buffer_head *bh;
	journal_t *journal = NULL;
	int err;

	err = ext3_get_inode_loc(inode, &iloc);
	if (err)
		return;

	bh = iloc.bh;
	if (bh->b_transaction != NULL) {
		journal = bh->b_transaction->t_journal;
		/* We aren't going to mess with the journal, but let's
                   just be safe and make sure that nobody else is doing
                   something critical with the buffer right now either */
		lock_journal(journal);
	}
	
	ext3_do_update_inode(NULL, inode, &iloc, 0);

	if (journal)
		unlock_journal(journal);
	return;
}

int ext3_notify_change(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	int		retval;
	unsigned int	flags;
	handle_t	*handle = 0;
	struct ext3_iloc iloc;
	
	retval = -EPERM;
	if (iattr->ia_valid & ATTR_ATTR_FLAG) {
		if ((iattr->ia_attr_flags &
		    (ATTR_FLAG_APPEND | ATTR_FLAG_IMMUTABLE)) ^
		    (inode->u.ext3_i.i_flags &
		     (EXT3_APPEND_FL | EXT3_IMMUTABLE_FL))) {
			if (!capable(CAP_LINUX_IMMUTABLE))
				goto out;
		}
	}
	if (iattr->ia_valid & ATTR_UID) {
		if ((current->fsuid != inode->i_uid) && !capable(CAP_FOWNER))
			goto out;
	}
	if (iattr->ia_valid & ATTR_SIZE) {
		off_t size = iattr->ia_size;
		unsigned long limit = current->rlim[RLIMIT_FSIZE].rlim_cur;

		if (size < 0)
			return -EINVAL;
#if BITS_PER_LONG == 64	
		if (size > ext3_max_sizes[EXT3_BLOCK_SIZE_BITS(inode->i_sb)])
			return -EFBIG;
#endif
 		if (limit < RLIM_INFINITY && size > limit) {
			send_sig(SIGXFSZ, current, 0);
			return -EFBIG;
		}

#if BITS_PER_LONG == 64	
		if (size >> 33) {
			struct super_block *sb = inode->i_sb;
			struct ext3_super_block *es = sb->u.ext3_sb.s_es;

			if (!EXT3_HAS_RO_COMPAT_FEATURE(sb,
					EXT3_FEATURE_RO_COMPAT_LARGE_FILE)) {
				struct buffer_head *bh = sb->u.ext3_sb.s_sbh;
				
				handle = ext3_journal_start(inode, 1);
				if (IS_ERR(handle))
					return PTR_ERR(handle);

				/* If this is the first large file
				 * created, add a flag to the superblock */
				ext3_update_fs_rev(sb);
				EXT3_SET_RO_COMPAT_FEATURE(sb,
					EXT3_FEATURE_RO_COMPAT_LARGE_FILE);
				journal_dirty_metadata(handle, bh); /*@@@err*/
				ext3_journal_stop(handle, inode);
			}
		}
#endif
	}
	

	retval = inode_change_ok(inode, iattr);
	if (retval != 0)
		goto out;

	/* Notify-change transaction.  The maximum number of buffers
	 * required is one. */

	handle = ext3_journal_start(inode, 1);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	retval = ext3_reserve_inode_write(handle, inode, &iloc);
	if (retval) 			
		goto out_stop;
	
	inode_setattr(inode, iattr);

	if (iattr->ia_valid & ATTR_ATTR_FLAG) {
		flags = iattr->ia_attr_flags;
		if (flags & ATTR_FLAG_SYNCRONOUS) {
			inode->i_flags |= MS_SYNCHRONOUS;
			inode->u.ext3_i.i_flags = EXT3_SYNC_FL;
		} else {
			inode->i_flags &= ~MS_SYNCHRONOUS;
			inode->u.ext3_i.i_flags &= ~EXT3_SYNC_FL;
		}
		if (flags & ATTR_FLAG_NOATIME) {
			inode->i_flags |= MS_NOATIME;
			inode->u.ext3_i.i_flags = EXT3_NOATIME_FL;
		} else {
			inode->i_flags &= ~MS_NOATIME;
			inode->u.ext3_i.i_flags &= ~EXT3_NOATIME_FL;
		}
		if (flags & ATTR_FLAG_APPEND) {
			inode->i_flags |= S_APPEND;
			inode->u.ext3_i.i_flags = EXT3_APPEND_FL;
		} else {
			inode->i_flags &= ~S_APPEND;
			inode->u.ext3_i.i_flags &= ~EXT3_APPEND_FL;
		}
		if (flags & ATTR_FLAG_IMMUTABLE) {
			inode->i_flags |= S_IMMUTABLE;
			inode->u.ext3_i.i_flags = EXT3_IMMUTABLE_FL;
		} else {
			inode->i_flags &= ~S_IMMUTABLE;
			inode->u.ext3_i.i_flags &= ~EXT3_IMMUTABLE_FL;
		}
	}
	
	retval = ext3_mark_iloc_dirty(handle, inode, &iloc);

out_stop:
	ext3_journal_stop(handle, inode);
out:
	return retval;
}

/*
 * Change the data-journaling status on an individual inode. 
 *
 * This is especially intended for use by the quota system to allow the
 * quota files to be data-journaled.
 *
 * The caller MUST NOT be running a journal handle already, since we
 * will have to wait for all running handles to complete as part of this
 * operation.  
 */

int ext3_change_inode_journal_flag(struct inode *inode, int val)
{
	journal_t *journal;
	handle_t *handle;

	/* We have to be very careful here: changing a data block's
	 * journaling status dynamically is dangerous.  If we write a
	 * data block to the journal, change the status and then delete
	 * that block, we risk forgetting to revoke the old log record
	 * from the journal and so a subsequent replay can corrupt data.
	 * So, first we make sure that the journal is empty and that
	 * nobody is changing anything. */

	journal=EXT3_JOURNAL(inode);
	journal_lock_updates(journal);
	journal_flush(journal);

	/* OK, there are no updates running now, and all cached data is
	 * synced to disk.  We are now in a completely consistent state
	 * which doesn't have anything in the journal, and we know that
	 * no filesystem updates are running, so it is safe to modify
	 * the inode's in-core data-journaling state flag now. */
	
	if (val)
		inode->u.ext3_i.i_flags |= EXT3_JOURNAL_DATA_FL;
	else
		inode->u.ext3_i.i_flags &= ~EXT3_JOURNAL_DATA_FL;

	journal_unlock_updates(journal);

	/* Finally we can mark the inode as dirty. */

	handle = ext3_journal_start(inode, 1);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	ext3_mark_inode_dirty(handle, inode);
	handle->h_sync = 1;
	ext3_journal_stop(handle, inode);

	return 0;
}
