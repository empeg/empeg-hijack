/*
 *  Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */


/*
 * Contains code from
 *
 *  linux/include/linux/lock.h and linux/fs/buffer.c /linux/fs/minix/fsync.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */


/* this is to be included from fs/buffer.c. This requires adding few lines in kernel/ksyms.c */

#ifndef __KERNEL__

#include "nokernel.h"
void mark_suspected_recipients_dirty(struct reiserfs_transaction_handle *th, kdev_t a) {}
void fixup_reiserfs_buffers (kdev_t dev) {}
void reiserfs_show_buffers (kdev_t dev){}
#else
#include <linux/reiserfs_fs.h>
#endif

#define CARRY_ON                0
#define SCHEDULE_OCCURRED       1

inline int  test_and_wait_on_buffer(
              struct buffer_head * p_s_bh
            ) {
  if ( buffer_locked(p_s_bh) )  {
    __wait_on_buffer(p_s_bh);
    return SCHEDULE_OCCURRED;
  }
  return CARRY_ON;
}
 

/* This is pretty much the same as the corresponding original linux
   function, it differs only in that it tracks whether schedule
   occurred. */
/* This function function calls find_buffer which looks for the buffer
   desired on the appropriate hash queue, and then waits for it to unlock. */
struct buffer_head  * reiserfs_get_hash_table(
                        kdev_t  n_dev,
                        int     n_block,
                        int     n_size,
                        int   * p_n_repeat
                      ) {
  struct buffer_head * p_s_bh;

  for ( ; ; ) {
    if ( ! (p_s_bh = find_buffer(n_dev, n_block, n_size)) )
      return NULL;
    p_s_bh->b_count++;
    *p_n_repeat |= test_and_wait_on_buffer(p_s_bh);
    if ( *p_n_repeat == CARRY_ON ||
	 (p_s_bh->b_dev == n_dev && p_s_bh->b_blocknr == n_block && p_s_bh->b_size == n_size) )
      return p_s_bh;
    p_s_bh->b_count--;
  }
}


#ifdef __KERNEL__
#if 0 /* not needed anymore */
struct super_block * reiserfs_get_super (kdev_t dev)
{
  struct super_block * s = NULL ;

  
  if (!dev)
    return NULL;
  s = sb_entry(super_blocks.next);
  while (s != sb_entry(&super_blocks)) {
    if (s && s->s_dev == dev) {
      return s;
    } else {
      s = sb_entry(s->s_list.next);
    }
  }
  return NULL;
}
#endif

/*   
** end_io for all the log blocks.
**   
** this used to do more, but right now it only decrements j_commit_left for the
** journal list this log block belongs to
*/   
void finish_log_block_io(struct super_block *p_s_sb, struct buffer_head *bh, int uptodate) {
  int j ;
  int index ;
  int found = 0 ;
  int start ;
  unsigned long startb, endb ;
     
  /* desc is at start, commit is at start + len + 1 */
  start = SB_JOURNAL_LIST_INDEX(p_s_sb) ;
  if (start < 5) {
    start = JOURNAL_LIST_COUNT - 5 ;
  } else {
    start = start - 5; 
  }
  for (j = 0 ; j < JOURNAL_LIST_COUNT ; j++) {
    index = (start + j) % JOURNAL_LIST_COUNT ;
    startb = SB_JOURNAL_LIST(p_s_sb)[index].j_start + SB_JOURNAL_BLOCK(p_s_sb) ;
    endb = SB_JOURNAL_BLOCK(p_s_sb) + ((SB_JOURNAL_LIST(p_s_sb)[index].j_start + SB_JOURNAL_LIST(p_s_sb)[index].j_len + 1) %
           JOURNAL_BLOCK_COUNT) ;
    if (SB_JOURNAL_LIST(p_s_sb)[index].j_len > 0 &&
       ( 
         ((SB_JOURNAL_LIST(p_s_sb)[index].j_start + SB_JOURNAL_LIST(p_s_sb)[index].j_len + 1) < JOURNAL_BLOCK_COUNT &&
           startb <= bh->b_blocknr && endb >= bh->b_blocknr) ||
         ((SB_JOURNAL_LIST(p_s_sb)[index].j_start + SB_JOURNAL_LIST(p_s_sb)[index].j_len + 1) >= JOURNAL_BLOCK_COUNT &&
           (startb <= bh->b_blocknr || endb >= bh->b_blocknr))
       )
      ) {
      atomic_dec(&(SB_JOURNAL_LIST(p_s_sb)[index].j_commit_left)) ;
      found = 1 ;
      break ;
    }
  }
  if (found == 0) { 
    printk("buffer-115: Unable to find journal list for block %lu\n", bh->b_blocknr) ;
  } 
  mark_buffer_uptodate(bh, uptodate);
  unlock_buffer(bh);
}


