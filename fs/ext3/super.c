/*
 *  linux/fs/ext3/super.c
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
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 */

#include <linux/module.h>

#include <stdarg.h>

#include <asm/bitops.h>
#include <asm/uaccess.h>
#include <asm/system.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jfs.h>
#include <linux/ext3_fs.h>
#include <linux/malloc.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/blkdev.h>
#include <linux/init.h>

static char error_buf[1024];

static int ext3_load_journal(struct super_block *, struct ext3_super_block *);
static int ext3_create_journal(struct super_block *, struct ext3_super_block *,
			       int);
static void ext3_commit_super (struct super_block * sb,
			       struct ext3_super_block * es, 
			       int sync);
static void ext3_mark_recovery_complete(struct super_block * sb,
					struct ext3_super_block * es);
static void ext3_clear_journal_err(struct super_block * sb,
				   struct ext3_super_block * es);
static void ext3_init_dquot(struct vfsmount *);

/* Deal with the reporting of failure conditions on a filesystem such as
 * inconsistencies detected or read IO failures. 
 *
 * On ext2, we can store the error state of the filesystem in the
 * superblock.  That is not possible on ext3, because we may have other
 * write ordering constraints on the superblock which prevent us from
 * writing it out straight away; and given that the journal is about to
 * be aborted, we can't rely on the current, or future, transactions to
 * write out the superblock safely.
 *
 * We'll just use the journal_abort() error code to record an error in
 * the journal instead.  On recovery, the journal will compain about
 * that error until we've noted it down and cleared it.
 */

void ext3_error (struct super_block * sb, const char * function,
		 const char * fmt, ...)
{
	va_list args;

	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);
	if (test_opt3 (sb, ERRORS_PANIC) ||
	    (le16_to_cpu(sb->u.ext3_sb.s_es->s_errors) == EXT3_ERRORS_PANIC &&
	     !test_opt3 (sb, ERRORS_CONT) && !test_opt3 (sb, ERRORS_RO)))
		panic ("EXT3-fs panic (device %s): %s: %s\n",
		       bdevname(sb->s_dev), function, error_buf);
	printk (KERN_CRIT "EXT3-fs error (device %s): %s: %s\n",
		bdevname(sb->s_dev), function, error_buf);
	if (test_opt3 (sb, ERRORS_RO) ||
	    (le16_to_cpu(sb->u.ext3_sb.s_es->s_errors) == EXT3_ERRORS_RO &&
	     !test_opt3 (sb, ERRORS_CONT) && !test_opt3 (sb, ERRORS_PANIC))) {
		printk ("Remounting filesystem read-only\n");
		sb->s_flags |= MS_RDONLY;
		sb->u.ext3_sb.s_mount_opt |= EXT3_MOUNT_ABORT;
		
		if (!(sb->s_flags & MS_RDONLY))
			journal_abort(EXT3_SB(sb)->s_journal, -EIO);
	}
}

/* 
 * ext3_abort is a much stronger failure handler than ext3_error.  The
 * abort function may be used to deal with unrecoverable failures such
 * as journal IO errors or ENOMEM at a critical moment in log
 * management. 
 * 
 * We unconditionally force the filesystem into an ABORT|READONLY state,
 * unless the error response on the fs has been set to panic in which we
 * take the easy way out and panic immediately.
 */

void ext3_abort (struct super_block * sb, const char * function,
		 const char * fmt, ...)
{
	va_list args;

	printk (KERN_CRIT "ext3_abort called.\n");

	sb->u.ext3_sb.s_mount_state |= EXT3_ERROR_FS;

	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);

	if (test_opt3 (sb, ERRORS_PANIC) ||
	    (le16_to_cpu(sb->u.ext3_sb.s_es->s_errors) ==
	     EXT3_ERRORS_PANIC))
		panic ("EXT3-fs panic (device %s): %s: %s\n",
		       bdevname(sb->s_dev), function, error_buf);

	printk (KERN_CRIT "EXT3-fs error (device %s): %s: %s\n",
		bdevname(sb->s_dev), function, error_buf);
	if (!(sb->s_flags & MS_RDONLY)) {
		printk ("Remounting filesystem read-only\n");
		sb->s_flags |= MS_RDONLY;
	}
	sb->u.ext3_sb.s_mount_opt |= EXT3_MOUNT_ABORT;
	journal_abort(EXT3_SB(sb)->s_journal, -EIO);
}

NORET_TYPE void ext3_panic (struct super_block * sb, const char * function,
			    const char * fmt, ...)
{
	va_list args;

	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);
	/* this is to prevent panic from syncing this filesystem */
	if (sb->s_lock)
		sb->s_lock=0;
	sb->s_flags |= MS_RDONLY;
	panic ("EXT3-fs panic (device %s): %s: %s\n",
	       bdevname(sb->s_dev), function, error_buf);
}

void ext3_warning (struct super_block * sb, const char * function,
		   const char * fmt, ...)
{
	va_list args;

	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);
	printk (KERN_WARNING "EXT3-fs warning (device %s): %s: %s\n",
		bdevname(sb->s_dev), function, error_buf);
}

void ext3_update_fs_rev(struct super_block *sb)
{
	struct ext3_super_block *es = EXT3_SB(sb)->s_es;

	if (le32_to_cpu(es->s_rev_level) > EXT3_GOOD_OLD_REV)
		return;

	ext3_warning(sb, __FUNCTION__,
		     "updating to rev %d because of new feature flag, "
		     "running e2fsck is recommended",
		     EXT3_DYNAMIC_REV);

	es->s_rev_level = cpu_to_le32(EXT3_DYNAMIC_REV);
	es->s_first_ino = cpu_to_le32(EXT3_GOOD_OLD_FIRST_INO);
	es->s_inode_size = cpu_to_le16(EXT3_GOOD_OLD_INODE_SIZE);
	/* leave es->s_feature_*compat flags alone */
	/* es->s_uuid will be set by e2fsck if empty */

	/*
	 * the rest of the superblock fields should be zero, and if not it
	 * means they are likely already in use, so leave them alone.  We
	 * can leave it up to e2fsck to clean up any inconsistencies there.
	 */
}

