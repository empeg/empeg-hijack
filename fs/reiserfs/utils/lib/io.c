/*
 * Copyright 1996, 1997 Hans Reiser, see reiserfs/README for licensing and copyright details
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
/*#include <mntent.h>*/
#include <sys/types.h>
#include <asm/types.h>
#include <linux/unistd.h>


#include "inode.h"
#include "io.h"
#include "misc.h"


struct super_block *reiserfs_get_super(int dev) ;
struct buffer_head * find_buffer (int dev, int block, int size);
void __wait_on_buffer (struct buffer_head * bh);
struct buffer_head * getblk (int dev, int block, int size);
void brelse (struct buffer_head * bh);
void bforget (struct buffer_head * bh);
struct buffer_head * bread (int dev, unsigned long block, size_t size);
int bwrite (struct buffer_head * bh);
void ll_rw_block (int rw, int nr, struct buffer_head * bh[]);
void refile_buffer (struct buffer_head * bh);
/*void init_buffer_mem (void);*/
void check_and_free_buffer_mem (void);
int fsync_dev (int dev);



/* All buffers are in double linked cycled list. Buffers of tree are
   hashed by their block number.  If getblk found buffer with wanted
   block number in hash queue it moves buffer to the end of list */

#define BLOCK_SIZE 1024
#define MAX_NR_BUFFERS 16384
static int g_nr_buffers;

#define NR_HASH_QUEUES 20
static struct buffer_head * g_a_hash_queues [NR_HASH_QUEUES];
static struct buffer_head * g_buffer_list_head;
static struct buffer_head * g_buffer_heads;

void unlock_buffer(struct buffer_head *bh) {;} 
static void show_buffers (int dev, int size)
{
  int all = 0;
  int dirty = 0;
  int in_use = 0; /* count != 0 */
  int free = 0;
  struct buffer_head * next = g_buffer_list_head;

  for (;;) {
    if (!next)
      die ("show_buffers: buffer list is corrupted");
    if (next->b_dev == dev && next->b_size == size) {
      all ++;
      if (next->b_count != 0) {
	in_use ++;
      }
      if (buffer_dirty (next)) {
	dirty ++;
      }
      if (buffer_clean (next) && next->b_count == 0) {
	free ++;
      }
    }
    next = next->b_next;
    if (next == g_buffer_list_head)
      break;
  }

  printf ("show_buffers (dev %d, size %d): free %d, count != 0 %d, dirty %d, all %d\n", dev, size, free, in_use, dirty, all);
}

#if 0
static void check_hash_queues (void)
{
  int i, j;
  int index;
  struct buffer_head * next;

  for (i = 0; i < NR_HASH_QUEUES; i ++) {
    if ((next = g_a_hash_queues[i]) == 0)
      continue;
    if (next->b_hash_prev != 0)
      die ("check_hash_queues: b_hash_prev corrupted");

    index = next->b_blocknr % NR_HASH_QUEUES;
    for (j = 0; next; j ++) {
      if (next->b_blocknr % NR_HASH_QUEUES != index)
	die ("check_hash_queues: bad b_blocknr");
      next = next->b_hash_next;
      if (j > g_nr_buffers)
	die ("check_hash_queues: too many buffers in hash queue");
    }
  }
  for (i = 0; i < NR_HASH_QUEUES; i ++)
    for (j = 0; j < NR_HASH_QUEUES; j ++) {
      if (g_a_hash_queues[i] == g_a_hash_queues[j] && i != j && g_a_hash_queues[i] != 0 && g_a_hash_queues[j] != 0)
	die ("check_hash_queues: g_a_hash_queues array corrupted");
    }
      
}
#endif /* 0 */

static void insert_into_hash_queue (struct buffer_head * bh)
{
  int index = bh->b_blocknr % NR_HASH_QUEUES;

  if (bh->b_hash_prev || bh->b_hash_next)
    die ("insert_into_hash_queue: hash queue corrupted");

  if (g_a_hash_queues[index]) {
    g_a_hash_queues[index]->b_hash_prev = bh;
    bh->b_hash_next = g_a_hash_queues[index];
  }
  g_a_hash_queues[index] = bh;

/*  check_hash_queues ();*/
}


