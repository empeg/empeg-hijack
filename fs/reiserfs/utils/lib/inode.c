#include "nokernel.h"

struct fs_struct fss = {0};
struct task_struct cur_task = {0, 0, 0, 0, 0, 0, &fss, {{0,},}};

int generic_readpage(struct file * file, struct page * page)
{
  return 0;
}

int generic_file_mmap (struct file * file, struct vm_area_struct * vma)
{
  return 0;
}

int reiserfs_sync_file (struct file * p_s_filp, struct dentry * p_s_dentry)
{
  return 0;
}

void wait_buffer_until_released (struct buffer_head * bh)
{
}

int schedule (void)
{
  return 0;
}

int fsuser (void)
{
  return 0;
}

int is_subdir (struct dentry * old, struct dentry * new)
{
  return 0;
}

void sleep_on (struct wait_queue ** w)
{
}

void wake_up (struct wait_queue ** w)
{
}


struct inode_operations chrdev_inode_operations = {0,};
struct inode_operations blkdev_inode_operations = {0,};
/*struct inode_operations reiserfs_dir_inode_operations = {0,};*/
struct inode_operations reiserfs_symlink_inode_operations = {0,};
struct super_operations reiserfs_sops = 
{
  reiserfs_read_inode,
  reiserfs_write_inode,
  NULL,				/* put_inode*/
  reiserfs_delete_inode,
  reiserfs_notify_change,
  reiserfs_put_super,
  reiserfs_write_super,
  reiserfs_statfs,
  reiserfs_remount,
  NULL, 				/* clear_inode */
  NULL				/* umount_begin */
};



void init_fifo(struct inode * inode)
{
}


#define NR_INODES 10

struct inode * first_inode;
int inodes = 0;

struct inode * find_inode (unsigned long ino)
{
  struct inode * inode;

  inode = first_inode;
  if (inode == 0)
    return 0;

  while (1) {
    if (inode->i_ino == ino) {
      inode->i_count ++;
      return inode;
    }
    inode = inode->i_next;
    if (inode == first_inode)
      break;
  }
  return 0;
}


struct inode * get_empty_inode (void)
{
  struct inode * inode, * prev, * next;

  if (inodes == NR_INODES) {
    first_inode->i_sb->s_op->write_inode (first_inode);

    /* set all but i_next and i_prev to 0 */
    next = first_inode->i_next;
    prev = first_inode->i_prev;
    memset (first_inode, 0, sizeof (struct inode));
    first_inode->i_next = next;
    first_inode->i_prev = prev;
    
    /* move to end of list */
    first_inode = first_inode->i_next;
    return first_inode->i_prev;
  }
  /* allocate new inode */
  inode = getmem (sizeof (struct inode));
  if (!inode)
    return 0;

  /* add to end of list */
  if (first_inode) {
    inode->i_prev = first_inode->i_prev;
    inode->i_next = first_inode;
    first_inode->i_prev->i_next = inode;
    first_inode->i_prev = inode;
  } else {
    first_inode = inode->i_next = inode->i_prev = inode;
  }
  inode->i_count = 1;
  return inode;
}


void insert_inode_hash (struct inode * inode)
{
}

struct inode * get_new_inode (struct super_block *sb, unsigned long ino)
{
  struct inode * inode;

  inode = get_empty_inode ();
  if (inode) {
    inode->i_sb = sb;
    inode->i_ino = ino;
    //inode->i_count = 1;
    sb->s_op->read_inode (inode);
    return inode;
  }
  return 0;
}


struct inode * iget (struct super_block *sb, unsigned long ino)
{
  struct inode * inode;

  inode = find_inode (ino);
  if (inode)
    return inode;
  return get_new_inode (sb, ino);
}


void iput (struct inode * inode)
{
    if (inode) {
	if (inode->i_count == 0)
	    die ("iput: can not free free inode");

	if (inode->i_op->default_file_ops->release)
	  inode->i_op->default_file_ops->release (inode, 0);
	if (inode->i_sb->s_op->put_inode)
	  inode->i_sb->s_op->put_inode (inode);
	inode->i_count --;

	if (inode->i_nlink == 0) {
	    inode->i_sb->s_op->delete_inode (inode);
	    return;
	}
	if (inode->i_state & I_DIRTY) {
	    inode->i_sb->s_op->write_inode (inode);
	    inode->i_state &= ~I_DIRTY;
	}
    }
}


void sync_inodes (void)
{
  struct inode * inode, * tmp;

  inode = first_inode;
  if (inode == 0)
    return;

  while (1) {
    if (inode->i_dirt)
      inode->i_sb->s_op->write_inode (inode);

    tmp = inode;
    inode = inode->i_next;

    inode->i_prev = tmp->i_prev;
    tmp->i_prev->i_next = inode;
    
    freemem (tmp);
    if (inode == tmp)
      break;
  }
  return;
}


//
// arch/i386/kernel/process.c
//
int kernel_thread(int (*fn)(void *), void * arg, unsigned long flags)
{
  return 0;
}



