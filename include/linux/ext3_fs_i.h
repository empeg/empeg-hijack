/*
 *  linux/include/linux/ext3_fs_i.h
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/include/linux/minix_fs_i.h
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#ifndef _LINUX_EXT3_FS_I
#define _LINUX_EXT3_FS_I

/*
 * second extended file system inode data in memory
 */
struct ext3_inode_info {
	union {
		struct pipe_inode_info	pipe_i;		/* placeholder */
		__u32			i_data[15];
	} i_data_u;
	__u32	i_flags;
	__u32	i_faddr;
	__u8	i_frag_no;
	__u8	i_frag_size;
	__u16	i_osync;
	__u32	i_file_acl;
	__u32	i_dir_acl;
	__u32	i_dtime;
	__u32	i_version;
	__u32	i_block_group;
	__u32	i_next_alloc_block;
	__u32	i_next_alloc_goal;
	__u32	i_prealloc_block;
	__u32	i_prealloc_count;
	__u32	i_high_size;
	struct list_head i_orphan;	/* unlinked but open inodes */
	int	i_new_inode:1;	/* Is a freshly allocated inode */
	/* i_disksize keeps track of what the inode size is ON DISK, not
	 * in memory.  During truncate, i_size is set to 0 by the VFS
	 * but the filesystem won't set i_disksize to 0 until the
	 * truncate is actually under way. */
	off_t	i_disksize;
};

#endif	/* _LINUX_EXT3_FS_I */
