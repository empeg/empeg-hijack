/*
 *  linux/fs/ext3/namei.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *  Directory entry file type support and forward compatibility hooks
 *  	for B-tree directories by Theodore Ts'o (tytso@mit.edu), 1998
 */

#include <asm/uaccess.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ext3_fs.h>
#include <linux/ext3_jfs.h>
#include <linux/fcntl.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/quotaops.h>


/*
 * define how far ahead to read directories while searching them.
 */
#define NAMEI_RA_CHUNKS  2
#define NAMEI_RA_BLOCKS  4
#define NAMEI_RA_SIZE        (NAMEI_RA_CHUNKS * NAMEI_RA_BLOCKS)
#define NAMEI_RA_INDEX(c,b)  (((c) * NAMEI_RA_BLOCKS) + (b))

/*
 * NOTE! unlike strncmp, ext3_match returns 1 for success, 0 for failure.
 *
 * `len <= EXT3_NAME_LEN' is guaranteed by caller.
 * `de != NULL' is guaranteed by caller.
 */
static inline int ext3_match (int len, const char * const name,
		       struct ext3_dir_entry_2 * de)
{
	if (len != de->name_len)
		return 0;
	if (!de->inode)
		return 0;
	return !memcmp(name, de->name, len);
}

/*
 *	ext3_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 */
static struct buffer_head * ext3_find_entry (struct inode * dir,
					     const char * const name, int namelen,
					     struct ext3_dir_entry_2 ** res_dir)
{
	struct super_block * sb;
	struct buffer_head * bh_use[NAMEI_RA_SIZE];
	struct buffer_head * bh_read[NAMEI_RA_SIZE];
	unsigned long offset;
	int block, toread, i, err;

	*res_dir = NULL;
	sb = dir->i_sb;

	if (namelen > EXT3_NAME_LEN)
		return NULL;

	memset (bh_use, 0, sizeof (bh_use));
	toread = 0;
	for (block = 0; block < NAMEI_RA_SIZE; ++block) {
		struct buffer_head * bh;

		if ((block << EXT3_BLOCK_SIZE_BITS (sb)) >= dir->i_size)
			break;
		bh = ext3_getblk (NULL, dir, block, 0, &err);
		bh_use[block] = bh;
		if (bh && !buffer_uptodate(bh))
			bh_read[toread++] = bh;
	}

	for (block = 0, offset = 0; offset < dir->i_size; block++) {
		struct buffer_head * bh;
		struct ext3_dir_entry_2 * de;
		char * dlimit;

		if ((block % NAMEI_RA_BLOCKS) == 0 && toread) {
			ll_rw_block (READ, toread, bh_read);
			toread = 0;
		}
		bh = bh_use[block % NAMEI_RA_SIZE];
		if (!bh) {
#if 0
			ext3_error (sb, "ext3_find_entry",
				    "directory #%lu contains a hole at offset %lu",
				    dir->i_ino, offset);
#endif
			offset += sb->s_blocksize;
			continue;
		}
		wait_on_buffer (bh);
		if (!buffer_uptodate(bh)) {
			/*
			 * read error: all bets are off
			 */
			break;
		}

		de = (struct ext3_dir_entry_2 *) bh->b_data;
		dlimit = bh->b_data + sb->s_blocksize;
		while ((char *) de < dlimit) {
			/* this code is executed quadratically often */
			/* do minimal checking `by hand' */
			int de_len;

			if ((char *) de + namelen <= dlimit &&
			    ext3_match (namelen, name, de)) {
				/* found a match -
				   just to be sure, do a full check */
				if (!ext3_check_dir_entry("ext3_find_entry",
							  dir, de, bh, offset))
					goto failure;
				for (i = 0; i < NAMEI_RA_SIZE; ++i) {
					if (bh_use[i] != bh)
						brelse (bh_use[i]);
				}
				*res_dir = de;
				return bh;
			}
			/* prevent looping on a bad block */
			de_len = le16_to_cpu(de->rec_len);
			if (de_len <= 0)
				goto failure;
			offset += de_len;
			de = (struct ext3_dir_entry_2 *)
				((char *) de + de_len);
		}

		brelse (bh);
		if (((block + NAMEI_RA_SIZE) << EXT3_BLOCK_SIZE_BITS (sb)) >=
		    dir->i_size)
			bh = NULL;
		else
			bh = ext3_getblk (NULL, dir, block + NAMEI_RA_SIZE, 0, &err);
		bh_use[block % NAMEI_RA_SIZE] = bh;
		if (bh && !buffer_uptodate(bh))
			bh_read[toread++] = bh;
	}

failure:
	for (i = 0; i < NAMEI_RA_SIZE; ++i)
		brelse (bh_use[i]);
	return NULL;
}

