/*
 * Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */
#ifdef __KERNEL__

#include <linux/sched.h>
#include <linux/reiserfs_fs.h>
#include <linux/locks.h>
#include <asm/bitops.h>

#else

#include "nokernel.h"

#endif


#ifdef REISERFS_CHECK
#if 0
static void check_bitmap (struct super_block * s)
{
  int i = 0;
  int free = 0;
  char * buf;

  while (i < SB_BLOCK_COUNT (s)) {
    buf = SB_AP_BITMAP (s)[i / (s->s_blocksize * 8)]->b_data;
    if (!test_bit (i % (s->s_blocksize * 8), buf))
      free ++;
    i ++;
  }

  if (free != SB_FREE_BLOCKS (s))
    reiserfs_warning ("vs-4000: check_bitmap: %d free blocks, must be %d\n",
		      free, SB_FREE_BLOCKS (s));
}
#endif


/* this checks, that block can be reused, and it has correct state
   (free or busy) */
int is_reusable (struct super_block * s, unsigned long block, int bit_value)
{
  int i, j;
  
  if (block == 0 || block >= SB_BLOCK_COUNT (s)) {
    printk ("REISERFS: block number is out of range %lu (%u)\n",
	    block, SB_BLOCK_COUNT (s));
    return 0;
  }

  /* it can't be one of the bitmap blocks */
  for (i = 0; i < le16_to_cpu (SB_BMAP_NR (s)); i ++)
    if (block == SB_AP_BITMAP (s)[i]->b_blocknr) {
      printk ("REISERFS: bitmap block %lu(%u) can't be freed or reused\n", block, le16_to_cpu (SB_BMAP_NR (s)));
      return 0;
    }
  
  i = block / (s->s_blocksize << 3);
  if (i >= le32_to_cpu (SB_BMAP_NR (s))) {
    printk ("REISERFS: there is no so many bitmap blocks: block=%lu, bitmap_nr=%d\n", block, i);
    return 0;
  }

  j = block % (s->s_blocksize << 3);
  if ((bit_value == 0 && test_bit (j, SB_AP_BITMAP (s)[i]->b_data)) ||
      (bit_value == 1 && test_bit (j, SB_AP_BITMAP (s)[i]->b_data) == 0)) {
    printk ("REISERFS: corresponding bit of block %lu does not match required value (i==%d, j==%d) test_bit==%d\n",
	    block, i, j, test_bit (j, SB_AP_BITMAP (s)[i]->b_data));
    return 0;
  }

  if (bit_value == 0 && block == SB_ROOT_BLOCK (s)) {
    printk ("REISERFS: this is root block (%u), it must be busy", SB_ROOT_BLOCK (s));
    return 0;
  }

  return 1;
}

#endif /* REISERFS_CHECK */


/* get address of corresponding bit (bitmap block number and offset in it) */
static inline void get_bit_address (struct super_block * s, unsigned long block, int * bmap_nr, int * offset)
{
                                /* It is in the bitmap block number equal to the block number divided by the number of
                                   bits in a block. */
  *bmap_nr = block / (s->s_blocksize << 3);
                                /* Within that bitmap block it is located at bit offset *offset. */
  *offset = block % (s->s_blocksize << 3);
  return;
}


/* There would be a modest performance benefit if we write a version
   to free a list of blocks at once. -Hans */
void reiserfs_free_block (struct reiserfs_transaction_handle *th, struct super_block * s, unsigned long block)
{
  struct reiserfs_super_block * rs;
  struct buffer_head * sbh;
  struct buffer_head ** apbh;
  int nr, offset;

#ifdef REISERFS_CHECK
  if (!s)
    reiserfs_panic (s, "vs-4005: reiserfs_free_block: trying to free block on nonexistent device");

  if (is_reusable (s, block, 1) == 0)
    reiserfs_panic (s, "vs-4010: reiserfs_free_block: can not free such block");
#endif

  rs = SB_DISK_SUPER_BLOCK (s);
  sbh = SB_BUFFER_WITH_SB (s);
  apbh = SB_AP_BITMAP (s);

  get_bit_address (s, block, &nr, &offset);

  /* mark it before we clear it, just in case */
  journal_mark_freed(th, s, block) ;

  /* clear bit for the given block in bit map */
  if (!test_and_clear_bit (offset, apbh[nr]->b_data)) {
    printk ("bitmap-124: reiserfs_free_block: free_block (%04x:%lu)[dev:blocknr]: bit already cleared\n", 
	    s->s_dev, block);
  }

  /* clear bit in cautious bitmap */
  /* clear_bit (offset, SB_AP_CAUTIOUS_BITMAP (s)[nr]->b_data); journal victim */

  /* update super block */
  rs->s_free_blocks = cpu_to_le32 (le32_to_cpu (rs->s_free_blocks) + 1);

  journal_mark_dirty (th, s, sbh);/* no need to place buffer on preserve list */
  journal_mark_dirty (th, s, apbh[nr]);/* no need to place buffer on preserve list */
  s->s_dirt = 1;
}



