/*
 * Copyright 1996-1999 Hans Reiser
 */

#include "fsck.h"
#include "reiserfs.h"

/* g_disk_bitmap initially contains copy of disk bitmaps
   (cautious version of it);

   g_new_bitmap initially has marked only super block, bitmap blocks
   and bits after the end of bitmap

   in pass 1 we go through g_disk_bitmap. 

   If block does not look like formatted node, we skip it.

   If block contains internal node, put 0 in g_disk_bitmap if block is
   not used in new tree yet.

   If block contains leaf and is used already (by an indirect item
   handled already to this time) save all items. They will be inserted
   into tree after pass 1.

   If block looking like leaf is not used in the new tree, try to
   insert in into tree. If it is not possible, mark block in
   g_uninsertable_leaf_bitmap. Blocks marked in this bitmap will be inserted into tree in pass 2. They can not be

  This means, that in pass 1 when we have
   found block containing the internal nodes we mark it in
   g_disk_bitmap as free (reiserfs_free_internal_block). When block
   gets into new tree it is marked in g_new_bitmap (mark_block_used)
   When collecting resources for do_balance, we mark new blocks with
   mark_block_used. After do_balance we unmark unused new blocks in
   g_new_bitmap (bitmap.c:/reiserfs_free_block)

   Allocating of new blocks: look for 0 bit in g_disk_bitmap
   (find_zero_bit_in_bitmap), make sure, that g_new_bitmap contains 0
   at the corresponding bit (is_block_used).
      
 */



int was_block_used (unsigned long block)
{
  int i, j;

  if (block >= SB_BLOCK_COUNT (&g_sb))
    die ("was_block_used: %d is too big (%d)\n", block, SB_BLOCK_COUNT (&g_sb));

  if (opt_what_to_scan == SCAN_WHOLE_PARTITION)
      /* this function is used to set 0 into indirect item entry when
         it points to a block which was marked free in the
         bitmap. When we scan whole partition we must gather as much
         as possible. So, take it */
      return 1;

  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  return test_bit (j, g_disk_bitmap[i]);
}


/* is blocks used (marked by 1 in new bitmap) in the tree which is being built (as leaf, internal,
   bitmap, or unformatted node) */
int is_block_used (unsigned long block)
{
  int i, j;

  if(g_new_bitmap == 0)
    return 0;
  if (block >= SB_BLOCK_COUNT (&g_sb)) {
    printf ("is_block_used: %ld is too big (%d)\n", block, SB_BLOCK_COUNT (&g_sb));
    return 1;
  }

  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  return test_bit (j, g_new_bitmap[i]);
}


void mark_block_used (unsigned long block)
{
  int i, j;

  if (is_block_used (block))
    die ("mark_block_used: (%lu) used already", block);

  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  set_bit (j, g_new_bitmap[i]);
  SB_FREE_BLOCKS (&g_sb)--;
}

/*%%%%%%%%%%%%%%%%%%%%%%*/
int is_block_formatted (unsigned long block)
{
  int i, j;

  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  return test_bit (j, g_formatted[i]);
}
int is_block_unformatted (unsigned long block)
{
  int i, j;

  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  return test_bit (j, g_unformatted[i]);
}
void mark_block_formatted (unsigned long block)
{
  int i, j;

  if (is_block_formatted (block) || is_block_unformatted (block))
    die ("mark_block_formatted: (%lu) used already", block);

  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  set_bit (j, g_formatted[i]);
}
void mark_block_unformatted (unsigned long block)
{
  int i, j;

  if (is_block_formatted (block) || is_block_unformatted (block))
    die ("mark_block_unformatted: (%lu) used already", block);


  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  set_bit (j, g_unformatted[i]);
}
void unmark_block_formatted (unsigned long block)
{
  int i, j;

  if (!is_block_formatted (block) || is_block_unformatted (block))
    die ("unmark_block_formatted: (%lu) used already", block);

  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  clear_bit (j, g_formatted[i]);
}
void unmark_block_unformatted (unsigned long block)
{
  int i, j;

  if (is_block_formatted (block) || !is_block_unformatted (block))
    die ("unmark_block_unformatted: (%lu) used already", block);

  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  clear_bit (j, g_unformatted[i]);
}
/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/

/* uninsertable block is marked by bit clearing */
void mark_block_uninsertable (unsigned long block)
{
  int i, j;

  if (is_block_used (block))
    die ("mark_block_uninsertable: (%lu) used already", block);

  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  clear_bit (j, g_uninsertable_leaf_bitmap[i]);
}

