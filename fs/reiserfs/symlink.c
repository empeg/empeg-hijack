/*
 * Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */
#ifdef __KERNEL__

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/reiserfs_fs.h>
#include <asm/uaccess.h>

#else

#include "nokernel.h"

#endif


static int reiserfs_readlink(struct dentry *, char *, int);
static struct dentry * reiserfs_follow_link(struct dentry *, struct dentry *, unsigned int);


struct inode_operations reiserfs_symlink_inode_operations = {
	NULL,			/* file-operations */
	NULL,			/* create */
	NULL,			/* lookup */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	reiserfs_readlink,	/* readlink */
	reiserfs_follow_link,	/* follow_link */
	NULL,			/* readpage */
	NULL,			/* writepage */
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL,			/* permission */
	NULL,			/* smap */
	NULL,			/* updatepage */
	NULL			/* revalidate */
	
};


static int read_symlink (struct inode * inode, char * buf, int memmode)
{
  struct key key;
  int repeat, pos_in_item, chars, len = inode->i_size;
  struct path path;
  struct item_head * ih;

  init_path (&path);
  copy_key (&key, INODE_PKEY(inode));
  key.k_offset = 1;
  key.k_uniqueness = TYPE_DIRECT;

  while (len > 0) {
    if (search_for_position_by_key (inode->i_sb, &key, &path, &pos_in_item, &repeat) == POSITION_NOT_FOUND) {
      reiserfs_warning ("vs-17000: read_symlink: symlink item not found");
      return -EIO ;
    }
    ih = PATH_PITEM_HEAD(&path);
    chars = ih->ih_item_len - pos_in_item;
    if (memmode == REISERFS_KERNEL_MEM)
      memcpy (buf, B_I_PITEM(PATH_PLAST_BUFFER(&path),ih) + pos_in_item, chars);
    else
      copy_to_user (buf, B_I_PITEM(PATH_PLAST_BUFFER(&path),ih) + pos_in_item, chars);
    buf += chars;
    key.k_offset += chars;
    len -= chars;

#ifdef REISERFS_CHECK
    if (len < 0)
      reiserfs_panic (inode->i_sb, "vs-17005: read_symlink: too many bytes read from symlink (%d). Must be %d", inode->i_size - len,
		      inode->i_size);
#endif

    
  }
  *buf = 0;
  decrement_counters_in_path(&path);
  return 0 ;
}


static struct dentry * reiserfs_follow_link (struct dentry * dentry, struct dentry * base, unsigned int follow)
{
  struct inode * inode = dentry->d_inode;
  char * buf;
  int ret ;

  buf = reiserfs_kmalloc (inode->i_size + 1, GFP_KERNEL, inode->i_sb);
  if (buf == 0) {
    dput (base);
    return ERR_PTR(-ENOMEM);
  }

  ret = read_symlink (inode, buf, REISERFS_KERNEL_MEM);
  if (ret != 0) {
    reiserfs_kfree (buf, inode->i_size + 1, inode->i_sb);
    dput (base);
    return ERR_PTR(-EIO) ;
  }

  UPDATE_ATIME(inode);
  base = lookup_dentry (buf, base, follow);
  reiserfs_kfree (buf, inode->i_size + 1, inode->i_sb);

  return base;
}


static int reiserfs_readlink (struct dentry * dentry, char * buffer, int buflen)
{
  struct inode * inode = dentry->d_inode;

  if (read_symlink (inode, buffer, REISERFS_USER_MEM) != 0) {
    return -EIO ;
  }
  return inode->i_size;
}