/* beginning from offset-th bit in bmap_nr-th bitmap block,
   find_forward finds the closest zero bit. It returns 1 and zero
   bit address (bitmap, offset) if zero bit found or 0 if there is no
   zero bits in forward direction */
static int find_forward (struct super_block * s, int * bmap_nr, int * offset, int * repeat, int for_unformatted)
{
  int i, j;
  struct buffer_head * bh;
  unsigned long block_to_try = 0;
  unsigned long next_block_to_try = 0 ;

  for (i = *bmap_nr; i < SB_BMAP_NR (s); i ++, *offset = 0) {
    /* get corresponding bitmap block */
    bh = SB_AP_BITMAP (s)[i];
    (*repeat) |= test_and_wait_on_buffer(bh) ;
retry:
    j = find_next_zero_bit ((unsigned long *)bh->b_data, s->s_blocksize << 3, *offset);

    /* wow, this really needs to be redone.  We can't allocate a block if
    ** it is in the journal somehow.  reiserfs_in_journal makes a suggestion
    ** for a good block if the one you ask for is in the journal.  Note,
    ** reiserfs_in_journal might reject the block it suggests.  The big
    ** gain from the suggestion is when a big file has been deleted, and
    ** many blocks show free in the real bitmap, but are all not free
    ** in the journal list bitmaps.
    **
    ** this whole system sucks.  The bitmaps should reflect exactly what
    ** can and can't be allocated, and the journal should update them as
    ** it goes.  TODO.
    */
    if (j < (s->s_blocksize << 3)) {
      block_to_try = (i * (s->s_blocksize << 3)) + j; 

      /* the block is not in the journal, we can proceed */
      if (!(reiserfs_in_journal(s, s->s_dev, block_to_try, s->s_blocksize, for_unformatted, &next_block_to_try))) {
	*bmap_nr = i;
	*offset = j;
	return 1;
      } 
      /* the block is in the journal */
      else if ((j+1) < (s->s_blocksize << 3)) { /* try again */
	/* reiserfs_in_journal suggested a new block to try */
	if (next_block_to_try > 0) {
	  int new_i ;
	  get_bit_address (s, next_block_to_try, &new_i, offset);

	  /* block is not in this bitmap. reset i and continue
	  ** we only reset i if new_i is in a later bitmap.
	  */
	  if (new_i > i) {
	    i = (new_i - 1 ); /* i gets incremented by the for loop */
	    continue ;
	  }
	} else {
	  /* no suggestion was made, just try the next block */
	  *offset = j+1 ;
	}
	goto retry ;
      }
    }
  }
  /* zero bit not found */
  return 0;
}

                                /* return 0 if no free blocks, else return 1 */
static int find_zero_bit_in_bitmap (struct super_block * s, unsigned long search_start, int * bmap_nr, int * offset, 
				    int * repeat, int for_unformatted)
{
  int retry_count = 0 ;
  /* get bit location (bitmap number and bit offset) of search_start block */
  get_bit_address (s, search_start, bmap_nr, offset);

    /* note that we search forward in the bitmap, benchmarks have shown that it is better to allocate in increasing
       sequence, which is probably due to the disk spinning in the forward direction.. */
    if (find_forward (s, bmap_nr, offset, repeat, for_unformatted) == 0) {
      /* there wasn't a free block with number greater than our
         starting point, so we are going to go to the beginning of the disk */

retry:
      search_start = 0; /* caller will reset search_start for itself also. */
      get_bit_address (s, search_start, bmap_nr, offset);
      if (find_forward (s, bmap_nr, offset, repeat, for_unformatted) == 0) {
	if (for_unformatted) {
	  if (retry_count == 0) {
	    /* we've got a chance that flushing async commits will free up
	    ** some space.  Sync then retry
	    */
	    flush_async_commits(s, repeat) ;
	    retry_count++ ;
	    goto retry ;
	  } else if (retry_count > 0) {
	    /* nothing more we can do.  Make the others wait, flush
	    ** all log blocks to disk, and flush to their home locations.
	    ** this will free up any blocks held by the journal
	    */
	    SB_JOURNAL(s)->j_must_wait = 1 ;
	  }
	}
        return 0;
      }
    }
  return 1;
}