int is_block_uninsertable (unsigned long block)
{
  int i, j;
  
  if (is_block_used (block))
    die ("is_block_uninsertable: (%lu) used already", block);

  i = block / (g_sb.s_blocksize * 8);
  j = block % (g_sb.s_blocksize * 8);
  return !test_bit (j, g_uninsertable_leaf_bitmap[i]);
}

static inline void get_bit_address (struct super_block * s, unsigned long block, int * bmap_nr, int * offset)
{
  *bmap_nr = block / (s->s_blocksize << 3);
  *offset = block % (s->s_blocksize << 3);
  return;
}

static inline int find_prev_zero_bit (void * addr, int offset)
{
  char * start;			/* byte pointer to starting byte of search */
  int bit_offset;		/* bit offset within starting byte of starting point */
  char mask;

  start = (char *)addr + (offset >> 3);
  bit_offset = (offset % 8);

  mask = (unsigned int)0xff >> (7 - bit_offset);
  while (start >= (char *)addr) {
    if ((*start & mask) != mask) {
      /* there is at least one 0 bit in current byte */
      for (; bit_offset >= 0; bit_offset --) {
	if (!((1 << bit_offset) & *start))
	  return ((start - (char *)addr) << 3) + bit_offset;
      }
      die ("find_prev_zero_bit: must be at least 1 zero bit");
    }
    bit_offset = 7;
    mask = (unsigned int)0xff;
    start --;
  }
  /* there is no zero bit when we go from offset to the left up to addr */
  return -1;

}


/* beginning from offset-th bit in bmap_nr-th bitmap block,
   find_forward finds the closest zero bit. It returns 1 and zero
   bit address (bitmap, offset) if zero bit found or 1 if there is no
   zero bits in forward direction */
static int find_forward (struct super_block * s, int * bmap_nr, int * offset)
{
  int i, j;
  struct buffer_head * bh;

  for (i = *bmap_nr; i < SB_BMAP_NR (s); i ++, *offset = 0) {
    /* get corresponding bitmap block */
    bh = SB_AP_BITMAP (s)[i];/*g_disk_bitmap[i];*/
    while (*offset < (s->s_blocksize << 3)) {
      j = find_next_zero_bit ((unsigned long *)bh->b_data, s->s_blocksize << 3, *offset);
      if (j < (s->s_blocksize << 3)) {
	*bmap_nr = i;
	*offset = j;
	
	/* we found free block in disk bitmap, make sure, that it is
           not used in new built tree yet */
	if (is_block_used (i * (s->s_blocksize << 3) + j)) {
	  (*offset) ++;
	  continue;
	}
	return 1;
      }
      break; /* while */
    }
  }	/* for */

  /* zero bit not found */
  return 0;
}


/* this does the same as find_forward does, but in backward direction */
static int find_backward (struct super_block * s, int * bmap_nr, int * offset)
{
  int i, j;
  struct buffer_head * bh;

  for (i = *bmap_nr; i > -1; i --, *offset = (s->s_blocksize << 3) - 1) {
    /* get corresponding bitmap block */
    bh = SB_AP_BITMAP (s)[i];/*g_disk_bitmap[i];*/
    
    /* at first we start from position, in next bitmap block we start from 0th position */
    while (*offset > -1) {
      j = find_prev_zero_bit ((unsigned long *)bh->b_data, *offset);
      if (j != -1) {
	*bmap_nr = i;
	*offset = j;
	
	/* we found free block in disk bitmap, make sure, that it is not used in new built tree yet */
	if (is_block_used (i * (s->s_blocksize << 3) + j)) {
	  (*offset) --;
	  continue;
	}
	return 1;
      }
      break;	/* from while */
    }
    
    /* in previous bitmap block we start from the end */
/*    *offset = (s->s_blocksize << 3) - 1;*/
  }	/* for */
  
  /* zero bit not found */
  return 0;
}


static unsigned long find_zero_bit_in_bitmap (struct super_block * s, unsigned long search_start)
{
  int bmap_nr, offset;

  /* get bit location (bitmap number and bit offset) of search_start block */
  get_bit_address (s, search_start, &bmap_nr, &offset);

  /* first we are going to the right (as elevator_direction requires) */
  if (find_forward (s, &bmap_nr, &offset) == 0) {
    /* there wasn't a free block with number greater than our
       starting point, so we are going to do find_backward */
    get_bit_address (s, search_start, &bmap_nr, &offset);
    if (find_backward (s, &bmap_nr, &offset) == 0)
      return 0;
  }

  /* ok, mark block in new bitmap */
  mark_block_used (bmap_nr * (s->s_blocksize << 3) + offset);
  return (bmap_nr * (s->s_blocksize << 3)) + offset;
}