void ext3_put_super (struct super_block * sb)
{
	int db_count;
	int i;
	struct ext3_super_block *es = EXT3_SB(sb)->s_es;

	journal_release(EXT3_SB(sb)->s_journal);
	if (!(sb->s_flags & MS_RDONLY)) {
		EXT3_CLEAR_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER);
		es->s_state = le16_to_cpu(EXT3_SB(sb)->s_mount_state);
		mark_buffer_dirty(EXT3_SB(sb)->s_sbh, 1);
		ext3_commit_super(sb, es, 1);
	}

	db_count = EXT3_SB(sb)->s_gdb_count;
	for (i = 0; i < db_count; i++)
		if (EXT3_SB(sb)->s_group_desc[i])
			brelse (EXT3_SB(sb)->s_group_desc[i]);
	kfree_s (EXT3_SB(sb)->s_group_desc,
		 db_count * sizeof (struct buffer_head *));
	for (i = 0; i < EXT3_MAX_GROUP_LOADED; i++)
		if (EXT3_SB(sb)->s_inode_bitmap[i])
			brelse (EXT3_SB(sb)->s_inode_bitmap[i]);
	for (i = 0; i < EXT3_MAX_GROUP_LOADED; i++)
		if (EXT3_SB(sb)->s_block_bitmap[i])
			brelse (EXT3_SB(sb)->s_block_bitmap[i]);
	brelse (EXT3_SB(sb)->s_sbh);

	J_ASSERT (list_empty(&EXT3_SB(sb)->s_orphan));

	MOD_DEC_USE_COUNT;
	return;
}

static struct super_operations ext3_sops = {
	ext3_read_inode,
	ext3_write_inode,
	ext3_put_inode,
	ext3_delete_inode,
	ext3_notify_change,
	ext3_put_super,
	ext3_write_super,
	ext3_statfs,
	ext3_remount,
	NULL,		/* clear_inode */
	NULL,		/* umount_begin */
	ext3_init_dquot
};

/*
 * This function has been shamelessly adapted from the msdos fs
 */
static int parse_options (char * options, unsigned long * sb_block,
			  unsigned short *resuid, unsigned short * resgid,
			  unsigned long * mount_options, 
			  unsigned long * inum,
			  int is_remount)
{
	char * this_char;
	char * value;

	if (!options)
		return 1;
	for (this_char = strtok (options, ",");
	     this_char != NULL;
	     this_char = strtok (NULL, ",")) {
		if ((value = strchr (this_char, '=')) != NULL)
			*value++ = 0;
		if (!strcmp (this_char, "bsddf"))
			clear_opt3 (*mount_options, MINIX_DF);
		else if (!strcmp (this_char, "abort"))
			set_opt3 (*mount_options, ABORT);
		else if (!strcmp (this_char, "check")) {
			if (!value || !*value)
				set_opt3 (*mount_options, CHECK_NORMAL);
			else if (!strcmp (value, "none")) {
				clear_opt3 (*mount_options, CHECK_NORMAL);
				clear_opt3 (*mount_options, CHECK_STRICT);
			}
			else if (!strcmp (value, "normal"))
				set_opt3 (*mount_options, CHECK_NORMAL);
			else if (!strcmp (value, "strict")) {
				set_opt3 (*mount_options, CHECK_NORMAL);
				set_opt3 (*mount_options, CHECK_STRICT);
			}
			else {
				printk ("EXT3-fs: Invalid check option: %s\n",
					value);
				return 0;
			}
		}
		else if (!strcmp (this_char, "debug"))
			set_opt3 (*mount_options, DEBUG);
		else if (!strcmp (this_char, "errors")) {
			if (!value || !*value) {
				printk ("EXT3-fs: the errors option requires "
					"an argument\n");
				return 0;
			}
			if (!strcmp (value, "continue")) {
				clear_opt3 (*mount_options, ERRORS_RO);
				clear_opt3 (*mount_options, ERRORS_PANIC);
				set_opt3 (*mount_options, ERRORS_CONT);
			}
			else if (!strcmp (value, "remount-ro")) {
				clear_opt3 (*mount_options, ERRORS_CONT);
				clear_opt3 (*mount_options, ERRORS_PANIC);
				set_opt3 (*mount_options, ERRORS_RO);
			}
			else if (!strcmp (value, "panic")) {
				clear_opt3 (*mount_options, ERRORS_CONT);
				clear_opt3 (*mount_options, ERRORS_RO);
				set_opt3 (*mount_options, ERRORS_PANIC);
			}
			else {
				printk ("EXT3-fs: Invalid errors option: %s\n",
					value);
				return 0;
			}
		}
		else if (!strcmp (this_char, "grpid") ||
			 !strcmp (this_char, "bsdgroups"))
			set_opt3 (*mount_options, GRPID);
		else if (!strcmp (this_char, "minixdf"))
			set_opt3 (*mount_options, MINIX_DF);
		else if (!strcmp (this_char, "nocheck")) {
			clear_opt3 (*mount_options, CHECK_NORMAL);
			clear_opt3 (*mount_options, CHECK_STRICT);
		}
		else if (!strcmp (this_char, "nogrpid") ||
			 !strcmp (this_char, "sysvgroups"))
			clear_opt3 (*mount_options, GRPID);
		else if (!strcmp (this_char, "resgid")) {
			if (!value || !*value) {
				printk ("EXT3-fs: the resgid option requires "
					"an argument\n");
				return 0;
			}
			*resgid = simple_strtoul (value, &value, 0);
			if (*value) {
				printk ("EXT3-fs: Invalid resgid option: %s\n",
					value);
				return 0;
			}
		}
		else if (!strcmp (this_char, "resuid")) {
			if (!value || !*value) {
				printk ("EXT3-fs: the resuid option requires "
					"an argument");
				return 0;
			}
			*resuid = simple_strtoul (value, &value, 0);
			if (*value) {
				printk ("EXT3-fs: Invalid resuid option: %s\n",
					value);
				return 0;
			}
		}
		else if (!strcmp (this_char, "sb")) {
			if (!value || !*value) {
				printk ("EXT3-fs: the sb option requires "
					"an argument\n");
				return 0;
			}
			*sb_block = simple_strtoul (value, &value, 0);
			if (*value) {
				printk ("EXT3-fs: Invalid sb option: %s\n",
					value);
				return 0;
			}
		}
		/* Silently ignore the quota options */
		else if (!strcmp (this_char, "grpquota")
		         || !strcmp (this_char, "noquota")
		         || !strcmp (this_char, "quota")
		         || !strcmp (this_char, "usrquota"))
			/* Don't do anything ;-) */ ;
		else if (!strcmp (this_char, "journal")) {
			/* @@@ FIXME */
			/* Eventually we will want to be able to create
                           a journal file here.  For now, only allow the
                           user to specify an existing inode to be the
                           journal file. */
			if (!value || !*value) {
				printk ("EXT3-fs: the journal option requires "
					"an argument\n");
				return 0;
			}
			if (is_remount) {
				printk ("EXT3-fs: cannot specify journal on remount\n");
				return 0;
			}

			if (!strcmp (value, "update")) {
				set_opt3 (*mount_options, UPDATE_JOURNAL);
			} else {
				*inum = simple_strtoul (value, &value, 0);
				if (*value) {
					printk ("EXT3-fs: Invalid journal option: "
						"%s\n", value);
					return 0;
				}
			}
		} 
		else if (!strcmp (this_char, "noload"))
			set_opt3 (*mount_options, NOLOAD);
		else if (!strcmp (this_char, "data")) {
			int data_opt = 0;

			if (!value || !*value) {
				printk ("EXT3-fs: the data option requires "
					"an argument\n");
				return 0;
			}
			if (is_remount) {
				printk ("EXT3-fs: cannot change data mode on remount\n");
				return 0;
			}

			if (!strcmp (value, "journal"))
				data_opt = EXT3_MOUNT_JOURNAL_DATA;
			else if (!strcmp (value, "ordered"))
				data_opt = EXT3_MOUNT_ORDERED_DATA;
			else if (!strcmp (value, "writeback"))
				data_opt = EXT3_MOUNT_WRITEBACK_DATA;
			else { 
				printk ("EXT3-fs: Invalid data option: %s\n",
					value);
				return 0;
			}
			*mount_options &= ~EXT3_MOUNT_DATA_FLAGS;
			*mount_options |= data_opt;
		} else {
			printk ("EXT3-fs: Unrecognized mount option %s\n", this_char);
			return 0;
		}
	}
	return 1;
}