static void remove_from_hash_queue (struct buffer_head * bh)
{
  if (bh->b_hash_next == 0 && bh->b_hash_prev == 0 && bh != g_a_hash_queues[bh->b_blocknr % NR_HASH_QUEUES])
    /* (b_dev == 0) ? */
    return;

  if (bh == g_a_hash_queues[bh->b_blocknr % NR_HASH_QUEUES]) {
    if (bh->b_hash_prev != 0)
      die ("remove_from_hash_queue: hash queue corrupted");
    g_a_hash_queues[bh->b_blocknr % NR_HASH_QUEUES] = bh->b_hash_next;
/*
    if (bh->b_hash_next)
      g_a_hash_queues[bh->b_blocknr % NR_HASH_QUEUES]->b_hash_prev = 0;
*/
  }
  if (bh->b_hash_next)
    bh->b_hash_next->b_hash_prev = bh->b_hash_prev;

  if (bh->b_hash_prev)
    bh->b_hash_prev->b_hash_next = bh->b_hash_next;
/*
  else
    g_a_hash_queues[bh->b_blocknr % NR_HASH_QUEUES] = bh->b_hash_next;
*/

  bh->b_hash_prev = bh->b_hash_next = 0;

/*  check_hash_queues ();*/
}


static void put_buffer_list_end (struct buffer_head * bh)
{
  struct buffer_head * last = 0;

  if (bh->b_prev || bh->b_next)
    die ("put_buffer_list_end: buffer list corrupted");

  if (g_buffer_list_head == 0) {
    bh->b_next = bh;
    bh->b_prev = bh;
    g_buffer_list_head = bh;
  } else {
    last = g_buffer_list_head->b_prev;
    
    bh->b_next = last->b_next;
    bh->b_prev = last;
    last->b_next->b_prev = bh;
    last->b_next = bh;
  }
}


static void remove_from_buffer_list (struct buffer_head * bh)
{
  if (bh == bh->b_next) {
    g_buffer_list_head = 0;
  } else {
    bh->b_prev->b_next = bh->b_next;
    bh->b_next->b_prev = bh->b_prev;
    if (bh == g_buffer_list_head)
      g_buffer_list_head = bh->b_next;
  }

  bh->b_next = bh->b_prev = 0;
}


static void put_buffer_list_head (struct buffer_head * bh)
{
  put_buffer_list_end (bh);
  g_buffer_list_head = bh;
}


#define GROW_BUFFERS__NEW_BUFERS_PER_CALL 10
/* creates number of new buffers and insert them into head of buffer list 
 */
static int grow_buffers (int size)
{
  int i;
  struct buffer_head * bh, * tmp;

  if (g_nr_buffers + GROW_BUFFERS__NEW_BUFERS_PER_CALL > MAX_NR_BUFFERS)
    return 0;

  /* get memory for array of buffer heads */
  bh = (struct buffer_head *)getmem (GROW_BUFFERS__NEW_BUFERS_PER_CALL * sizeof (struct buffer_head) + sizeof (struct buffer_head *));
  if (g_buffer_heads == 0)
    g_buffer_heads = bh;
  else {
    /* link new array to the end of array list */
    tmp = g_buffer_heads;
    while (*(struct buffer_head **)(tmp + GROW_BUFFERS__NEW_BUFERS_PER_CALL) != 0)
      tmp = *(struct buffer_head **)(tmp + GROW_BUFFERS__NEW_BUFERS_PER_CALL);
    *(struct buffer_head **)(tmp + GROW_BUFFERS__NEW_BUFERS_PER_CALL) = bh;
  }

  for (i = 0; i < GROW_BUFFERS__NEW_BUFERS_PER_CALL; i ++) {

    tmp = bh + i;
    memset (tmp, 0, sizeof (struct buffer_head));
    tmp->b_data = getmem (size);
    if (tmp->b_data == 0)
      die ("grow_buffers: no memory for new buffer data");
    tmp->b_dev = 0;
    tmp->b_size = size;
    put_buffer_list_head (tmp);

    g_nr_buffers ++;
  }
  return GROW_BUFFERS__NEW_BUFERS_PER_CALL;
}


/*
int test_and_wait_on_buffer (struct buffer_head * bh)
{
  return CARRY_ON;
}
*/


struct buffer_head * find_buffer (int dev, int block, int size)
{		
  struct buffer_head * next;

