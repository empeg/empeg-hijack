/*
 * this is to be included by all kernel files if __KERNEL__ undefined
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <asm/types.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/vfs.h>
#include <time.h>

#ifndef __alpha__
#include <asm/bitops.h>
#endif

/*#define kdev_t dev_t*/

#include "inode.h"
#include "io.h"
#include "sb.h"
#include "misc.h"
#include "reiserfs_fs.h"
#include "reiserfs_fs_sb.h"

#define make_bad_inode(i) {;}
#define printk printf
#define le16_to_cpu(x) ((__u16)x)
#define cpu_to_le16(x) ((__u16)x)
#define le32_to_cpu(x) ((__u32)x)
#define cpu_to_le32(x) ((__u32)x)
#define copy_from_user memcpy
#define copy_to_user memcpy
#define put_user(b,a) ((*(a))=b)
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define BUF_CLEAN 0
#define kmalloc(a,b) getmem(a)
#define kfree freemem
#define update_vm_cache(a,b,c,d)
#define wait_on_buffer __wait_on_buffer
#define NODEV 0
extern struct inode_operations chrdev_inode_operations;
extern struct inode_operations blkdev_inode_operations;

#define page_address(page) 0
void init_fifo(struct inode * inode);
#define buffer_req(bh) 0
//#define kdevname(a) "kdevname"
#define kdev_t_to_nr(a) a
#define to_kdev_t(a) a
#define clear_inode(a)
#define PAGE_SIZE 4096
#define d_add(a,b) a->d_inode = b
#define d_instantiate(a,b) a->d_inode = b
#define d_delete(a)
#define d_move(a,b)
#define down(a)
#define up(a)
#define MOD_DEC_USE_COUNT
#define MOD_INC_USE_COUNT
#define lock_super(a)
#define unlock_super(a)
#define set_blocksize(a,b)
#define d_alloc_root(a,b) 0
/* #define file_fsync 0 */
#define set_writetime(a,b)

#define reiserfs_bread(a,b,c,d) bread(a,b,c)
/* #define reiserfs_getblk(a,b,c,d)  getblk(a,b,c) */
extern struct inode_operations reiserfs_dir_inode_operations;
extern struct inode_operations reiserfs_symlink_inode_operations;
struct buffer_head * reiserfs_getblk (kdev_t n_dev, int n_block, int n_size, int * p_n_repeat);

#ifdef REISERFS_FSCK
#undef REISERFS_CHECK
#endif

/* fs.h */
#define BLOCK_SIZE 1024
#define READ 0
#define WRITE 1
#define MS_RDONLY	 1
#define UPDATE_ATIME(inode)
#define MAJOR(dev) ((dev)>>8)
#define MINOR(dev) ((dev) & 0xff)

#define CURRENT_TIME (time(NULL))

void set_super(struct super_block *s) ;
int file_fsync(struct file *filp, struct dentry *dentry) ;
int reiserfs_file_release(struct inode *p_s_inode, struct file *p_s_filp) ;
int preserve_trace_print_srs(struct super_block *s) ;


//
// fs/reiserfs/buffer.c
//
#define reiserfs_file_buffer(bh,state) do {} while (0)
#define reiserfs_journal_end_io 0

//
// fs/reiserfs/journal.c
//
#define journal_mark_dirty(th,s,bh) mark_buffer_dirty (bh, 1)
#define journal_mark_dirty_nolog(th,s,bh) mark_buffer_dirty (bh, 1)
#define mark_buffer_journal_new(bh) mark_buffer_dirty (bh, 1)

#define reiserfs_update_inode_transaction(i) do {} while (0)
#define reiserfs_inode_in_this_transaction(i) 1
#define reiserfs_commit_for_inode(i) do {} while(0)

extern inline int flush_old_commits (struct super_block * s, int i)
{
  return 0;
}

#define journal_begin(th,s,n) do {int fu = n;fu++;} while (0)
#define journal_release(th,s) do {} while (0)
#define journal_release_error(th,s) do {} while (0)
#define journal_init(s) 0
#define journal_end(th,s,n) do {s=0;} while (0)
#define buffer_journaled(bh) 0
#define journal_lock_dobalance(s) do {} while (0)
#define journal_unlock_dobalance(s) do {} while (0)
#define journal_transaction_should_end(th,n) 1
#define push_journal_writer(s) 1
#define pop_journal_writer(n) do {} while (0)
#define journal_end_sync(th,s,n) do {} while (0)
#define journal_mark_freed(th,s,n) do {} while (0)
#define reiserfs_in_journal(a,b,c,d,e,f) 0
#define flush_async_commits(s,n) do {} while (0)

//
// fs/reiserfs/resize.c
//
#define reiserfs_resize(s,n) do {} while (0)
#define simple_strtoul strtol