static void ext3_setup_super (struct super_block * sb,
			      struct ext3_super_block * es)
{
	if (le32_to_cpu(es->s_rev_level) > EXT3_MAX_SUPP_REV) {
		printk ("EXT3-fs warning: revision level too high, "
			"forcing read-only mode\n");
		sb->s_flags |= MS_RDONLY;
	}
	if (!(sb->s_flags & MS_RDONLY)) {
		if (!(sb->u.ext3_sb.s_mount_state & EXT3_VALID_FS))
			printk ("EXT3-fs warning: mounting unchecked fs, "
				"running e2fsck is recommended\n");
		else if ((sb->u.ext3_sb.s_mount_state & EXT3_ERROR_FS))
			printk ("EXT3-fs warning: mounting fs with errors, "
				"running e2fsck is recommended\n");
		else if ((__s16) le16_to_cpu(es->s_max_mnt_count) >= 0 &&
		         le16_to_cpu(es->s_mnt_count) >=
			 (unsigned short) (__s16) le16_to_cpu(es->s_max_mnt_count))
			printk ("EXT3-fs warning: maximal mount count reached, "
				"running e2fsck is recommended\n");
		else if (le32_to_cpu(es->s_checkinterval) &&
			(le32_to_cpu(es->s_lastcheck) + le32_to_cpu(es->s_checkinterval) <= CURRENT_TIME))
			printk ("EXT3-fs warning: checktime reached, "
				"running e2fsck is recommended\n");

#if 0
		/* @@@ We _will_ want to clear the valid bit if we find
                   inconsistencies, to force a fsck at reboot.  But for
                   a plain journaled filesystem we can keep it set as
                   valid forever! :) */
		es->s_state = cpu_to_le16(le16_to_cpu(es->s_state) & ~EXT3_VALID_FS);
#endif
		if (!(__s16) le16_to_cpu(es->s_max_mnt_count))
			es->s_max_mnt_count = (__s16) cpu_to_le16(EXT3_DFL_MAX_MNT_COUNT);
		es->s_mnt_count=cpu_to_le16(le16_to_cpu(es->s_mnt_count) + 1);
		es->s_mtime = cpu_to_le32(CURRENT_TIME);
		ext3_update_fs_rev(sb);
		EXT3_SET_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER);
		ext3_commit_super (sb, es, 1);
		if (test_opt3 (sb, DEBUG))
			printk ("[EXT II FS %s, %s, bs=%lu, fs=%lu, gc=%lu, "
				"bpg=%lu, ipg=%lu, mo=%04lx]\n",
				EXT3FS_VERSION, EXT3FS_DATE, sb->s_blocksize,
				sb->u.ext3_sb.s_frag_size,
				sb->u.ext3_sb.s_groups_count,
				EXT3_BLOCKS_PER_GROUP(sb),
				EXT3_INODES_PER_GROUP(sb),
				sb->u.ext3_sb.s_mount_opt);
		if (test_opt3 (sb, CHECK)) {
			ext3_check_blocks_bitmap (sb);
			ext3_check_inodes_bitmap (sb);
		}
	}
#if 0 /* ibasket's still have unresolved bugs... -DaveM */

	/* [T. Schoebel-Theuer] This limit should be maintained on disk.
	 * This is just provisionary.
	 */
	sb->s_ibasket_max = 100;
#endif
}

static int ext3_check_descriptors (struct super_block * sb)
{
	int i;
	int desc_block = 0;
	unsigned long block = le32_to_cpu(sb->u.ext3_sb.s_es->s_first_data_block);
	struct ext3_group_desc * gdp = NULL;

	ext3_debug ("Checking group descriptors");

	for (i = 0; i < sb->u.ext3_sb.s_groups_count; i++)
	{
		if ((i % EXT3_DESC_PER_BLOCK(sb)) == 0)
			gdp = (struct ext3_group_desc *) sb->u.ext3_sb.s_group_desc[desc_block++]->b_data;
		if (le32_to_cpu(gdp->bg_block_bitmap) < block ||
		    le32_to_cpu(gdp->bg_block_bitmap) >= block + EXT3_BLOCKS_PER_GROUP(sb))
		{
			ext3_error (sb, "ext3_check_descriptors",
				    "Block bitmap for group %d"
				    " not in group (block %lu)!",
				    i, (unsigned long) le32_to_cpu(gdp->bg_block_bitmap));
			return 0;
		}
		if (le32_to_cpu(gdp->bg_inode_bitmap) < block ||
		    le32_to_cpu(gdp->bg_inode_bitmap) >= block + EXT3_BLOCKS_PER_GROUP(sb))
		{
			ext3_error (sb, "ext3_check_descriptors",
				    "Inode bitmap for group %d"
				    " not in group (block %lu)!",
				    i, (unsigned long) le32_to_cpu(gdp->bg_inode_bitmap));
			return 0;
		}
		if (le32_to_cpu(gdp->bg_inode_table) < block ||
		    le32_to_cpu(gdp->bg_inode_table) + sb->u.ext3_sb.s_itb_per_group >=
		    block + EXT3_BLOCKS_PER_GROUP(sb))
		{
			ext3_error (sb, "ext3_check_descriptors",
				    "Inode table for group %d"
				    " not in group (block %lu)!",
				    i, (unsigned long) le32_to_cpu(gdp->bg_inode_table));
			return 0;
		}
		block += EXT3_BLOCKS_PER_GROUP(sb);
		gdp++;
	}
	return 1;
}

