//
// ./include/linux/kdev_t.h>
//
typedef unsigned long long kdev_t;

//
// include/asm-i386/spinlock.h
//
typedef struct { } spinlock_t;
#define spin_lock(lock) do {} while (0)
#define spin_lock_irq(lock) do {} while (0)
#define spin_unlock(lock) do {} while (0)
#define spin_unlock_irq(lock) do {} while (0)
#define read_lock(lock) do {} while (0)
#define read_unlock(lock) do {} while (0)
#define write_lock(lock) do {} while (0)
#define write_unlock(lock) do {} while (0)
#define spin_lock_init(lock) do {} while(0)
typedef struct { } rwlock_t;

//
// include/asm-i386/atomic.h
//
typedef struct { int counter; } atomic_t;
#define atomic_read(v) ((v)->counter)
#define atomic_set(v,i) (((v)->counter) = i)
#define atomic_inc(v) (((v)->counter)++)
#define atomic_dec(v) (((v)->counter)--)

//
// include/linux/signal.h
//
#define sigfillset(set) do {} while (0)
typedef unsigned long sigset_t; 


//
// ??
//
struct qstr {
  const unsigned char * name;
  unsigned int len;
  unsigned int hash;
};

struct dentry {
  struct inode * d_inode;
  struct qstr d_name;
  unsigned char d_iname[256];
};

struct pipe_inode_info {
  int reserved;
};

#include "reiserfs_fs_i.h"

struct semaphore {
};


struct inode {
  struct super_block * i_sb;
  struct inode_operations * i_op;
  unsigned long i_blksize;
  unsigned long	i_blocks;
  unsigned int i_flags;
  unsigned int i_count;
  int i_dirt;
  kdev_t i_dev;
  off_t i_size;
  unsigned long i_ino;
  umode_t i_mode;
  time_t i_mtime;
  time_t i_ctime;
  time_t i_atime;
  uid_t	i_uid;
  gid_t	i_gid;
  nlink_t i_nlink;
  kdev_t i_rdev;
  int i_state;
  struct semaphore i_sem;
  struct inode * i_next;
  struct inode * i_prev;
  union {
    struct reiserfs_inode_info reiserfs_i;
  } u;
};
/* #define CURRENT_TIME 0 */

#define mark_inode_dirty(inode) ((inode)->i_dirt = 1)


struct file {
  struct dentry	* f_dentry;
  unsigned int f_flags;
  loff_t f_pos;
  struct file_operations * f_op;
  int f_error;
};

struct iattr {
};

struct page {
  int offset;
  int count;
  int flags;
};
#define PG_uptodate 1

typedef int (*filldir_t)(void *, const char *, int, off_t, ino_t);

struct vm_area_struct {
};

struct file_operations {
  int *llseek;
  ssize_t (*read) (struct file *, char *, size_t, loff_t *);
  ssize_t (*write) (struct file *, const char *, size_t, loff_t *);
  int (*readdir) (struct file *, void *, filldir_t);
  int *poll;
  int *ioctl;
  int (*mmap) (struct file *, struct vm_area_struct *);
  int *open;
  int *flush;
  int (*release) (struct inode *, struct file *);
  int (*fsync) (struct file *, struct dentry *);
  int *fasync;
  int *check_media_change;
  int *revalidate;
  int *lock;
};

struct wait_queue {
};


extern int generic_file_mmap(struct file *, struct vm_area_struct *);
struct inode_operations {
  struct file_operations * default_file_ops;
  int (*create) (struct inode *,struct dentry *,int);
  struct dentry * (*lookup) (struct inode *,struct dentry *);
  int (*link) (struct dentry *,struct inode *,struct dentry *);
  int (*unlink) (struct inode *,struct dentry *);
  int (*symlink) (struct inode *,struct dentry *,const char *);
  int (*mkdir) (struct inode *,struct dentry *,int);
  int (*rmdir) (struct inode *,struct dentry *);
  int (*mknod) (struct inode *,struct dentry *,int,int);
  int (*rename) (struct inode *, struct dentry *,
		 struct inode *, struct dentry *);
  int (*readlink) (struct dentry *, char *,int);
  struct dentry * (*follow_link) (struct dentry *, struct dentry *, unsigned int);
  int (*readpage) (struct file *, struct page *);
  int *writepage;
  int (*bmap) (struct inode *,int);
  void (*truncate) (struct inode *);
  int (*permission) (struct inode *, int);
  int *smap;
  int *updatepage;
  int *revalidate;
};

struct fs_struct {
  int umask;
};

#define RLIMIT_FSIZE    1
#define RLIM_NLIMITS     10
struct rlimit {
	long	rlim_cur;
	long	rlim_max;
};

//
// include/linux/sched.h
//
struct task_struct {
  int pid;
  int counter;
  int fsuid;
  int fsgid;
  int need_resched ;
  int policy;
  struct fs_struct * fs;
  struct rlimit rlim[RLIM_NLIMITS];
  spinlock_t sigmask_lock;
  sigset_t blocked;
  pid_t session;
  pid_t pgrp;
  char comm[16];
};

static inline void recalc_sigpending(struct task_struct *t)
{
}

#define SCHED_YIELD		0x10

#define CLONE_VM	0x00000100
#define CLONE_FS        0x00000200
#define CLONE_FILES	0x00000400

extern struct task_struct cur_task;

#define current (&cur_task)
int schedule (void);
int fsuser (void);
#define ERR_PTR(err)	((void *)((long)(err)))

/* inode.c */
#define I_DIRTY	1
#define I_LOCK 	2
void insert_inode_hash (struct inode * inode);
struct inode * get_empty_inode (void);
struct inode * iget (struct super_block *sb, unsigned long ino);
void iput (struct inode * inode);
int generic_readpage(struct file * file, struct page * page);
void sync_inodes (void);
int is_subdir (struct dentry *, struct dentry *);
void sleep_on (struct wait_queue **);
void wake_up (struct wait_queue **);

/*int not_formatted_node (char * buf, int blocksize);*/
int is_internal_node (char * buf);
int is_leaf_node (char * buf);
/*int is_bad_item (struct item_head * ih, char * item, int blocksize, int dev);*/
/*int is_leaf_bad (struct buffer_head * bh);*/


#define	EHASHCOLLISION	125


//
// mm/vmalloc.c
//
#define vfree freemem
#define vmalloc getmem



//
// include/linux/tqueue.h
//
struct tq_struct {
    struct tq_struct *next;		/* linked list of active bh's */
    unsigned long sync;		/* must be initialized to zero */
    void (*routine)(void *);	/* function to call */
    void *data;			/* argument to function */
};
typedef struct tq_struct * task_queue; 
#define queue_task(a,b) do {} while (0)
extern __inline__ void run_task_queue(task_queue *list)
{
}

//
// include/asm/smplock.h
//
#define lock_kernel() do {} while (0)
#define unlock_kernel() do {} while (0)

//
// kernel/exit.c
#define exit_files(tsk) do {} while (0)
#define exit_mm(tsk) do {} while (0)



//
// include/asm-i386/processor.h
//
int kernel_thread(int (*fn)(void *), void * arg, unsigned long flags);