/* get amount_needed free block numbers from scanning the bitmap of free/used blocks.
   
   Optimize layout by trying to find them starting from search_start
   and moving in elevator_direction, until a free block or the disk
   edge is reached, and then if the edge was reached, changing the
   elevator direction, and looking backwards from search_start.

   search_start is the block number of the current node if we are
   creating a new node, and it is the block number of the left
   semantic neighbor of the current node if we are relocating a node
   (using the write next to algorithm).  

   If no free blocks are found, and there are blocks on the preserve
   list, run sync_buffers() to free them.
   
   Note that when we free the preserve list we free all members of the
   free_blocknrs array that we have gotten so far, on the assumption
   that the freeing was likely to have created a better choice of
   blocknrs, since needing to free implies that there were few free
   ones to choose from, and that in turn implies that they were likely
   to be poor choices, but if schedule occurs because of lock then we
   guess that the old values are likely enough to be good that we
   should not bother to see if we get better ones and save on the CPU
   consumption and code size.  I don't know if this is correct, but it
   seems unlikely to really matter much.

   return 0 if everything is ok
   return NO_DISK_SPACE if out of disk space,
   or SCHEDULE_OCCURED
   
   return block numbers found, in the array free_blocknrs.  assumes
   that any non-zero entries already present in the array are valid.

   if number of free blocks is less than RESERVED_FOR_PRESERVE_LIST, we try to
   get_space_from_preserve_list, because we do not want to lose free
   blocks, that are reserved for the preserve list

   If number of free blocks + number of preserved blocks is less than
   RESERVED_FOR_PRESERVE_LIST, than reiserfs_new_blocknrs will fail until it is used for
   preserve list

   reiserfsck has its own reiserfs_new_blocknrs, which can use RESERVED_FOR_PRESERVE_LIST blocks
*/