/* ext3_orphan_cleanup() walks a singly-linked list of inodes (starting at
 * the superblock) which were deleted from all directories, but held open by
 * a process at the time of a crash.  We walk the list and try to delete these
 * inodes at recovery time (only with a read-write filesystem).
 *
 * In order to keep the orphan inode chain consistent during traversal (in
 * case of crash during recovery), we link each inode into the superblock
 * orphan list_head and handle it the same way as an inode deletion during
 * normal operation (which journals the operations for us).
 *
 * We only do an iget() and an iput() on each inode, which is very safe if we
 * accidentally point at an in-use or already deleted inode.  The worst that
 * can happen in this case is that we get a "bit already cleared" message from
 * ext3_free_inode().  The only reason we would point at a wrong inode is if
 * e2fsck was run on this filesystem, and it must have already done the orphan
 * inode cleanup for us, so we can safely abort without any further action.
 */
static void ext3_orphan_cleanup (struct super_block * sb,
				 struct ext3_super_block * es)
{
	unsigned int s_flags = sb->s_flags;
	int nr_orphans = 0, nr_truncates = 0;
	if (!es->s_last_orphan) {
		jfs_debug(4, "no orphan inodes to clean up\n");
		return;
	}
	
	if (s_flags & MS_RDONLY) {
		printk(KERN_INFO "EXT3-fs: %s: orphan cleanup on read-only fs\n",
		       kdevname(sb->s_dev));
		sb->s_flags &= ~MS_RDONLY;
	}

	while (es->s_last_orphan) {
		struct inode *inode;

		if (!(inode =
		      ext3_orphan_get(sb, le32_to_cpu(es->s_last_orphan)))) {
			es->s_last_orphan = 0;
			break;
		}

		list_add(&inode->u.ext3_i.i_orphan, &EXT3_SB(sb)->s_orphan);
		if (inode->i_nlink) {
			jfs_debug(2, "truncating inode %ld to %ld bytes\n",
				  inode->i_ino, inode->i_size);
			ext3_truncate(inode);
			nr_truncates++;
		} else {
			jfs_debug(2, "deleting unreferenced inode %ld\n",
				  inode->i_ino);
			nr_orphans++;
		}
		iput(inode);  /* The delete magic happens here! */
	}

#define PLURAL(x) (x), ((x)==1) ? "" : "s"

	if (nr_orphans)
		printk(KERN_INFO "EXT3-fs: %s: %d orphan inode%s deleted\n", 
		       kdevname(sb->s_dev), PLURAL(nr_orphans));
	if (nr_truncates)
		printk(KERN_INFO "EXT3-fs: %s: %d truncate%s cleaned up\n", 
		       kdevname(sb->s_dev), PLURAL(nr_truncates));
	sb->s_flags = s_flags; /* Restore MS_RDONLY status */
}

#define log2(n) ffz(~(n))