struct dentry *ext3_lookup(struct inode * dir, struct dentry *dentry)
{
	struct inode * inode;
	struct ext3_dir_entry_2 * de;
	struct buffer_head * bh;

	if (dentry->d_name.len > EXT3_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	bh = ext3_find_entry (dir, dentry->d_name.name, dentry->d_name.len, &de);
	inode = NULL;
	if (bh) {
		unsigned long ino = le32_to_cpu(de->inode);
		brelse (bh);
		inode = iget(dir->i_sb, ino);

		if (!inode)
			return ERR_PTR(-EACCES);
	}
	d_add(dentry, inode);
	return NULL;
}

/*
 *	ext3_add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as ext3_find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
static struct buffer_head * ext3_add_entry (handle_t * handle,
					    struct inode * dir,
					    const char * name, int namelen,
					    struct ext3_dir_entry_2 ** res_dir,
					    int *err)
{
	unsigned long offset;
	unsigned short rec_len;
	struct buffer_head * bh;
	struct ext3_dir_entry_2 * de, * de1;
	struct super_block * sb;

	*err = -EINVAL;
	*res_dir = NULL;
	if (!dir || !dir->i_nlink)
		return NULL;
	sb = dir->i_sb;

	if (!namelen)
		return NULL;
	/*
	 * Is this a busy deleted directory?  Can't create new files if so
	 */
	if (dir->i_size == 0)
	{
		*err = -ENOENT;
		return NULL;
	}
	bh = ext3_bread (0, dir, 0, 0, err);
	if (!bh)
		return NULL;
	rec_len = EXT3_DIR_REC_LEN(namelen);
	offset = 0;
	de = (struct ext3_dir_entry_2 *) bh->b_data;
	*err = -ENOSPC;
	while (1) {
		if ((char *)de >= sb->s_blocksize + bh->b_data) {
			brelse (bh);
			bh = NULL;
			bh = ext3_bread (handle, dir, offset >> EXT3_BLOCK_SIZE_BITS(sb), 1, err);
			if (!bh)
				return NULL;
			if (dir->i_size <= offset) {
				if (dir->i_size == 0) {
					*err = -ENOENT;
					return NULL;
				}

				ext3_debug ("creating next block\n");

				journal_get_write_access(handle, bh);
				de = (struct ext3_dir_entry_2 *) bh->b_data;
				de->inode = 0;
				de->rec_len = le16_to_cpu(sb->s_blocksize);
				dir->i_size = offset + sb->s_blocksize;
				dir->u.ext3_i.i_disksize = dir->i_size;
				dir->u.ext3_i.i_flags &= ~EXT3_BTREE_FL;
				ext3_mark_inode_dirty(handle, dir);
				/* Just keep the buffer reserved for now. */
				goto got_buffer;
			} else {

				ext3_debug ("skipping to next block\n");

				de = (struct ext3_dir_entry_2 *) bh->b_data;
			}
		}
		if (!ext3_check_dir_entry ("ext3_add_entry", dir, de, bh,
					   offset)) {
			*err = -ENOENT;
			brelse (bh);
			return NULL;
		}
		if (ext3_match (namelen, name, de)) {
				*err = -EEXIST;
				brelse (bh);
				return NULL;
		}
		if ((le32_to_cpu(de->inode) == 0 && le16_to_cpu(de->rec_len) >= rec_len) ||
		    (le16_to_cpu(de->rec_len) >= EXT3_DIR_REC_LEN(de->name_len) + rec_len)) {
			journal_get_write_access(handle, bh);
got_buffer:		/* By now the buffer is marked for journaling */
			offset += le16_to_cpu(de->rec_len);
			if (le32_to_cpu(de->inode)) {
				de1 = (struct ext3_dir_entry_2 *) ((char *) de +
					EXT3_DIR_REC_LEN(de->name_len));
				de1->rec_len = cpu_to_le16(le16_to_cpu(de->rec_len) -
					EXT3_DIR_REC_LEN(de->name_len));
				de->rec_len = cpu_to_le16(EXT3_DIR_REC_LEN(de->name_len));
				de = de1;
			}
			de->inode = 0;
			de->name_len = namelen;
			de->file_type = 0;
			memcpy (de->name, name, namelen);
			/*
			 * XXX shouldn't update any times until successful
			 * completion of syscall, but too many callers depend
			 * on this.
			 *
			 * XXX similarly, too many callers depend on
			 * ext3_new_inode() setting the times, but error
			 * recovery deletes the inode, so the worst that can
			 * happen is that the times are slightly out of date
			 * and/or different from the directory change time.
			 */
			dir->i_mtime = dir->i_ctime = CURRENT_TIME;
			dir->u.ext3_i.i_flags &= ~EXT3_BTREE_FL;
			dir->i_version = ++global_event;
			ext3_mark_inode_dirty(handle, dir);
			journal_dirty_metadata(handle, bh);
			*res_dir = de;
			*err = 0;
			return bh;
		}
		offset += le16_to_cpu(de->rec_len);
		de = (struct ext3_dir_entry_2 *) ((char *) de + le16_to_cpu(de->rec_len));
	}
	brelse (bh);
	return NULL;
}