/* no longer need, should just make journal.c use the default handler */
void reiserfs_journal_end_io (struct buffer_head *bh, int uptodate)
{
  mark_buffer_uptodate(bh, uptodate);
  unlock_buffer(bh);
  return ;
}


/* struct used to service end_io events.  kmalloc'd in 
** reiserfs_end_buffer_io_sync 
*/
struct reiserfs_end_io {
  struct buffer_head *bh ; /* buffer head to check */
  struct tq_struct task ;  /* task struct to use */
  struct reiserfs_end_io *self ; /* pointer to this struct for kfree to use */
} ;

/*
** does the hash list updating required to release a buffer head.
** must not be called at interrupt time (so I can use the non irq masking 
** spinlocks).  Right now, put onto the schedule task queue, one for
** each block that gets written
*/
static void reiserfs_end_io_task(struct reiserfs_end_io *io) {
  struct buffer_head *bh = io->bh ;

  if (buffer_journal_dirty(bh)) {
    struct reiserfs_journal_cnode *cur ;
    struct super_block * s = get_super (bh->b_dev);

    if (!s) 
      goto done ;

    lock_kernel() ;
    if (!buffer_journal_dirty(bh)) { 
      unlock_kernel() ;
      goto done ;
    }
    mark_buffer_notjournal_dirty(bh) ;
    cur = (journal_hash(SB_JOURNAL(s)->j_list_hash_table, bh->b_dev, bh->b_blocknr)) ;
    while(cur) {
      if (cur->bh && cur->blocknr == bh->b_blocknr && cur->dev == bh->b_dev) {
	if (cur->jlist) { /* since we are clearing the bh, we must decrement nonzerolen */
	  atomic_dec(&(cur->jlist->j_nonzerolen)) ;
	}
	cur->bh = NULL ;
      }
      cur = cur->hnext ;
    }
    bh->b_count-- ;
    unlock_kernel() ;
  }
done:
  kfree(io->self) ;
  return ;
}

/*
** general end_io routine for all reiserfs blocks.
** logged blocks will come in here marked buffer_journal_dirty()
** a reiserfs_end_io struct is kmalloc'd for them, and a task is put 
** on the scheduler queue.  It then does all the required hash table
** operations to reflect the buffer as writen
*/
void reiserfs_end_buffer_io_sync (struct buffer_head *bh, int uptodate)
{

  mark_buffer_notjournal_new(bh) ;
  if (buffer_journal_dirty(bh)) {
    struct reiserfs_end_io *io = kmalloc(sizeof(struct reiserfs_end_io), 
                                         GFP_ATOMIC) ;
    /* note, if kmalloc fails, this buffer will be taken care of
    ** by a check at the end of do_journal_end() in journal.c
    */
    if (io) {
      io->task.next = NULL ;
      io->task.sync = 0 ;
      io->task.routine = (void *)(void *)reiserfs_end_io_task ;
      io->task.data = io ;
      io->self = io ;
      io->bh = bh ;
      queue_task(&(io->task), &tq_scheduler) ;
    } else {
      printk("reiserfs/buffer.c-184: kmalloc returned NULL\n") ;
    }
  }
  mark_buffer_uptodate(bh, uptodate);
  unlock_buffer(bh);
}