struct super_block * ext3_read_super (struct super_block * sb, void * data,
				      int silent)
{
	struct buffer_head * bh;
	struct ext3_super_block * es;
	unsigned long sb_block = 1;
	unsigned short resuid = EXT3_DEF_RESUID;
	unsigned short resgid = EXT3_DEF_RESGID;
	unsigned long logic_sb_block = 1;
	unsigned long offset = 0;
	unsigned long journal_inum = 0;

	kdev_t dev = sb->s_dev;
	int blocksize = BLOCK_SIZE;
	int hblock;
	int db_count;
	int i, j, err;
	int needs_recovery;
	
	/*
	 * See what the current blocksize for the device is, and
	 * use that as the blocksize.  Otherwise (or if the blocksize
	 * is smaller than the default) use the default.
	 * This is important for devices that have a hardware
	 * sectorsize that is larger than the default.
	 */
	blocksize = get_hardblocksize(dev);
	if( blocksize == 0 || blocksize < BLOCK_SIZE )
	  {
	    blocksize = BLOCK_SIZE;
	  }

	sb->u.ext3_sb.s_mount_opt = 0;
	set_opt3 (sb->u.ext3_sb.s_mount_opt, CHECK_NORMAL);

	if (!parse_options ((char *) data, &sb_block, &resuid, &resgid,
	    &sb->u.ext3_sb.s_mount_opt, &journal_inum, 0)) {
		sb->s_dev = 0;
		return NULL;
	}

	MOD_INC_USE_COUNT;
	lock_super (sb);
	set_blocksize (dev, blocksize);

	/*
	 * If the superblock doesn't start on a sector boundary,
	 * calculate the offset.  FIXME(eric) this doesn't make sense
	 * that we would have to do this.
	 */
	if (blocksize != BLOCK_SIZE) {
		logic_sb_block = (sb_block*BLOCK_SIZE) / blocksize;
		offset = (sb_block*BLOCK_SIZE) % blocksize;
	}

	if (!(bh = bread (dev, logic_sb_block, blocksize))) {
		sb->s_dev = 0;
		unlock_super (sb);
		printk ("EXT3-fs: unable to read superblock\n");
		MOD_DEC_USE_COUNT;
		return NULL;
	}
	/*
	 * Note: s_es must be initialized s_es as soon as possible because
	 * some ext3 macro-instructions depend on its value
	 */
	es = (struct ext3_super_block *) (((char *)bh->b_data) + offset);
	sb->u.ext3_sb.s_es = es;
	sb->s_magic = le16_to_cpu(es->s_magic);
	if (sb->s_magic != EXT3_SUPER_MAGIC) {
		if (!silent)
			printk ("VFS: Can't find an ext3 filesystem on dev "
				"%s.\n", bdevname(dev));
	failed_mount:
		sb->s_dev = 0;
		unlock_super (sb);
		if (bh)
			brelse(bh);
		MOD_DEC_USE_COUNT;
		return NULL;
	}

	if (le32_to_cpu(es->s_rev_level) == EXT3_GOOD_OLD_REV &&
	    (EXT3_HAS_COMPAT_FEATURE(sb, ~0U) ||
	     EXT3_HAS_RO_COMPAT_FEATURE(sb, ~0U) ||
	     EXT3_HAS_INCOMPAT_FEATURE(sb, ~0U)))
		printk("EXT3-fs warning: feature flags set on rev 0 fs, "
		       "running e2fsck is recommended\n");
	/*
	 * Check feature flags regardless of the revision level, since we
	 * previously didn't change the revision level when setting the flags,
	 * so there is a chance incompat flags are set on a rev 0 filesystem.
	 */
	if ((i = EXT3_HAS_INCOMPAT_FEATURE(sb, ~EXT3_FEATURE_INCOMPAT_SUPP))) {
		printk("EXT3-fs: %s: couldn't mount because of "
		       "unsupported optional features (%x).\n",
		       bdevname(dev), i);
		goto failed_mount;
	}
	if (!(sb->s_flags & MS_RDONLY) &&
	    (i = EXT3_HAS_RO_COMPAT_FEATURE(sb, ~EXT3_FEATURE_RO_COMPAT_SUPP))){
		printk("EXT3-fs: %s: couldn't mount RDWR because of "
		       "unsupported optional features (%x).\n",
		       bdevname(dev), i);
		goto failed_mount;
	}
	sb->s_blocksize_bits = 
		le32_to_cpu(EXT3_SB(sb)->s_es->s_log_block_size) + 10;
	sb->s_blocksize = 1 << sb->s_blocksize_bits;
	if (sb->s_blocksize != BLOCK_SIZE &&
	    (sb->s_blocksize == 1024 || sb->s_blocksize == 2048 ||
	     sb->s_blocksize == 4096)) {
		/*
		 * Make sure the blocksize for the filesystem is larger
		 * than the hardware sectorsize for the machine.
		 */
		hblock = get_hardblocksize(dev);
		if(    (hblock != 0)
		    && (sb->s_blocksize < hblock) )
		{
			printk("EXT3-fs: blocksize too small for device.\n");
			goto failed_mount;
		}

		brelse (bh);
		set_blocksize (dev, sb->s_blocksize);
		logic_sb_block = (sb_block*BLOCK_SIZE) / sb->s_blocksize;
		offset = (sb_block*BLOCK_SIZE) % sb->s_blocksize;
		bh = bread (dev, logic_sb_block, sb->s_blocksize);
		if(!bh) {
			printk("EXT3-fs: Couldn't read superblock on "
			       "2nd try.\n");
			goto failed_mount;
		}
		es = (struct ext3_super_block *) (((char *)bh->b_data) + offset);
		sb->u.ext3_sb.s_es = es;
		if (es->s_magic != le16_to_cpu(EXT3_SUPER_MAGIC)) {
			printk ("EXT3-fs: Magic mismatch, very weird !\n");
			goto failed_mount;
		}
	}
	if (le32_to_cpu(es->s_rev_level) == EXT3_GOOD_OLD_REV) {
		sb->u.ext3_sb.s_inode_size = EXT3_GOOD_OLD_INODE_SIZE;
		sb->u.ext3_sb.s_first_ino = EXT3_GOOD_OLD_FIRST_INO;
	} else {
		sb->u.ext3_sb.s_inode_size = le16_to_cpu(es->s_inode_size);
		sb->u.ext3_sb.s_first_ino = le32_to_cpu(es->s_first_ino);
		if (sb->u.ext3_sb.s_inode_size != EXT3_GOOD_OLD_INODE_SIZE) {
			printk ("EXT3-fs: unsupported inode size: %d\n",
				sb->u.ext3_sb.s_inode_size);
			goto failed_mount;
		}
	}
	sb->u.ext3_sb.s_frag_size = EXT3_MIN_FRAG_SIZE <<
				   le32_to_cpu(es->s_log_frag_size);
	if (sb->u.ext3_sb.s_frag_size)
		sb->u.ext3_sb.s_frags_per_block = sb->s_blocksize /
						  sb->u.ext3_sb.s_frag_size;
	else
		sb->s_magic = 0;
	sb->u.ext3_sb.s_blocks_per_group = le32_to_cpu(es->s_blocks_per_group);
	sb->u.ext3_sb.s_frags_per_group = le32_to_cpu(es->s_frags_per_group);
	sb->u.ext3_sb.s_inodes_per_group = le32_to_cpu(es->s_inodes_per_group);
	sb->u.ext3_sb.s_inodes_per_block = sb->s_blocksize /
					   EXT3_INODE_SIZE(sb);
	sb->u.ext3_sb.s_itb_per_group = sb->u.ext3_sb.s_inodes_per_group /
				        sb->u.ext3_sb.s_inodes_per_block;
	sb->u.ext3_sb.s_desc_per_block = sb->s_blocksize /
					 sizeof (struct ext3_group_desc);
	sb->u.ext3_sb.s_sbh = bh;
	if (resuid != EXT3_DEF_RESUID)
		sb->u.ext3_sb.s_resuid = resuid;
	else
		sb->u.ext3_sb.s_resuid = le16_to_cpu(es->s_def_resuid);
	if (resgid != EXT3_DEF_RESGID)
		sb->u.ext3_sb.s_resgid = resgid;
	else
		sb->u.ext3_sb.s_resgid = le16_to_cpu(es->s_def_resgid);
	sb->u.ext3_sb.s_mount_state = le16_to_cpu(es->s_state);
	sb->u.ext3_sb.s_addr_per_block_bits =
		log2 (EXT3_ADDR_PER_BLOCK(sb));
	sb->u.ext3_sb.s_desc_per_block_bits =
		log2 (EXT3_DESC_PER_BLOCK(sb));
	if (sb->s_magic != EXT3_SUPER_MAGIC) {
		if (!silent)
			printk ("VFS: Can't find an ext3 filesystem on dev "
				"%s.\n",
				bdevname(dev));
		goto failed_mount;
	}
	if (sb->s_blocksize != bh->b_size) {
		if (!silent)
			printk ("VFS: Unsupported blocksize on dev "
				"%s.\n", bdevname(dev));
		goto failed_mount;
	}

	if (sb->s_blocksize != sb->u.ext3_sb.s_frag_size) {
		printk ("EXT3-fs: fragsize %lu != blocksize %lu (not supported yet)\n",
			sb->u.ext3_sb.s_frag_size, sb->s_blocksize);
		goto failed_mount;
	}

	if (sb->u.ext3_sb.s_blocks_per_group > sb->s_blocksize * 8) {
		printk ("EXT3-fs: #blocks per group too big: %lu\n",
			sb->u.ext3_sb.s_blocks_per_group);
		goto failed_mount;
	}
	if (sb->u.ext3_sb.s_frags_per_group > sb->s_blocksize * 8) {
		printk ("EXT3-fs: #fragments per group too big: %lu\n",
			sb->u.ext3_sb.s_frags_per_group);
		goto failed_mount;
	}
	if (sb->u.ext3_sb.s_inodes_per_group > sb->s_blocksize * 8) {
		printk ("EXT3-fs: #inodes per group too big: %lu\n",
			sb->u.ext3_sb.s_inodes_per_group);
		goto failed_mount;
	}

	sb->u.ext3_sb.s_groups_count = (le32_to_cpu(es->s_blocks_count) -
				        le32_to_cpu(es->s_first_data_block) +
				       EXT3_BLOCKS_PER_GROUP(sb) - 1) /
				       EXT3_BLOCKS_PER_GROUP(sb);
	db_count = (sb->u.ext3_sb.s_groups_count + EXT3_DESC_PER_BLOCK(sb) - 1) /
		   EXT3_DESC_PER_BLOCK(sb);
	sb->u.ext3_sb.s_group_desc = kmalloc (db_count * sizeof (struct buffer_head *), GFP_KERNEL);
	if (sb->u.ext3_sb.s_group_desc == NULL) {
		printk ("EXT3-fs: not enough memory\n");
		goto failed_mount;
	}
	for (i = 0; i < db_count; i++) {
		sb->u.ext3_sb.s_group_desc[i] = bread (dev, logic_sb_block + i + 1,
						       sb->s_blocksize);
		if (!sb->u.ext3_sb.s_group_desc[i]) {
			for (j = 0; j < i; j++)
				brelse (sb->u.ext3_sb.s_group_desc[j]);
			kfree_s (sb->u.ext3_sb.s_group_desc,
				 db_count * sizeof (struct buffer_head *));
			printk ("EXT3-fs: unable to read group descriptors\n");
			goto failed_mount;
		}
	}
	if (!ext3_check_descriptors (sb)) {
		for (j = 0; j < db_count; j++)
			brelse (sb->u.ext3_sb.s_group_desc[j]);
		kfree_s (sb->u.ext3_sb.s_group_desc,
			 db_count * sizeof (struct buffer_head *));
		printk ("EXT3-fs: group descriptors corrupted !\n");
		goto failed_mount;
	}
	for (i = 0; i < EXT3_MAX_GROUP_LOADED; i++) {
		sb->u.ext3_sb.s_inode_bitmap_number[i] = 0;
		sb->u.ext3_sb.s_inode_bitmap[i] = NULL;
		sb->u.ext3_sb.s_block_bitmap_number[i] = 0;
		sb->u.ext3_sb.s_block_bitmap[i] = NULL;
	}
	sb->u.ext3_sb.s_loaded_inode_bitmaps = 0;
	sb->u.ext3_sb.s_loaded_block_bitmaps = 0;
	sb->u.ext3_sb.s_gdb_count = db_count;
	/*
	 * set up enough so that it can read an inode
	 */
	sb->s_dev = dev;
	sb->s_op = &ext3_sops;
	INIT_LIST_HEAD(&sb->u.ext3_sb.s_orphan); /* unlinked but open files */
	unlock_super (sb);

	err = 0;
	sb->s_root = 0;

	needs_recovery = (es->s_last_orphan != 0 || 
			  EXT3_HAS_INCOMPAT_FEATURE(sb, 
				    EXT3_FEATURE_INCOMPAT_RECOVER));

	/*
	 * The first inode we look at is the journal inode.  Don't try
	 * root first: it may be modified in the journal!
	 */
	if (!test_opt3(sb, NOLOAD) &&
	    EXT3_HAS_COMPAT_FEATURE(sb, EXT3_FEATURE_COMPAT_HAS_JOURNAL)) {
		if (ext3_load_journal(sb, es))
			goto error_out;
	} else if (journal_inum) {
		if (ext3_create_journal(sb, es, journal_inum))
			goto error_out;
	} else {
		if (!silent)
			printk (KERN_ERR 
				"ext3: No journal on filesystem on %s\n",
				kdevname(dev));
		goto error_out;
	}

	/* We have now updated the journal if required, so we can
	 * validate the data journaling mode. */
	switch (test_opt3(sb, DATA_FLAGS)) {
	case 0:
		/* No mode set, assume a default based on the journal
                   capabilities: ORDERED_DATA if the journal can
                   cope, else JOURNAL_DATA */
 		if (journal_check_available_features
 		    (EXT3_SB(sb)->s_journal, 0, 0, 
 		     JFS_FEATURE_INCOMPAT_REVOKE))
			set_opt3(EXT3_SB(sb)->s_mount_opt, ORDERED_DATA);
		else 
			set_opt3(EXT3_SB(sb)->s_mount_opt, JOURNAL_DATA);
		break;
                
	case EXT3_MOUNT_ORDERED_DATA:
	case EXT3_MOUNT_WRITEBACK_DATA:
 		if (!journal_check_available_features
 		    (EXT3_SB(sb)->s_journal, 0, 0, 
 		     JFS_FEATURE_INCOMPAT_REVOKE)) {
			printk(KERN_ERR "EXT3-fs: Journal does not support "
			       "requested data journaling mode\n");
			journal_release(EXT3_SB(sb)->s_journal);
			goto error_out;
		}
	default:
	}
		
	/*
	 * The journal_load will have done any necessary log recovery,
	 * so we can safely mount the rest of the filesystem now.  
	 */
	sb->s_root = d_alloc_root(iget(sb, EXT3_ROOT_INO), NULL);
	if (!sb->s_root) {
		journal_release(EXT3_SB(sb)->s_journal);
		goto error_out;
	}
			
	ext3_setup_super(sb, es);
	ext3_orphan_cleanup(sb, es);
	if (needs_recovery)
		printk (KERN_INFO "EXT3-fs: recovery complete.\n");
	ext3_mark_recovery_complete(sb, es);
	printk (KERN_INFO "EXT3-fs: mounted filesystem with %s data mode.\n",
		test_opt3(sb,DATA_FLAGS) == EXT3_MOUNT_JOURNAL_DATA ? "journal":
		test_opt3(sb,DATA_FLAGS) == EXT3_MOUNT_ORDERED_DATA ? "ordered":
		"writeback");
	
	return sb;

 error_out:
	sb->s_dev = 0;

	for (i = 0; i < db_count; i++)
		if (sb->u.ext3_sb.s_group_desc[i])
			brelse (sb->u.ext3_sb.s_group_desc[i]);
	kfree_s (sb->u.ext3_sb.s_group_desc,
		 db_count * sizeof (struct buffer_head *));
	brelse (bh);
	if (!silent)
		printk ("EXT3-fs: get root inode failed\n");
	/* We have to do a full forced inode invalidate because the VFS
           doesn't do one for us.  If we leave a journal inode in the
           inode cache (even if it has been released with iput()), then
           a future ext2 mount can trip over it. */
	invalidate_inodes(sb);
	MOD_DEC_USE_COUNT;
	return NULL;
}