/*
 * ext3_delete_entry deletes a directory entry by merging it with the
 * previous entry
 */
static int ext3_delete_entry (handle_t *handle, 
			      struct ext3_dir_entry_2 * dir,
			      struct buffer_head * bh)
{
	struct ext3_dir_entry_2 * de, * pde;
	int i;

	i = 0;
	pde = NULL;
	de = (struct ext3_dir_entry_2 *) bh->b_data;
	while (i < bh->b_size) {
		if (!ext3_check_dir_entry ("ext3_delete_entry", NULL, 
					   de, bh, i))
			return -EIO;
		if (de == dir)  {
			journal_get_write_access(handle, bh);
			if (pde)
				pde->rec_len =
					cpu_to_le16(le16_to_cpu(pde->rec_len) +
						    le16_to_cpu(dir->rec_len));
			else
				dir->inode = 0;
			journal_dirty_metadata(handle, bh);
			return 0;
		}
		i += le16_to_cpu(de->rec_len);
		pde = de;
		de = (struct ext3_dir_entry_2 *) ((char *) de + le16_to_cpu(de->rec_len));
	}
	return -ENOENT;
}

/*
 * By the time this is called, we already have created
 * the directory cache entry for the new file, but it
 * is so far negative - it has no inode.
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate(). 
 */
int ext3_create (struct inode * dir, struct dentry * dentry, int mode)
{
	struct inode * inode;
	struct buffer_head * bh;
	struct ext3_dir_entry_2 * de;
	handle_t *handle;
	int err = -EIO;

	handle = ext3_journal_start(dir, EXT3_DATA_TRANS_BLOCKS + 3);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	/*
	 * N.B. Several error exits in ext3_new_inode don't set err.
	 */
	inode = ext3_new_inode (handle, dir, mode, &err);
	if (!inode)
		goto out;

	err = 0;
	
	inode->i_op = &ext3_file_inode_operations;
	inode->i_mode = mode;
	ext3_mark_inode_dirty(handle, inode); /* @@@ Error? */
	bh = ext3_add_entry (handle, dir, dentry->d_name.name, dentry->d_name.len, &de, &err);
	if (!bh) {
		inode->i_nlink--;
		ext3_mark_inode_dirty(handle, inode);
		iput (inode);
		goto out;
	}
	de->inode = cpu_to_le32(inode->i_ino);
	if (EXT3_HAS_INCOMPAT_FEATURE(dir->i_sb,
				      EXT3_FEATURE_INCOMPAT_FILETYPE))
		de->file_type = EXT3_FT_REG_FILE;
	dir->i_version = ++global_event;
	if (IS_SYNC(dir))
		handle->h_sync = 1;
	brelse (bh);
	d_instantiate(dentry, inode);

out:
	ext3_journal_stop(handle, dir);
	return err;
}

