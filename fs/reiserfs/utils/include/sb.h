/*
struct reiserfs_sb_info {
  struct reiserfs_super_block * s_rs;
  struct buffer_head * s_sbh;
  struct buffer_head ** s_ap_true_bitmap;
  struct buffer_head ** s_ap_cautious_bitmap;
  int s_mount_state;
  unsigned long s_mount_opt;
  int s_direct2indirect;
  int s_bmaps;
  int s_suspected_recipient_count;
  void (*unpreserve)(struct super_block * s, struct buffer_head * bh);
};
*/
/*
typedef struct {
  volatile unsigned int lock ;
} spinlock_t ;
*/
/*
typedef struct {
  volatile unsigned int lock ;
  int previous ;
} rwlock_t ;
*/
/*typedef int task_queue ;*/

struct super_operations {
  void (*read_inode) (struct inode *);
  void (*write_inode) (struct inode *);
  void (*put_inode) (struct inode *);
  void (*delete_inode) (struct inode *);
  int (*notify_change) (struct dentry *, struct iattr *);
  void (*put_super) (struct super_block *);
  void (*write_super) (struct super_block *);
  int (*statfs) (struct super_block *, struct statfs *, int);
  int (*remount_fs) (struct super_block *, int *, char *);
  void (*clear_inode) (struct inode *);
  void (*umount_begin) (struct super_block *);
};

#include "reiserfs_fs_sb.h"

struct super_block {
  kdev_t s_dev;
  unsigned long s_blocksize;
  int s_blocksize_bits;
  int s_dirt;
  int s_flags;
  struct dentry * s_root;
  struct super_operations * s_op;
  union {
    struct reiserfs_sb_info reiserfs_sb;
  } u;
};
#if 0
#define NOTAIL 0  /* mount option -o notail */
#define NOPRESERVE 1 /*           -o nopreserve */ 
#define GENERICREAD 2 /*          -o genericread */
#define dont_have_tails(s) 0
#define dont_preserve(s) 1
#define use_genericread(s) 0

#define is_buffer_suspected_recipient(a) 0
#define is_buffer_preserved(a) 0
#define is_buffer_unwritten(a) 0
#define add_to_preserve(a,b) 
#define unmark_suspected_recipient(a,b)
#define mark_suspected_recipient(a,b)
#define preserve_shifted(a,b,c,d,e)
#define get_space_from_preserve_list(a) 0
#define preserve_invalidate(a,b,c)
#define mark_buffer_unwritten(a)
#define maybe_free_preserve_list(a) 1
#define ready_preserve_list(a,b) 0
#define preserve_trace_release_bitmap(a) do {} while (0)
#define preserve_trace_init_bitmap(a) do {} while (0)

extern int unpreserve;

static void reiserfs_show_buffers(kdev_t a) {}
static void reiserfs_refile_buffer(struct buffer_head * bh) {}
static void mark_suspected_recipients_dirty(kdev_t a) {}
static void invalidate_reiserfs_buffers(kdev_t a) {}
static int reiserfs_notify_change(struct dentry * dentry, struct iattr * attr) {return 0;}

#endif

#ifndef BLKSIZE_SIZE
#define BLKSIZE_SIZE
int *blksize_size[256];
#endif