static journal_t *ext3_get_journal(struct super_block *sb, int journal_inum)
{
	struct inode *journal_inode;
	journal_t *journal;

	/* First, test for the existence of a valid inode on disk.  Bad
	 * things happen if we iget() an unused inode, as the subsequent
	 * iput() will try to delete it. */

	journal_inode = iget(sb, journal_inum);
	if (!journal_inode) {
		printk("EXT3-fs: no journal found.\n");
		return NULL;
	}
	if (!journal_inode->i_nlink) {
		make_bad_inode(journal_inode);
		iput(journal_inode);
		printk("EXT3-fs: journal inode is deleted.\n");
		return NULL;
	}

	jfs_debug(2, "Journal inode found at %p: %ld bytes\n",
		  journal_inode, journal_inode->i_size);
	if (is_bad_inode(journal_inode) || !S_ISREG(journal_inode->i_mode)) {
		printk("EXT3-fs: invalid journal inode.\n");
		iput(journal_inode);
		return NULL;
	}

	journal = journal_init_inode(journal_inode);
	if (!journal)
		iput(journal_inode);
	if (sb->s_flags & MS_SYNCHRONOUS)
		journal->j_flags |= JFS_SYNC;
	return journal;
}

static int ext3_load_journal(struct super_block * sb,
			     struct ext3_super_block * es)
{
	journal_t *journal;
	int journal_inum = le32_to_cpu(es->s_journal_inum);
	int err;
	int really_read_only;
	