  next = g_a_hash_queues[block % NR_HASH_QUEUES];
  for (;;) {
    struct buffer_head *tmp = next;
    if (!next)
      break;
    next = tmp->b_hash_next;
    if (tmp->b_blocknr != block || tmp->b_size != size || tmp->b_dev != dev)
      continue;
    next = tmp;
    break;
  }
  return next;
}

void __wait_on_buffer (struct buffer_head * bh)
{
}

struct buffer_head * get_hash_table(kdev_t dev, int block, int size)
{
  struct buffer_head * bh;

  bh = find_buffer (dev, block, size);
  if (bh) {
    bh->b_count ++;
  }
  return bh;
}


static struct buffer_head * get_free_buffer (int size)
{
  struct buffer_head * next = g_buffer_list_head;

  if (!next)
    return 0;
  for (;;) {
    if (!next)
      die ("get_free_buffer: buffer list is corrupted");
    if (next->b_count == 0 && buffer_clean (next) && next->b_size == size) {
      remove_from_hash_queue (next);
      remove_from_buffer_list (next);
      put_buffer_list_end (next);
      return next;
    }
    next = next->b_next;
    if (next == g_buffer_list_head)
      break;
  }
  return 0;
}


void sync_buffers (int size, int to_write)
{
  struct buffer_head * next = g_buffer_list_head;
  int written = 0;

  for (;;) {
    if (!next)
      die ("flush_buffer: buffer list is corrupted");
    
    if ((!size || next->b_size == size) && buffer_dirty (next) && buffer_uptodate (next)) {
      written ++;
      bwrite (next);
      if (written == to_write)
	return;
    }
    
    next = next->b_next;
    if (next == g_buffer_list_head)
      break;
  }
}

void reiserfs_sync_buffers(int dev, int wait) {
  fsync_dev(dev) ;
}
void reiserfs_sync_all_buffers(int dev, int wait) {
  fsync_dev(dev) ;
}

struct buffer_head * getblk (int dev, int block, int size)
{
  struct buffer_head * bh;

  bh = find_buffer (dev, block, size);
  if (bh) {
    if (0 && !buffer_uptodate (bh))
      die ("getblk: buffer must be uptodate");
    bh->b_count ++;
    return bh;
  }

  bh = get_free_buffer (size);
  if (bh == 0) {
    if (grow_buffers (size) == 0) {
      sync_buffers (size, 10);
    }
    bh = get_free_buffer (size);
    if (bh == 0) {
      show_buffers (dev, size);
      die ("getblk: no free buffers after grow_buffers and refill (%d)", g_nr_buffers);
    }
  }

  bh->b_count = 1;
  bh->b_dev = dev;
  bh->b_size = size;
  bh->b_blocknr = block;
  bh->b_end_io = NULL ;
  memset (bh->b_data, 0, size);
  clear_bit(BH_Dirty, &bh->b_state);
  clear_bit(BH_Uptodate, &bh->b_state);

  insert_into_hash_queue (bh);

  return bh;
}


void brelse (struct buffer_head * bh)
{
  if (bh == 0)
    return;
  if (bh->b_count == 0) {
    die ("brelse: can not free a free buffer %lu", bh->b_blocknr);
  }
  bh->b_count --;
}


void bforget (struct buffer_head * bh)
{
  if (bh) {
    brelse (bh);
    remove_from_hash_queue (bh);
    remove_from_buffer_list (bh);
    put_buffer_list_head (bh);
  }
}


#ifndef __alpha__

_syscall5 (int,  _llseek,  uint,  fd, ulong, hi, ulong, lo,
	  loff_t *, res, uint, wh);

loff_t reiserfs_llseek (unsigned int fd, loff_t offset, unsigned int origin)
{
  loff_t retval, result;
  
  retval = _llseek (fd, ((unsigned long long) offset) >> 32,
		    ((unsigned long long) offset) & 0xffffffff,
		    &result, origin);
  return (retval != 0 ? (loff_t)-1 : result);
  
}

#endif	/* ! __alpha__ */

struct buffer_head * bread (int dev, unsigned long block, size_t size)
{
  struct buffer_head * bh;
  loff_t offset;
  ssize_t bytes;

  bh = getblk (dev, block, size);
  if (buffer_uptodate (bh))
    return bh;

  offset = (loff_t)size * (loff_t)block;
  if (reiserfs_llseek (dev, offset, SEEK_SET) == (loff_t)-1)
    die ("bread: _llseek to position %ld (block=%d, dev=%d): %s\n", offset, block, dev, strerror (errno));