int ext3_mknod (struct inode * dir, struct dentry *dentry, int mode, int rdev)
{
	struct inode * inode;
	struct buffer_head * bh;
	struct ext3_dir_entry_2 * de;
	int err = -EIO;
	handle_t *handle = 0;

	handle = ext3_journal_start(dir, EXT3_DATA_TRANS_BLOCKS + 3);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	
	inode = ext3_new_inode (handle, dir, mode, &err);
	if (!inode)
		goto out_stop;

	inode->i_uid = current->fsuid;
	inode->i_mode = mode;
	inode->i_op = NULL;
	bh = ext3_add_entry (handle, dir, dentry->d_name.name, dentry->d_name.len, &de, &err);
	if (!bh)
		goto out_no_entry;
	de->inode = cpu_to_le32(inode->i_ino);
	dir->i_version = ++global_event;
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &ext3_file_inode_operations;
		if (EXT3_HAS_INCOMPAT_FEATURE(dir->i_sb,
					      EXT3_FEATURE_INCOMPAT_FILETYPE))
			de->file_type = EXT3_FT_REG_FILE;
	} else if (S_ISSOCK(inode->i_mode)) {
		if (EXT3_HAS_INCOMPAT_FEATURE(dir->i_sb,
					      EXT3_FEATURE_INCOMPAT_FILETYPE))
			de->file_type = EXT3_FT_SOCK;
	} else if (S_ISCHR(inode->i_mode)) {
		inode->i_op = &chrdev_inode_operations;
		if (EXT3_HAS_INCOMPAT_FEATURE(dir->i_sb,
					      EXT3_FEATURE_INCOMPAT_FILETYPE))
			de->file_type = EXT3_FT_CHRDEV;
	} else if (S_ISBLK(inode->i_mode)) {
		inode->i_op = &blkdev_inode_operations;
		if (EXT3_HAS_INCOMPAT_FEATURE(dir->i_sb,
					      EXT3_FEATURE_INCOMPAT_FILETYPE))
			de->file_type = EXT3_FT_BLKDEV;
	} else if (S_ISFIFO(inode->i_mode))  {
		init_fifo(inode);
		if (EXT3_HAS_INCOMPAT_FEATURE(dir->i_sb,
					      EXT3_FEATURE_INCOMPAT_FILETYPE))
			de->file_type = EXT3_FT_FIFO;
	}
	if (S_ISBLK(mode) || S_ISCHR(mode))
		inode->i_rdev = to_kdev_t(rdev);
	ext3_mark_inode_dirty(handle, inode);
	journal_dirty_metadata(handle, bh);
	if (IS_SYNC(dir))
		handle->h_sync = 1;
	d_instantiate(dentry, inode);
	brelse(bh);
out_stop:
	ext3_journal_stop(handle, dir);
	return err;

out_no_entry:
	inode->i_nlink--;
	ext3_mark_inode_dirty(handle, inode);
	iput(inode);
	goto out_stop;
}