	really_read_only = is_read_only(sb->s_dev);
	
	/*
	 * Are we loading a blank journal or performing recovery after a
	 * crash?  For recovery, we need to check in advance whether we
	 * can get read-write access to the device.
	 */

	if (EXT3_HAS_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER)) {
		if (sb->s_flags & MS_RDONLY) {
			printk(KERN_ERR "EXT3-fs: WARNING: recovery required on readonly filesystem.\n");
			if (really_read_only) {
				printk(KERN_ERR "EXT3-fs: write access unavailable, cannot proceed.\n");
				return -EROFS;
			}
			printk (KERN_ERR "EXT3-fs: write access will be enabled during recovery.\n");
		}
	}
	
	if (!(journal = ext3_get_journal(sb, journal_inum)))
		return -EINVAL;

	if (!really_read_only && test_opt3(sb, UPDATE_JOURNAL)) {
		err = journal_update_format(journal);
		if (err)  {
			printk(KERN_ERR "EXT3-fs: error updating journal.\n");
			journal_release(journal);
			return err;
		}
	}

	if (!EXT3_HAS_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER))
		journal_wipe(journal, !really_read_only);

	err = journal_load(journal);
	if (err) {
		printk(KERN_ERR "EXT3-fs: error loading journal.\n");
		journal_release(journal);
		return err;
	}

	EXT3_SB(sb)->s_journal = journal;
	ext3_clear_journal_err(sb, es);
	return 0;
}

static int ext3_create_journal(struct super_block * sb,
			       struct ext3_super_block * es,
			       int journal_inum)
{
	journal_t *journal;
		
	if (sb->s_flags & MS_RDONLY) {
		printk("EXT3-fs: readonly filesystem when trying to create journal.\n");
		return -EROFS;
	}
	
	if (!(journal = ext3_get_journal(sb, journal_inum)))
		return -EINVAL;

	if (journal_create(journal)) {
		printk("EXT3-fs: error creating journal.\n");
		journal_release(journal);
		return -EIO;
	}

	EXT3_SB(sb)->s_journal = journal;
	if (sb->s_flags & MS_SYNCHRONOUS)
		journal->j_flags |= JFS_SYNC;
	
	ext3_update_fs_rev(sb);
	EXT3_SET_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER);
	EXT3_SET_COMPAT_FEATURE(sb, EXT3_FEATURE_COMPAT_HAS_JOURNAL);
	
	es->s_journal_inum = cpu_to_le32(journal_inum);
	sb->s_dirt = 1;

	/* Make sure we flush the recovery flag to disk. */
	ext3_commit_super(sb, es, 1);
	
	return 0;
}

static void ext3_commit_super (struct super_block * sb,
			       struct ext3_super_block * es, 
			       int sync)
{
	es->s_wtime = cpu_to_le32(CURRENT_TIME);
	mark_buffer_dirty(sb->u.ext3_sb.s_sbh, 1);
	if (sync) {
		ll_rw_block(WRITE, 1, &sb->u.ext3_sb.s_sbh);
		wait_on_buffer(sb->u.ext3_sb.s_sbh);
	}
}


/* 
 * Have we just finished recovery?  If so, and if we are mounting (or
 * remounting) the filesystem readonly, then we will end up with a
 * consistent fs on disk.  Record that fact.  
 */
static void ext3_mark_recovery_complete(struct super_block * sb,
					struct ext3_super_block * es)
{
	journal_flush(EXT3_SB(sb)->s_journal);
	if (EXT3_HAS_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER) &&
	    sb->s_flags & MS_RDONLY) {
		EXT3_CLEAR_INCOMPAT_FEATURE(sb, 
					    EXT3_FEATURE_INCOMPAT_RECOVER);
		sb->s_dirt = 0;
		ext3_commit_super(sb, es, 1);
	}
}

/*
 * If we are mounting (or read-write remounting) a filesystem whose journal
 * has recorded an error from a previous lifetime, move that error to the
 * main filesystem now. 
 */
static void ext3_clear_journal_err(struct super_block * sb,
				   struct ext3_super_block * es)
{
	journal_t *journal;
	int j_errno;
	
	journal = EXT3_SB(sb)->s_journal;
	
	/*
	 * Now check for any error status which may have been recorded in the 
	 * journal by a prior ext3_panic()
	 */

	j_errno = journal_errno(journal);
	if (j_errno) {
		ext3_warning(sb, __FUNCTION__, 
			     "detected journal error %d from previous mount", 
			     j_errno);

		sb->u.ext3_sb.s_mount_state |= EXT3_ERROR_FS;
		sb->u.ext3_sb.s_es->s_state |= cpu_to_le16(EXT3_ERROR_FS);
		ext3_commit_super (sb, es, 1);

		journal_clear_err(journal);
	}
}
	

/*
 * In the second extended file system, it is not necessary to
 * write the super block since we use a mapping of the
 * disk super block in a buffer.
 *
 * However, this function is still used to set the fs valid
 * flags to 0.  We need to set this flag to 0 since the fs
 * may have been checked while mounted and e2fsck may have
 * set s_state to EXT3_VALID_FS after some corrections.
 */

void ext3_write_super (struct super_block * sb)
{
	tid_t wait_tid;

	sb->s_dirt = 0;
	
	if (!(sb->s_flags & MS_RDONLY)) {
		journal_t *journal;
		
		journal = EXT3_SB(sb)->s_journal;

		if (journal->j_running_transaction) {
			wait_tid = journal->j_running_transaction->t_tid;
			log_start_commit(journal, journal->j_running_transaction);
			log_wait_commit(journal, wait_tid);
		} else if (journal->j_committing_transaction)
			log_wait_commit(journal, journal->j_committing_transaction->t_tid);
	}
}