  bytes = read (bh->b_dev, bh->b_data, size);
  if (bytes != (ssize_t)size) {
    die ("bread: read %d bytes returned %d (block=%d, dev=%d)\n", size, bytes, block, dev);
  }

  mark_buffer_uptodate (bh, 0);
  return bh;
}


int aux_dev = 0;


int bwrite (struct buffer_head * bh)
{
    loff_t offset;
    ssize_t bytes;
    size_t size;

    if (!buffer_dirty (bh) || !buffer_uptodate (bh))
	return 0;

    size = bh->b_size;
    offset = (loff_t)size * (loff_t)bh->b_blocknr;
/*  off_hi = ((unsigned long long)offset) >> 32;
    off_lo = offset & 0xffffffff;*/

    if (reiserfs_llseek (bh->b_dev, offset, SEEK_SET) == (loff_t)-1)
	die ("bwrite: lseek to position %ld (block=%d, dev=%d): %s\n", offset, bh->b_blocknr, bh->b_dev, strerror (errno));

    bytes = write (bh->b_dev, bh->b_data, size);
    if (bytes != (ssize_t)size) {
	die ("bwrite: write %ld bytes returned %d (block=%ld, dev=%d): %s\n", size, bytes, bh->b_blocknr, bh->b_dev, strerror (errno));
    }
  
    mark_buffer_clean (bh);
    if (bh->b_end_io) {
	bh->b_end_io(bh, 1) ;
    }
    return 0;
}


void ll_rw_block (int rw, int nr, struct buffer_head * bh[])
{
  int i;
  long offset, res_lseek;
  int res_read;

  if (rw) {
    for(i = 0 ; i < nr ; i++) {
      bwrite(bh[i]) ;
    }
    return;
  }
  for(i = 0; i < nr; i ++) {
    offset = bh[i]->b_size * bh[i]->b_blocknr;
				/* This might be a problem that it is
                                   not lseek64 -Hans */
    res_lseek = lseek (bh[i]->b_dev, offset, SEEK_SET);
    if (res_lseek != offset) {      
      die ("bread: lseek to position %ld returned %ld (block=%d, dev=%d)\n", offset, res_lseek, bh[i]->b_blocknr, bh[i]->b_dev);
    }

    res_read = read (bh[i]->b_dev, bh[i]->b_data, bh[i]->b_size);
    if (res_read - bh[i]->b_size) {
      die ("bread: read %d bytes returned %d (block=%d, dev=%d)\n", bh[i]->b_size, res_read, bh[i]->b_blocknr, bh[i]->b_dev);
    }
    
    mark_buffer_uptodate (bh[i], 0);
  }
}


/*
void init_buffer_mem ()
{
  grow_buffers (BLOCK_SIZE);
}
*/


void check_and_free_buffer_mem (void)
{
  int i = 0;
  struct buffer_head * next = g_buffer_list_head;

  sync_buffers (0, 0);
  for (;;) {
    if (!next)
      die ("check_and_free_buffer_mem: buffer list is corrupted");
    if (next->b_count != 0)
      die ("check_and_free_buffer_mem: not free buffer (%d, %d, %d)",
	   next->b_blocknr, next->b_size, next->b_count);

    if (buffer_dirty (next) && buffer_uptodate (next))
      die ("check_and_free_buffer_mem: dirty buffer found");

    freemem (next->b_data);
    i ++;
    next = next->b_next;
    if (next == g_buffer_list_head)
      break;
  }
  if (i != g_nr_buffers)
    die ("check_and_free_buffer_mem: found %d buffers, must be %d", i, g_nr_buffers);

  /* free buffer heads */
  while ((next = g_buffer_heads)) {
    g_buffer_heads = *(struct buffer_head **)(next + GROW_BUFFERS__NEW_BUFERS_PER_CALL);
    freemem (next);
  }
  
  return;
}


void file_buffer(struct buffer_head *bh, int list) {
  refile_buffer(bh) ;
}

void refile_buffer (struct buffer_head * bh)
{
  remove_from_buffer_list (bh);
  put_buffer_list_head (bh);
  return;
}

int fsync_dev (int dev)
{
  sync_buffers (0, 0);
  return 0;
}

int file_fsync(struct file *filp, struct dentry *dentry)  {
  return 0 ;
}
