/* This is pretty much the same as the corresponding original linux
   function, it differs only in that it tracks whether schedule
   occurred. */
/* This function looks for a buffer which contains a given block.  If
   the block is in cache it returns it, otherwise it returns a new
   buffer which is not uptodate.  This is called by reiserfs_bread and
   other functions. Note that get_new_buffer ought to be called this
   and this ought to be called get_new_buffer, since this doesn't
   actually get the block off of the disk. */
struct buffer_head  * reiserfs_getblk(
                        kdev_t    n_dev,
                        int       n_block,
                        int       n_size,
                        int     * p_n_repeat
                      ) {
   struct buffer_head  * p_s_bh;
  int                   n_isize;

repeat:

  p_s_bh = reiserfs_get_hash_table(n_dev, n_block, n_size, p_n_repeat);
  if ( p_s_bh ) {
    if ( ! buffer_dirty(p_s_bh) ) {
      p_s_bh->b_flushtime = 0;
    }
    p_s_bh->b_end_io = reiserfs_end_buffer_io_sync;
    return p_s_bh;
  }

  n_isize = BUFSIZE_INDEX(n_size);
get_free:
  p_s_bh = free_list[n_isize];
  if (!p_s_bh)
    goto refill;
  remove_from_free_list(p_s_bh);

  /* OK, FINALLY we know that this buffer is the only one of its kind,
   * and that it's unused (b_count=0), unlocked, and clean.
   */
  init_buffer(p_s_bh, n_dev, n_block, reiserfs_end_buffer_io_sync, NULL);
  p_s_bh->b_state=0;
  insert_into_queues(p_s_bh);
  return p_s_bh;

  /*
   * If we block while refilling the free list, somebody may
   * create the buffer first ... search the hashes again.
   */
refill:
  *p_n_repeat |= SCHEDULE_OCCURRED;
  refill_freelist(n_size);
  if (!find_buffer(n_dev,n_block,n_size))
    goto get_free;
  goto repeat;

}

void fixup_reiserfs_buffers (kdev_t dev)
{
  int i;
  int nlist;
  int slept;
  struct buffer_head * bh;
  
 again:
  slept = 0;
  for(nlist = 0; nlist < NR_LIST; nlist++) {
    bh = lru_list[nlist];
    for (i = nr_buffers_type[nlist] ; i > 0 ; bh = bh->b_next_free, i--) {
      if (bh->b_dev != dev)
	continue;
      if (buffer_locked(bh))
      {
	slept = 1;
	__wait_on_buffer(bh);
      }

      /* set the end_io callback once the buffer is not under I/O,
         nobody can start I/O from under us because we are protected
         by the big kernel lock */
      bh->b_end_io = end_buffer_io_sync;
      if (bh->b_count > 0) {
        printk("buffer-300: BAD, count is %d for buffer %lu\n", bh->b_count, bh->b_blocknr) ;
      }
      if (slept)
        goto again;
    }
  }
}

/* call by unpreserve when cautous bitmap block is being dirtied */
void reiserfs_refile_buffer (struct buffer_head * buf)
{
  if (BUF_DIRTY != buf->b_list)
    file_buffer (buf, BUF_DIRTY);
}

void reiserfs_file_buffer(struct buffer_head *bh, int list) {
  file_buffer(bh, list) ;
}

#else

void reiserfs_end_buffer_io_sync (struct buffer_head *bh, int uptodate)
{
 ;
}

int reiserfs_add_handler(struct buffer_head *bh) {
  bh->b_end_io =  reiserfs_end_buffer_io_sync  ;
  return 0 ;
}

struct buffer_head * reiserfs_getblk (kdev_t dev, int block, int size, int * p_n_repeat) {
  struct buffer_head *bh ;
  bh = getblk(dev, block, size) ;
  if (bh) {
    bh->b_end_io = reiserfs_end_buffer_io_sync ;
  }
  return bh ;
}


#endif /* __KERNEL__ */