/* mark block free in bitmap we use to build the tree */
void reiserfs_free_internal_block (struct super_block * s, unsigned long block)
{
  int i, j;

  i = block / (s->s_blocksize * 8);
  j = block % (s->s_blocksize * 8);

  if (test_bit (j, SB_AP_BITMAP (s)[i]->b_data) == 0)
    die ("reiserfs_free_internal_block: Block %lu is free", block);

  clear_bit (j, SB_AP_BITMAP (s)[i]->b_data);
  g_old_rs->s_free_blocks ++;
}


/* try to find 'to_free' internal nodes and mark corresponding blocks
   free. Return number of freed blocks */
static int try_to_free_unused_internal_blocks (int to_free)
{
    int i, j, k;
    int freed = 0;
    struct buffer_head * bh;
    int block;

    /* just to do not waste time: onthe partition sent by Petru there
       is no internal nodes */
    return 0;

    printf ("Trying to find internal nodes which are not used in new tree..");fflush (stdout);
    for (i = 0; i < SB_BMAP_NR (&g_sb); i ++)
	for (j = 0; j < g_sb.s_blocksize; j ++) {
	    if (i * g_sb.s_blocksize * 8 + j * 8 == SB_BLOCK_COUNT (&g_sb))
		goto out_of_bitmap;
	    for (k = 0; k < 8; k ++) {
		block = i * g_sb.s_blocksize * 8 + j * 8 + k;
		if (is_block_used (block/*i * g_sb.s_blocksize * 8 + j * 8 + k*/))
		    continue;
		bh = bread (g_sb.s_dev, i * g_sb.s_blocksize * 8 + j * 8 + k, g_sb.s_blocksize);
		if (not_formatted_node (bh->b_data, g_sb.s_blocksize)) {
		    brelse (bh);
		    continue;
		}
		/* this node is formatted node. we can free internal node  */
		if (is_internal_node (bh->b_data)) {
		    reiserfs_free_internal_block (&g_sb, bh->b_blocknr);
		    printf (".");fflush (stdout);
		    freed ++;
		    if (freed == to_free) {
			brelse (bh);
			goto out_of_bitmap;
		    }
		}
		brelse (bh);
	    }
	}
 out_of_bitmap:
    printf ("\n");
    return freed;
}


int from_journal;

int reiserfs_new_blocknrs (struct reiserfs_transaction_handle *th, struct super_block * s, 
			   unsigned long * free_blocknrs, unsigned long start, int amount_needed, int notused)
{
    while (amount_needed --) {
	*free_blocknrs = find_zero_bit_in_bitmap (s, start);
	if (*free_blocknrs == 0) {
	    /* if we still did not take journal space lets try to find
               internal nodes and free them */
	    if (from_journal == 0 && try_to_free_unused_internal_blocks (10))
		/* got some space */
		continue;

	    /* ok, no free space on device. There are no internal
	       nodes which could be freed. This is especially very
	       likely when you specify --scan-whole-partition. Take
	       journal space! */

	    /* take blocks starting from must journal start but not
               over the journal partition had before recovering */
	    if (from_journal == get_journal_size (s))
		/* whole journal is used already */
		die ("Journal space is used. No idea where to get free space");

	    if (from_journal == 0)
		printf ("Start using journal space\n");
	    *free_blocknrs = from_journal + get_journal_start (s);
	    from_journal ++;
	}

	free_blocknrs ++;
    }
    
    return CARRY_ON;
}


int reiserfs_new_unf_blocknrs(struct reiserfs_transaction_handle *th, struct super_block * s, unsigned long * free_blocknrs,
				 unsigned long search_start, int amount_needed, int for_preserve_list) {
  return reiserfs_new_blocknrs(th, s, free_blocknrs, search_start, amount_needed, for_preserve_list) ;
}



struct buffer_head * reiserfsck_get_new_buffer (unsigned long start)
{
  unsigned long blocknr = 0;
  struct buffer_head * bh;

  reiserfs_new_blocknrs (0, &g_sb, &blocknr, start, 1, 0);
  
  bh = getblk (g_sb.s_dev, blocknr, g_sb.s_blocksize);
  if (buffer_uptodate (bh))
    die ("reiserfsck_get_new_buffer: found uptodate buffer for new blocknr");

  return bh;
}


/* free block in new bitmap */
void reiserfs_free_block (struct reiserfs_transaction_handle *th, struct super_block * s, unsigned long block)
{
  int i, j;

  i = block / (s->s_blocksize * 8);
  j = block % (s->s_blocksize * 8);

  if (test_bit (j, g_new_bitmap[i]) == 0)
    die ("reiserfs_free_block: Block %lu is free", block);

  clear_bit (j, g_new_bitmap[i]);
  SB_FREE_BLOCKS (&g_sb)++;
}



