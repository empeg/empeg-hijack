/*
 * Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */
#ifdef __KERNEL__

#include <linux/string.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/reiserfs_fs.h>
#include <linux/stat.h>
#include <asm/uaccess.h>

#else

#include "nokernel.h"

#endif


extern struct key  MIN_KEY;

static ssize_t reiserfs_dir_read  (struct file * filp, char * buf, size_t count, loff_t * ppos)
{
  return -EISDIR;
}

/*static loff_t reiserfs_llseek(struct file *file, loff_t offset, int origin);*/
static int reiserfs_readdir (struct file *, void *, filldir_t);
int reiserfs_dir_fsync(struct file *filp, struct dentry *dentry)  ;

static struct file_operations reiserfs_dir_operations = {
	NULL/*reiserfs_llseek*/,	/* lseek */
	reiserfs_dir_read,	/* read */
	NULL,			/* write */
	reiserfs_readdir,	/* readdir */
	NULL,			/* poll */
	NULL,			/* ioctl */
	NULL,			/* mmap */
	NULL,			/* open */
	NULL,			/* flush */
	NULL,			/* release */
	reiserfs_dir_fsync,	/* fsync */ 
	NULL, 			/* fasync */
	NULL,			/* check_media_change */
	NULL,			/* revalidate */
	NULL			/* lock */
};

/*
 * directories can handle most operations...
 */
struct inode_operations reiserfs_dir_inode_operations = {
	&reiserfs_dir_operations,	/* default_file_ops */
	reiserfs_create,		/* create */
	reiserfs_lookup,		/* lookup */
	reiserfs_link,			/* link */
	reiserfs_unlink,		/* unlink */
	reiserfs_symlink,		/* symlink */
	reiserfs_mkdir,			/* mkdir */
	reiserfs_rmdir,			/* rmdir */
	reiserfs_mknod,			/* mknod */
	reiserfs_rename,		/* rename */
	NULL,				/* readlink */
	NULL,				/* follow_link */
	NULL,				/* readpage */
	NULL,				/* writepage */
	NULL,				/* bmap */
	NULL,				/* truncate */
	NULL,				/* permission */
	NULL,				/* smap */
	NULL,				/* updatepage */
	NULL				/* revalidate */
};

int reiserfs_dir_fsync(struct file *filp, struct dentry *dentry) {
  struct reiserfs_transaction_handle th ;
  if (!dentry || !dentry->d_inode) {
     return file_fsync(filp, dentry) ;
  }
  /* ret = file_fsync(filp, dentry) ; we don't need this */

  /* if any changes were made to the dir, this will catch them */
  if (reiserfs_inode_in_this_transaction(dentry->d_inode)) {
    journal_begin(&th, dentry->d_inode->i_sb, 1) ;
    journal_end_sync(&th, dentry->d_inode->i_sb, 1) ;
  } else {
    reiserfs_commit_for_inode(dentry->d_inode) ;
  }
  return 0 ;
}


/* read ahead can be used also for directory */
static int reiserfs_readdir (struct file * filp, void * dirent, filldir_t filldir)
{
  struct inode *inode = filp->f_dentry->d_inode;
  struct key pos_key;	/* key of current position in the directory (key of directory entry) */
  struct path path_to_entry;
  struct buffer_head * bh;
  int item_num, entry_num;
  int repeat;

  struct key * rkey;
  struct item_head * ih;
  int search_res;

  init_path (&path_to_entry);

  if (!inode || !inode->i_sb || !S_ISDIR (inode->i_mode))
    return -EBADF;

  /* form key for search the next directory entry using f_pos field of file structure  */
  copy_key (&pos_key, INODE_PKEY (inode));
  pos_key.k_offset = (filp->f_pos) ? (filp->f_pos) : DOT_OFFSET;
  pos_key.k_uniqueness = DIRENTRY_UNIQUENESS;

  while (1) {
    /* search the directory item, containing entry with specified key */
    search_res = search_by_entry_key (inode->i_sb, &pos_key, &path_to_entry, &entry_num, &repeat);

    bh = PATH_PLAST_BUFFER (&path_to_entry);
    item_num = PATH_LAST_POSITION (&path_to_entry);
    ih = B_N_PITEM_HEAD (bh, item_num);
		
#ifdef REISERFS_CHECK
    /* we must have found item, that is item of this directory, */
    if (COMP_SHORT_KEYS (&pos_key, B_N_PKEY (bh, item_num)))
      reiserfs_panic (inode->i_sb, "vs-9000: reiserfs_readdir: can not find directory item (%lu %lu)",
		      pos_key.k_dir_id, pos_key.k_objectid);
      
    if (item_num > B_NR_ITEMS (bh) - 1)
      reiserfs_panic (inode->i_sb, "vs-9005: reiserfs_readdir: item_num == %d, item amount == %d",
		      item_num, B_NR_ITEMS (bh));
      
    /* and entry must be not more than number of entries in the item */
    if (I_ENTRY_COUNT (ih) < entry_num)
      reiserfs_panic (inode->i_sb, "vs-9010: reiserfs_readdir: entry number is too big %d (%d)",
		      entry_num, I_ENTRY_COUNT (ih));
#endif	/* REISERFS_CHECK */

    if (search_res == POSITION_FOUND || entry_num < I_ENTRY_COUNT (ih)) {
      /* go through all entries in the directory item beginning from the entry, that has been found */
      struct reiserfs_de_head * deh = B_I_DEH (bh, ih) + entry_num;

      for (; entry_num < I_ENTRY_COUNT (ih); entry_num ++, deh ++) {
	if (!de_visible (deh))
	  /* it is hidden entry */
	  continue;
	if (filldir (dirent, B_I_DEH_ENTRY_FILE_NAME (bh, ih, deh), I_DEH_N_ENTRY_FILE_NAME_LENGTH (ih, deh, entry_num),
		     deh->deh_offset,
		     DEH_OBJECTID (deh)) < 0) {
	  pathrelse (&path_to_entry);
	  return 0;
	}

	/* set position in directory to the next entry */
	filp->f_pos = deh->deh_offset + 1;

      } /* for */
    }
    /* item we went through is last item of node. Using right
       delimiting key check is it directory end */
    if (item_num == B_NR_ITEMS (bh) - 1) {
      rkey = get_rkey (&path_to_entry, inode->i_sb);
      if (! COMP_KEYS (rkey, &MIN_KEY)) {
#ifdef REISERFS_CHECK
	printk ("reiserfs_readdir could not calculate rigth delimiting key (path has been changed)\n");
#endif
	/* set pos_key to key, that is the smallest and greater
	   that key of the last entry in the item */
	pos_key.k_offset = filp->f_pos;
	continue;
      }

      if ( COMP_SHORT_KEYS (rkey, &pos_key)) {
	/* end of the directory */
	pathrelse (&path_to_entry);
	return 0;
      }
      /* directory continues in the right neighboring block */
      copy_key (&pos_key, rkey);
      continue;
    }

    /* directory item is not a last item of the node. End of the directory is achieved */
    pathrelse (&path_to_entry);
    return 0;
  } /* while */

  return 0;
}


#if 0
static loff_t reiserfs_llseek(struct file *file, loff_t offset, int origin)
{
  long long retval;

  switch (origin) {
  case 2:
    offset += file->f_dentry->d_inode->i_size;
    break;
  case 1:
    offset += file->f_pos;
  }
  retval = -EINVAL;
 /* if (offset >= 0) {*/
    if (offset != file->f_pos) {
      file->f_pos = offset;
      file->f_reada = 0;
      file->f_version = ++event;
    }
    retval = offset;
/*  }*/
  return retval;
}
#endif




