int ext3_remount (struct super_block * sb, int * flags, char * data)
{
	struct ext3_super_block * es;
	unsigned short resuid = sb->u.ext3_sb.s_resuid;
	unsigned short resgid = sb->u.ext3_sb.s_resgid;
	unsigned long new_mount_opt;
	unsigned long tmp;

	/*
	 * Allow the "check" option to be passed as a remount option.
	 */
	new_mount_opt = sb->u.ext3_sb.s_mount_opt;
	if (!parse_options (data, &tmp, &resuid, &resgid,
			    &new_mount_opt, &tmp, 1))
		return -EINVAL;

	if (new_mount_opt & EXT3_MOUNT_ABORT)
		ext3_abort(sb, __FUNCTION__, "Abort forced by user");
	
	sb->u.ext3_sb.s_mount_opt = new_mount_opt;
	sb->u.ext3_sb.s_resuid = resuid;
	sb->u.ext3_sb.s_resgid = resgid;
	es = sb->u.ext3_sb.s_es;
	if ((*flags & MS_RDONLY) == (sb->s_flags & MS_RDONLY))
		return 0;

	if (sb->u.ext3_sb.s_mount_opt & EXT3_MOUNT_ABORT)
		return -EROFS;
	
	if (*flags & MS_RDONLY) {
		/* 
		 * First of all, the unconditional stuff we have to do
		 * to disable replay of the journal when we next remount
		 */
		sb->s_flags |= MS_RDONLY;

		/*
		 * OK, test if we are remounting a valid rw partition
		 * rdonly, and if so set the rdonly flag and then mark the
		 * partition as valid again.  
		 */
		if (!(es->s_state & cpu_to_le16(EXT3_VALID_FS)) &&
		    (sb->u.ext3_sb.s_mount_state & EXT3_VALID_FS))
			es->s_state = cpu_to_le16(sb->u.ext3_sb.s_mount_state);

		ext3_mark_recovery_complete(sb, es);
	}
	else {
		int ret;
		if ((ret = EXT3_HAS_RO_COMPAT_FEATURE(sb,
				       ~EXT3_FEATURE_RO_COMPAT_SUPP))) {
			printk("EXT3-fs: %s: couldn't remount RDWR because of "
			       "unsupported optional features (%x).\n",
			       bdevname(sb->s_dev), ret);
			return -EROFS;
		}

		/*
		 * Mounting a RDONLY partition read-write, so reread and
		 * store the current valid flag.  (It may have been changed
		 * by e2fsck since we originally mounted the partition.)
		 */
		ext3_clear_journal_err(sb, es);
		sb->u.ext3_sb.s_mount_state = le16_to_cpu(es->s_state);
		sb->s_flags &= ~MS_RDONLY;
		ext3_setup_super (sb, es);
		sb->s_dirt = 1;
	}
	return 0;
}

/* Quota is being set up on this volume.  Initialise the quota files for
 * journaling and set the required quota flags. */

static void ext3_init_dquot(struct vfsmount *vfsmnt)
{
	int i;
	struct file *filp;
	struct inode *inode;
	int err;

	/* Set the quota system to "writethrough", so that all writes to
	 * a dquot are immediately written to the underlying quota file */
	jfs_debug(2, "setting DQUOT_WRITETHROUGH\n");
	vfsmnt->mnt_dquot.flags |= DQUOT_WRITETHROUGH;

	/* Check the state of all of the established quota inodes to
	 * make sure that we have requested data journaling on them,
	 * even if we are using one of the data modes which does not
	 * automatically journal all data. */
	for (i=0; i<MAXQUOTAS; i++) { 
		filp = vfsmnt->mnt_dquot.files[i];
		if (filp && filp->f_dentry && filp->f_dentry->d_inode) {
			inode = filp->f_dentry->d_inode;
			if (inode->u.ext3_i.i_flags & EXT3_JOURNAL_DATA_FL)
				continue;
			
			err = ext3_change_inode_journal_flag(inode, 1);
			if (err) 
				ext3_warning(vfsmnt->mnt_sb,
					     __FUNCTION__,
					     "error %d setting quota "
					     "inode journal stat",
					     err);
		}
	}
}


static struct file_system_type ext3_fs_type = {
	"ext3", 
	FS_REQUIRES_DEV /* | FS_IBASKET */,	/* ibaskets have unresolved bugs */
        ext3_read_super, 
	NULL
};

__initfunc(int init_ext3_fs(void))
{
        return register_filesystem(&ext3_fs_type);
}

#ifdef MODULE
EXPORT_NO_SYMBOLS;

int init_module(void)
{
	return init_ext3_fs();
}

void cleanup_module(void)
{
        unregister_filesystem(&ext3_fs_type);
}

#endif

int ext3_statfs (struct super_block * sb, struct statfs * buf, int bufsiz)
{
	unsigned long overhead;
	struct statfs tmp;
	int i;

	if (test_opt3 (sb, MINIX_DF))
		overhead = 0;
	else {
		/*
		 * Compute the overhead (FS structures)
		 */

		/*
		 * All of the blocks before first_data_block are
		 * overhead
		 */
		overhead = le32_to_cpu(sb->u.ext3_sb.s_es->s_first_data_block);

		/*
		 * Add the overhead attributed to the superblock and
		 * block group descriptors.  If the sparse superblocks
		 * feature is turned on, then not all groups have this.
		 */
		for (i = 0; i < EXT3_SB(sb)->s_groups_count; i++)
			overhead += ext3_bg_has_super(sb, i) +
				ext3_bg_num_gdb(sb, i);

		/*
		 * Every block group has an inode bitmap, a block
		 * bitmap, and an inode table.
		 */
		overhead += (sb->u.ext3_sb.s_groups_count *
			     (2 + sb->u.ext3_sb.s_itb_per_group));
	}

	tmp.f_type = EXT3_SUPER_MAGIC;
	tmp.f_bsize = sb->s_blocksize;
	tmp.f_blocks = le32_to_cpu(sb->u.ext3_sb.s_es->s_blocks_count) - overhead;
	tmp.f_bfree = ext3_count_free_blocks (sb);
	tmp.f_bavail = tmp.f_bfree - le32_to_cpu(sb->u.ext3_sb.s_es->s_r_blocks_count);
	if (tmp.f_bfree < le32_to_cpu(sb->u.ext3_sb.s_es->s_r_blocks_count))
		tmp.f_bavail = 0;
	tmp.f_files = le32_to_cpu(sb->u.ext3_sb.s_es->s_inodes_count);
	tmp.f_ffree = ext3_count_free_inodes (sb);
	tmp.f_namelen = EXT3_NAME_LEN;
	return copy_to_user(buf, &tmp, bufsiz) ? -EFAULT : 0;
}