static int do_reiserfs_new_blocknrs (struct reiserfs_transaction_handle *th, struct super_block * s, unsigned long * free_blocknrs, 
			   unsigned long search_start, int amount_needed, int for_preserve_list, int for_unformatted)
{
  int i, j;
  int retval = CARRY_ON;	/* it is set to SCHEDULE_OCCURED when
				   get_space_from_preserve_list ran,
				   or NO_DISK_SPACE when .. */
  unsigned long * block_list_start = free_blocknrs;
  int init_amount_needed = amount_needed;

/*
  if (SB_FREE_BLOCKS (s) < RESERVED_FOR_PRESERVE_LIST) {
    get_space_from_preserve_list (s);
    retval = SCHEDULE_OCCURRED;
  }
*/

  if (SB_FREE_BLOCKS (s) < RESERVED_FOR_PRESERVE_LIST && !for_preserve_list) {
    /* there is some free space just to keep preserve list working */
    return NO_DISK_SPACE;
  }

#ifdef REISERFS_CHECK
  if (!s)
    reiserfs_panic (s, "vs-4020: reiserfs_new_blocknrs: trying to get new block from nonexistent device");

  if (search_start == MAX_B_NUM)
    reiserfs_panic (s, "vs-4025: reiserfs_new_blocknrs: we are optimizing location based on "
		    "the bogus location of a temp buffer (%lu).", search_start);

  if (amount_needed < 1 || amount_needed > MAX_PRESERVE_NODES) 
    reiserfs_panic (s, "vs-4030: reiserfs_new_blocknrs: amount_needed parameter incorrect (%d)", amount_needed);
#endif /* REISERFS_CHECK */

  /* We continue the while loop if another process snatches our found
   * free block from us after we find it but before we successfully
   * mark it as in use, or if we need to use sync to free up some
   * blocks on the preserve list.  */

  while (amount_needed--) {
    /* skip over any blocknrs already gotten last time. */
    if (*(free_blocknrs) != 0) {
#ifdef REISERFS_CHECK
      if (is_reusable (s, *free_blocknrs, 1) == 0)
	reiserfs_panic(s, "vs-4035: reiserfs_new_blocknrs: bad blocknr on free_blocknrs list");
#endif /* REISERFS_CHECK */
      free_blocknrs++;
      continue;
    }
    /* look for zero bits in bitmap */
    if (find_zero_bit_in_bitmap (s, search_start, &i, &j, &retval, for_unformatted) == 0) {
      if (find_zero_bit_in_bitmap (s, search_start, &i, &j, &retval, for_unformatted) == 0) {
	for ( ; block_list_start != free_blocknrs; block_list_start++) {
	  reiserfs_free_block (th, s, *block_list_start);
	  COMPLETE_BITMAP_DIRTING_AFTER_FREEING(s,*block_list_start / (s->s_blocksize * 8));
	  *block_list_start = 0;
	}
	return NO_DISK_SPACE;
      }
    }
    
    /* i and j now contain the results of the search. i = bitmap block
       number containing free block, j = offset in this block.  we
       compute the blocknr which is our result, store it in
       free_blocknrs, and increment the pointer so that on the next
       loop we will insert into the next location in the array.  Also
       in preparation for the next loop, search_start is changed so
       that the next search will not rescan the same range but will
       start where this search finished.  Note that while it is
       possible that schedule has occurred and blocks have been freed
       in that range, it is perhaps more important that the blocks
       returned be near each other than that they be near their other
       neighbors, and it also simplifies and speeds the code this way.  */

    /* journal: we need to make sure the block we are giving out is not
    ** a log block, horrible things would happen there.
    */
    search_start = (i * (s->s_blocksize << 3)) + j; 
    if (search_start >= SB_JOURNAL_BLOCK(s) &&
        search_start < (SB_JOURNAL_BLOCK(s) + JOURNAL_BLOCK_COUNT)) {
      reiserfs_warning("bitmap-370, trying to allocate log block %lu\n",
                        search_start) ;
      search_start++ ;
      continue ;
    }
       
    *free_blocknrs = search_start ;

#ifdef REISERFS_CHECK
    if (buffer_locked (SB_AP_BITMAP (s)[i]) || is_reusable (s, search_start, 0) == 0)
      reiserfs_panic (s, "vs-4040: reiserfs_new_blocknrs: bitmap block is locked or bad block number found");
#endif

    /* set bit in true bitmap, but do not set bit in cautious bitmap */
    if (test_and_set_bit (j, SB_AP_BITMAP (s)[i]->b_data))
      reiserfs_panic (s, "vs-4045: reiserfs_new_blocknrs: schedule did not occur and this block was free");
    
    journal_mark_dirty (th, s, SB_AP_BITMAP (s)[i]); 

    /* it should be marked as suspected recipient when old items moved
       to it. For now do it unconditionally */
/*    mark_suspected_recipient (s, SB_AP_BITMAP (s)[i]);*/
    free_blocknrs ++;
  }

  /* update free block count in super block */
  SB_FREE_BLOCKS (s) = cpu_to_le32 (le32_to_cpu (SB_FREE_BLOCKS (s)) - init_amount_needed);
  journal_mark_dirty (th, s, SB_BUFFER_WITH_SB (s));
  s->s_dirt = 1;

  return retval;
}

int reiserfs_new_blocknrs (struct reiserfs_transaction_handle *th, struct super_block * s, unsigned long * free_blocknrs,
			    unsigned long search_start, int amount_needed, int for_preserve_list) {
  return do_reiserfs_new_blocknrs(th, s, free_blocknrs, search_start, amount_needed, for_preserve_list, 0) ;
}

int reiserfs_new_unf_blocknrs(struct reiserfs_transaction_handle *th, struct super_block * s, unsigned long * free_blocknrs,
				 unsigned long search_start, int amount_needed, int for_preserve_list) {
  return do_reiserfs_new_blocknrs(th, s, free_blocknrs, search_start, amount_needed, for_preserve_list, 1) ;
}