int ext3_mkdir(struct inode * dir, struct dentry * dentry, int mode)
{
	struct inode * inode;
	struct buffer_head * bh, * dir_block;
	struct ext3_dir_entry_2 * de;
	handle_t *handle = 0;
	int err;

	err = -EMLINK;
	if (dir->i_nlink >= EXT3_LINK_MAX)
		goto out;

	handle = ext3_journal_start(dir, EXT3_DATA_TRANS_BLOCKS + 4);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	err = -EIO;
	inode = ext3_new_inode (handle, dir, S_IFDIR, &err);
	if (!inode)
		goto out_stop;

	inode->i_op = &ext3_dir_inode_operations;
	inode->i_size = inode->u.ext3_i.i_disksize = inode->i_sb->s_blocksize;
	inode->i_blocks = 0;	
	dir_block = ext3_bread (handle, inode, 0, 1, &err);
	if (!dir_block) {
		inode->i_nlink--; /* is this nlink == 0? */
		ext3_mark_inode_dirty(handle, inode);
		iput (inode);
		goto out_stop;
	}
	journal_get_write_access(handle, dir_block);
	de = (struct ext3_dir_entry_2 *) dir_block->b_data;
	de->inode = cpu_to_le32(inode->i_ino);
	de->name_len = 1;
	de->rec_len = cpu_to_le16(EXT3_DIR_REC_LEN(de->name_len));
	strcpy (de->name, ".");
	if (EXT3_HAS_INCOMPAT_FEATURE(dir->i_sb,
				      EXT3_FEATURE_INCOMPAT_FILETYPE))
		de->file_type = EXT3_FT_DIR;
	de = (struct ext3_dir_entry_2 *) ((char *) de + le16_to_cpu(de->rec_len));
	de->inode = cpu_to_le32(dir->i_ino);
	de->rec_len = cpu_to_le16(inode->i_sb->s_blocksize - EXT3_DIR_REC_LEN(1));
	de->name_len = 2;
	strcpy (de->name, "..");
	if (EXT3_HAS_INCOMPAT_FEATURE(dir->i_sb,
				      EXT3_FEATURE_INCOMPAT_FILETYPE))
		de->file_type = EXT3_FT_DIR;
	inode->i_nlink = 2;
	journal_dirty_metadata(handle, dir_block);
	brelse (dir_block);
	inode->i_mode = S_IFDIR | (mode & (S_IRWXUGO|S_ISVTX) & ~current->fs->umask);
	if (dir->i_mode & S_ISGID)
		inode->i_mode |= S_ISGID;
	ext3_mark_inode_dirty(handle, inode);
	bh = ext3_add_entry (handle, dir, dentry->d_name.name, dentry->d_name.len, &de, &err);
	if (!bh)
		goto out_no_entry;
	de->inode = cpu_to_le32(inode->i_ino);
	if (EXT3_HAS_INCOMPAT_FEATURE(dir->i_sb,
				      EXT3_FEATURE_INCOMPAT_FILETYPE))
		de->file_type = EXT3_FT_DIR;
	dir->i_version = ++global_event;
	journal_dirty_metadata(handle, bh);
	dir->i_nlink++;
	dir->u.ext3_i.i_flags &= ~EXT3_BTREE_FL;
	ext3_mark_inode_dirty(handle, dir);
	if (IS_SYNC(dir)) 
		handle->h_sync = 1;
	d_instantiate(dentry, inode);
	brelse (bh);
	err = 0;

out_stop:
	ext3_journal_stop(handle, dir);
out:
	return err;

out_no_entry:
	inode->i_nlink = 0;
	ext3_mark_inode_dirty(handle, inode);
	/* The implicit delete in the iput() will be dealt with as a
	 * recursive transaction. */
	iput (inode);
	goto out_stop;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir (struct inode * inode)
{
	unsigned long offset;
	struct buffer_head * bh;
	struct ext3_dir_entry_2 * de, * de1;
	struct super_block * sb;
	int err;

	sb = inode->i_sb;
	if (inode->i_size < EXT3_DIR_REC_LEN(1) + EXT3_DIR_REC_LEN(2) ||
	    !(bh = ext3_bread (NULL, inode, 0, 0, &err))) {
	    	ext3_warning (inode->i_sb, "empty_dir",
			      "bad directory (dir #%lu) - no data block",
			      inode->i_ino);
		return 1;
	}
	de = (struct ext3_dir_entry_2 *) bh->b_data;
	de1 = (struct ext3_dir_entry_2 *) ((char *) de + le16_to_cpu(de->rec_len));
	if (le32_to_cpu(de->inode) != inode->i_ino || !le32_to_cpu(de1->inode) || 
	    strcmp (".", de->name) || strcmp ("..", de1->name)) {
	    	ext3_warning (inode->i_sb, "empty_dir",
			      "bad directory (dir #%lu) - no `.' or `..'",
			      inode->i_ino);
		brelse (bh);
		return 1;
	}
	offset = le16_to_cpu(de->rec_len) + le16_to_cpu(de1->rec_len);
	de = (struct ext3_dir_entry_2 *) ((char *) de1 + le16_to_cpu(de1->rec_len));
	while (offset < inode->i_size ) {
		if (!bh || (void *) de >= (void *) (bh->b_data + sb->s_blocksize)) {
			brelse (bh);
			bh = ext3_bread (NULL, inode, offset >> EXT3_BLOCK_SIZE_BITS(sb), 0, &err);
			if (!bh) {
#if 0
				ext3_error (sb, "empty_dir",
					    "directory #%lu contains a hole at offset %lu",
					    inode->i_ino, offset);
#endif
				offset += sb->s_blocksize;
				continue;
			}
			de = (struct ext3_dir_entry_2 *) bh->b_data;
		}
		if (!ext3_check_dir_entry ("empty_dir", inode, de, bh,
					   offset)) {
			brelse (bh);
			return 1;
		}
		if (le32_to_cpu(de->inode)) {
			brelse (bh);
			return 0;
		}
		offset += le16_to_cpu(de->rec_len);
		de = (struct ext3_dir_entry_2 *) ((char *) de + le16_to_cpu(de->rec_len));
	}
	brelse (bh);
	return 1;
}

int ext3_rmdir (struct inode * dir, struct dentry *dentry)
{
	int retval;
	struct inode * inode;
	struct buffer_head * bh;
	struct ext3_dir_entry_2 * de;
	handle_t *handle;
	
	handle = ext3_journal_start(dir, EXT3_DELETE_TRANS_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	retval = -ENOENT;
	bh = ext3_find_entry (dir, dentry->d_name.name, dentry->d_name.len, &de);
	if (!bh)
		goto end_rmdir;

	inode = dentry->d_inode;
	DQUOT_INIT(inode);

	retval = -EIO;
	if (le32_to_cpu(de->inode) != inode->i_ino)
		goto end_rmdir;

	retval = -ENOTEMPTY;
	if (!empty_dir (inode))
		goto end_rmdir;

	retval = ext3_delete_entry (handle, de, bh); /* @@@ handle */
	dir->i_version = ++global_event;

	if (retval)
		goto end_rmdir;
	if (inode->i_nlink != 2)
		ext3_warning (inode->i_sb, "ext3_rmdir",
			      "empty directory has nlink!=2 (%d)",
			      inode->i_nlink);
	inode->i_version = ++global_event;
	inode->i_nlink = 0;
	inode->i_size = 0;
	ext3_orphan_add(handle, inode);
	ext3_mark_inode_dirty(handle, inode);
	dir->i_nlink--;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->u.ext3_i.i_flags &= ~EXT3_BTREE_FL;
	ext3_mark_inode_dirty(handle, dir);
	d_delete(dentry);
	if (IS_SYNC(dir)) 
		handle->h_sync = 1;

end_rmdir:
	ext3_journal_stop(handle, dir);
	brelse (bh);
	return retval;
}

/* ext3_orphan_add() links a unlinked or truncated inode into a list of
 * such inodes, starting at the superblock, in case we crash before the
 * file is closed/deleted, or in case the inode truncate spans multiple
 * transactions and the last transaction is not recovered after a crash.
 *
 * At filesystem recovery time, we walk this list deleting unlinked
 * inodes and truncating linked inodes in ext3_orphan_cleanup().
 */
void ext3_orphan_add(handle_t *handle, struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext3_iloc iloc;
	
	lock_super(sb);
	if (!list_empty(&inode->u.ext3_i.i_orphan)) {
		unlock_super(sb);
		return;
	}

	/* Orphan handling is only valid for files with data blocks
	 * being truncated, or files being unlinked. */
	
	J_ASSERT ((S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
		   S_ISLNK(inode->i_mode)) || inode->i_nlink == 0);

	journal_get_write_access(handle, sb->u.ext3_sb.s_sbh);
	ext3_reserve_inode_write(handle, inode, &iloc);
	
	/* Insert this inode at the head of the on-disk orphan list... */
	NEXT_ORPHAN(inode) = le32_to_cpu(EXT3_SB(sb)->s_es->s_last_orphan);
	EXT3_SB(sb)->s_es->s_last_orphan = cpu_to_le32(inode->i_ino);
	journal_dirty_metadata(handle, sb->u.ext3_sb.s_sbh);
	ext3_mark_iloc_dirty(handle, inode, &iloc);

	/* ...and the head of the in-memory list. */
	list_add(&inode->u.ext3_i.i_orphan, &EXT3_SB(sb)->s_orphan);

	unlock_super(sb);
	jfs_debug(4, "superblock will point to %ld\n", inode->i_ino);
	jfs_debug(4, "orphan inode %ld will point to %d\n",
		  inode->i_ino, NEXT_ORPHAN(inode));
}

int ext3_unlink(struct inode * dir, struct dentry *dentry)
{
	int retval;
	struct inode * inode;
	struct buffer_head * bh;
	struct ext3_dir_entry_2 * de;
	handle_t *handle;
	
	handle = ext3_journal_start(dir, EXT3_DELETE_TRANS_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	retval = -ENOENT;
	bh = ext3_find_entry (dir, dentry->d_name.name, dentry->d_name.len, &de);
	if (!bh)
		goto end_unlink;

	inode = dentry->d_inode;
	DQUOT_INIT(inode);

	retval = -EIO;
	if (le32_to_cpu(de->inode) != inode->i_ino)
		goto end_unlink;
	
	if (!inode->i_nlink) {
		ext3_warning (inode->i_sb, "ext3_unlink",
			      "Deleting nonexistent file (%lu), %d",
			      inode->i_ino, inode->i_nlink);
		inode->i_nlink = 1;
	}
	retval = ext3_delete_entry (handle, de, bh);
	if (retval)
		goto end_unlink;
	dir->i_version = ++global_event;
	if (IS_SYNC(dir))
		handle->h_sync = 1;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->u.ext3_i.i_flags &= ~EXT3_BTREE_FL;
	ext3_mark_inode_dirty(handle, dir);
	inode->i_nlink--;
	if (!inode->i_nlink)
		ext3_orphan_add(handle, inode);
	ext3_mark_inode_dirty(handle, inode);
	inode->i_ctime = dir->i_ctime;
	retval = 0;
	d_delete(dentry);	/* This also frees the inode */

end_unlink:
	ext3_journal_stop(handle, dir);
	brelse (bh);
	return retval;
}

int ext3_symlink (struct inode * dir, struct dentry *dentry, const char * symname)
{
	struct ext3_dir_entry_2 * de;
	struct inode * inode;
	struct buffer_head * bh = NULL, * name_block = NULL;
	char * link;
	int i, l, err = -EIO;
	char c;
	handle_t *handle;
	
	handle = ext3_journal_start(dir, EXT3_DATA_TRANS_BLOCKS + 5);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (!(inode = ext3_new_inode (handle, dir, S_IFLNK, &err)))
		goto out;
	inode->i_mode = S_IFLNK | S_IRWXUGO;
	inode->i_op = &ext3_symlink_inode_operations;
	for (l = 0; l < inode->i_sb->s_blocksize - 1 &&
	     symname [l]; l++)
		;
	if (l >= sizeof (inode->u.ext3_i.i_data_u.i_data)) {

		ext3_debug ("l=%d, normal symlink\n", l);

		name_block = ext3_bread (handle, inode, 0, 1, &err);
		if (!name_block) {
			inode->i_nlink--;
			ext3_mark_inode_dirty(handle, inode);
			iput (inode);
			goto out;
		}
		link = name_block->b_data;
		journal_get_write_access(handle, name_block);
	} else {
		link = (char *) inode->u.ext3_i.i_data_u.i_data;

		ext3_debug ("l=%d, fast symlink\n", l);

	}
	i = 0;
	while (i < inode->i_sb->s_blocksize - 1 && (c = *(symname++)))
		link[i++] = c;
	link[i] = 0;
	if (name_block) {
		journal_dirty_metadata(handle, name_block);
		brelse (name_block);
	}
	inode->i_size = inode->u.ext3_i.i_disksize = i;
	ext3_mark_inode_dirty(handle, inode);

	bh = ext3_add_entry (handle, dir, dentry->d_name.name, dentry->d_name.len, &de, &err);
	if (!bh)
		goto out_no_entry;
	de->inode = cpu_to_le32(inode->i_ino);
	if (EXT3_HAS_INCOMPAT_FEATURE(dir->i_sb,
				      EXT3_FEATURE_INCOMPAT_FILETYPE))
		de->file_type = EXT3_FT_SYMLINK;
	dir->i_version = ++global_event;
	if (IS_SYNC(dir)) 
		handle->h_sync = 1;
	brelse (bh);
	d_instantiate(dentry, inode);
	err = 0;
out:
	ext3_journal_stop(handle, dir);
	return err;

out_no_entry:
	inode->i_nlink--;
	ext3_mark_inode_dirty(handle, inode);
	iput (inode);
	goto out;
}

int ext3_link (struct dentry * old_dentry,
		struct inode * dir, struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;
	struct ext3_dir_entry_2 * de;
	struct buffer_head * bh;
	int err;
	handle_t *handle;

	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	if (inode->i_nlink >= EXT3_LINK_MAX)
		return -EMLINK;

	handle = ext3_journal_start(dir, EXT3_DATA_TRANS_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	
	bh = ext3_add_entry (handle, dir, dentry->d_name.name, dentry->d_name.len, &de, &err);
	if (!bh)
		goto out;

	de->inode = cpu_to_le32(inode->i_ino);
	if (EXT3_HAS_INCOMPAT_FEATURE(inode->i_sb,
				      EXT3_FEATURE_INCOMPAT_FILETYPE)) {
		if (S_ISREG(inode->i_mode))
			de->file_type = EXT3_FT_REG_FILE;
		else if (S_ISDIR(inode->i_mode))
			de->file_type = EXT3_FT_DIR;
		else if (S_ISLNK(inode->i_mode))
			de->file_type = EXT3_FT_SYMLINK;
		else if (S_ISSOCK(inode->i_mode))
			de->file_type = EXT3_FT_SOCK;
		else if (S_ISCHR(inode->i_mode))
			de->file_type = EXT3_FT_CHRDEV;
		else if (S_ISBLK(inode->i_mode))
			de->file_type = EXT3_FT_BLKDEV;
		else if (S_ISFIFO(inode->i_mode))  
			de->file_type = EXT3_FT_FIFO;
	}
	dir->i_version = ++global_event;
	if (IS_SYNC(dir))
		handle->h_sync = 1;
	brelse (bh);
	inode->i_nlink++;
	inode->i_ctime = CURRENT_TIME;
	ext3_mark_inode_dirty(handle, inode);
	inode->i_count++;
	d_instantiate(dentry, inode);
	err = 0;
out:
	ext3_journal_stop(handle, dir);
	return err;
}

#define PARENT_INO(buffer) \
	((struct ext3_dir_entry_2 *) ((char *) buffer + \
	le16_to_cpu(((struct ext3_dir_entry_2 *) buffer)->rec_len)))->inode

/*
 * Anybody can rename anything with this: the permission checks are left to the
 * higher-level routines.
 */
int ext3_rename (struct inode * old_dir, struct dentry *old_dentry,
			   struct inode * new_dir,struct dentry *new_dentry)
{
	struct inode * old_inode, * new_inode;
	struct buffer_head * old_bh, * new_bh, * dir_bh;
	struct ext3_dir_entry_2 * old_de, * new_de;
	int retval;
	handle_t *handle;
	
	old_bh = new_bh = dir_bh = NULL;

	handle = ext3_journal_start(old_dir, 2 * EXT3_DATA_TRANS_BLOCKS + 2);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	old_bh = ext3_find_entry (old_dir, old_dentry->d_name.name, old_dentry->d_name.len, &old_de);
	/*
	 *  Check for inode number is _not_ due to possible IO errors.
	 *  We might rmdir the source, keep it as pwd of some process
	 *  and merrily kill the link to whatever was created under the
	 *  same name. Goodbye sticky bit ;-<
	 */
	old_inode = old_dentry->d_inode;
	retval = -ENOENT;
	if (!old_bh || le32_to_cpu(old_de->inode) != old_inode->i_ino)
		goto end_rename;

	new_inode = new_dentry->d_inode;
	new_bh = ext3_find_entry (new_dir, new_dentry->d_name.name,
				new_dentry->d_name.len, &new_de);
	if (new_bh) {
		if (!new_inode) {
			brelse (new_bh);
			new_bh = NULL;
		} else {
			DQUOT_INIT(new_inode);
		}
	}
	if (S_ISDIR(old_inode->i_mode)) {
		if (new_inode) {
			retval = -ENOTEMPTY;
			if (!empty_dir (new_inode))
				goto end_rename;
		}
		retval = -EIO;
		dir_bh = ext3_bread (NULL, old_inode, 0, 0, &retval);
		if (!dir_bh)
			goto end_rename;
		if (le32_to_cpu(PARENT_INO(dir_bh->b_data)) != old_dir->i_ino)
			goto end_rename;
		retval = -EMLINK;
		if (!new_inode && new_dir!=old_dir &&
				new_dir->i_nlink >= EXT3_LINK_MAX)
			goto end_rename;
	}
	if (!new_bh) {
		new_bh = ext3_add_entry (handle, new_dir, new_dentry->d_name.name,
					new_dentry->d_name.len, &new_de,
					&retval);
		if (!new_bh)
			goto end_rename;
	}
	new_dir->i_version = ++global_event;

	/*
	 * ok, that's it
	 */

	journal_get_write_access(handle, old_bh);
	journal_get_write_access(handle, new_bh);
	if (dir_bh)
		journal_get_write_access(handle, dir_bh);

	new_de->inode = le32_to_cpu(old_inode->i_ino);
	if (EXT3_HAS_INCOMPAT_FEATURE(new_dir->i_sb,
				      EXT3_FEATURE_INCOMPAT_FILETYPE))
		new_de->file_type = old_de->file_type;
	
	ext3_delete_entry (handle, old_de, old_bh); /* @@@ HANDLE */

	old_dir->i_version = ++global_event;
	if (new_inode) {
		new_inode->i_nlink--;
		new_inode->i_ctime = CURRENT_TIME;
		ext3_mark_inode_dirty(handle, new_inode);
	}
	old_dir->i_ctime = old_dir->i_mtime = CURRENT_TIME;
	old_dir->u.ext3_i.i_flags &= ~EXT3_BTREE_FL;
	ext3_mark_inode_dirty(handle, old_dir);
	if (dir_bh) {
		PARENT_INO(dir_bh->b_data) = le32_to_cpu(new_dir->i_ino);
		journal_dirty_metadata(handle, dir_bh);
		old_dir->i_nlink--;
		ext3_mark_inode_dirty(handle, old_dir);
		if (new_inode) {
			new_inode->i_nlink--;
			ext3_mark_inode_dirty(handle, new_inode);
		} else {
			new_dir->i_nlink++;
			new_dir->u.ext3_i.i_flags &= ~EXT3_BTREE_FL;
			ext3_mark_inode_dirty(handle, new_dir);
		}
	}
	if (new_inode && !new_inode->i_nlink)
		ext3_orphan_add(handle, new_inode);
	journal_dirty_metadata(handle, old_bh);
	journal_dirty_metadata(handle, new_bh);
	if (IS_SYNC(old_dir) || IS_SYNC(new_dir))
		handle->h_sync = 1;

	retval = 0;

end_rename:
	brelse (dir_bh);
	brelse (old_bh);
	brelse (new_bh);
	ext3_journal_stop(handle, old_dir);
	return retval;
}
