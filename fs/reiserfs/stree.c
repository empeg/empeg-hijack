/*
 *  Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */

/*
 *  Written by Anatoly P. Pinchuk pap@namesys.botik.ru
 *  Programm System Institute
 *  Pereslavl-Zalessky Russia
 */

/*
 *  This file contains functions dealing with S+tree
 *
 * comp_keys
 * comp_short_keys
 * bin_search
 * get_lkey
 * get_rkey
 * key_in_buffer
 * decrement_bcount
 * decrement_counters_in_path
 * pathrelse
 * search_by_key
 * search_for_position_by_key
 * comp_items
 * prepare_for_delete_or_cut
 * calc_deleted_bytes_number
 * init_tb_struct
 * reiserfs_delete_item
 * reiserfs_delete_object
 * indirect_to_direct
 * maybe_indirect_to_direct
 * reiserfs_cut_from_item
 * reiserfs_truncate_file
 * reiserfs_paste_into_item
 * reiserfs_insert_item
 * get_buffer_by_range
 * get_buffers_from_range
 */
#ifdef __KERNEL__

#include <linux/sched.h>
#include <linux/string.h>
#include <linux/reiserfs_fs.h>

#else

#include "nokernel.h"

#endif

/* Does the buffer contain a disk block which is in the tree. */
inline int B_IS_IN_TREE (struct buffer_head * p_s_bh)
{

#ifdef REISERFS_CHECK
  if ( B_BLK_HEAD(p_s_bh)->blk_level > MAX_HEIGHT ) {
    reiserfs_panic(0, "PAP-1010: B_IS_IN_TREE: block (%b) has too big level (%z)",
		   p_s_bh, p_s_bh);
  }
#endif

  return ( B_BLK_HEAD(p_s_bh)->blk_level != FREE_LEVEL );
}


inline void copy_key (void * to, void * from)
{
  memcpy (to, from, KEY_SIZE);
}

inline void copy_short_key (void * to, void * from)
{
  memcpy (to, from, SHORT_KEY_SIZE);
}

inline void copy_item_head(void * p_v_to, void * p_v_from)
{
  memcpy (p_v_to, p_v_from, IH_SIZE);
}


/*
 Compare keys using all 4 key fields.
 Returns:  -1 if key1 < key2
            0 if key1 = key2
            1 if key1 > key2
*/
inline int  comp_keys (void * k1, void * k2)
{
  __u32 * p_s_key1, * p_s_key2;
  int n_key_length = REISERFS_FULL_KEY_LEN;

  p_s_key1 = (__u32 *)k1;
  p_s_key2 = (__u32 *)k2;
  for( ; n_key_length--; ++p_s_key1, ++p_s_key2 ) {
    if ( *p_s_key1 < *p_s_key2 )
      return -1;
    if ( *p_s_key1 > *p_s_key2 )
      return 1;
  }

  return 0;
}


/*
 Compare keys using REISERFS_SHORT_KEY_LEN fields.
 Returns:  -1 if key1 < key2
            0 if key1 = key2
            1 if key1 > key2
*/
inline int  comp_short_keys (void * k1, void * k2)
{
  __u32 * p_s_key1, * p_s_key2;
  int n_key_length = REISERFS_SHORT_KEY_LEN;

  p_s_key1 = (__u32 *)k1;
  p_s_key2 = (__u32 *)k2;

  for( ; n_key_length--; ++p_s_key1, ++p_s_key2 ) {
    if ( *p_s_key1 < *p_s_key2 )
      return -1;
    if ( *p_s_key1 > *p_s_key2 )
      return 1;
  }

  return 0;
}








/**************************************************************************
 *  Binary search toolkit function                                        *
 *  Search for an item in the array by the item key                       *
 *  Returns:    1 if found,  0 if not found;                              *
 *        *p_n_pos = number of the searched element if found, else the    *
 *        number of the first element that is larger than p_v_key.        *
 **************************************************************************/
/* For those not familiar with binary search: n_lbound is the leftmost item that it
 could be, n_rbound the rightmost item that it could be.  We examine the item
 halfway between n_lbound and n_rbound, and that tells us either that we can increase
 n_lbound, or decrease n_rbound, or that we have found it, or if n_lbound <= n_rbound that
 there are no possible items, and we have not found it. With each examination we
 cut the number of possible items it could be by one more than half rounded down,
 or we find it. */
inline	int bin_search (
              void    * p_v_key,    /* Key to search for.                   */
	      void    * p_v_base,   /* First item in the array.             */
	      int       p_n_num,    /* Number of items in the array.        */
	      int       p_n_width,  /* Item size in the array.
				       searched. Lest the reader be
				       confused, note that this is crafted
				       as a general function, and when it
				       is applied specifically to the array
				       of item headers in a node, p_n_width
				       is actually the item header size not
				       the item size.                      */
	      int     * p_n_pos     /* Number of the searched for element. */
            ) {
  int   n_rbound, n_lbound, n_j;

  for ( n_j = ((n_rbound = p_n_num - 1) + (n_lbound = 0))/2; n_lbound <= n_rbound; n_j = (n_rbound + n_lbound)/2 )
    switch( COMP_KEYS(((char * )p_v_base + n_j * p_n_width), p_v_key) )  {
    case -1: n_lbound = n_j + 1; continue;
    case  1: n_rbound = n_j - 1; continue;
    case  0: *p_n_pos = n_j;     return ITEM_FOUND; /* Key found in the array.  */
    }

  /* bin_search did not find given key, it returns position of key,
     that is minimal and greater than the given one. */
  *p_n_pos = n_lbound;
  return ITEM_NOT_FOUND;
}

#ifdef REISERFS_CHECK
extern struct tree_balance * cur_tb;
extern struct tree_balance init_tb;
extern int init_item_pos, init_pos_in_item, init_mode;
#endif



/* Minimal possible key. It is never in the tree. */
struct key  MIN_KEY = {0, 0, 0, 0};

/* Maximal possible key. It is never in the tree. */
struct key  MAX_KEY = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};


/* Get delimiting key of the buffer by looking for it in the buffers in the path, starting from the bottom
   of the path, and going upwards.  We must check the path's validity at each step.  If the key is not in
   the path, there is no delimiting key in the tree (buffer is first or last buffer in tree), and in this
   case we return a special key, either MIN_KEY or MAX_KEY. */
inline	struct  key * get_lkey  (
	                struct path         * p_s_chk_path,
                        struct super_block  * p_s_sb
                      ) {
  int                   n_position, n_path_offset = p_s_chk_path->path_length;
  struct buffer_head  * p_s_parent;
  
#ifdef REISERFS_CHECK
  if ( n_path_offset < FIRST_PATH_ELEMENT_OFFSET )
    reiserfs_panic(p_s_sb,"PAP-5010: get_lkey: illegal offset in the path");
#endif

  /* While not higher in path than first element. */
  while ( n_path_offset-- > FIRST_PATH_ELEMENT_OFFSET ) {

#ifdef REISERFS_CHECK
    if ( ! buffer_uptodate(PATH_OFFSET_PBUFFER(p_s_chk_path, n_path_offset)) )
      reiserfs_panic(p_s_sb, "PAP-5020: get_lkey: parent is not uptodate");
#endif

    /* Parent at the path is not in the tree now. */
    if ( ! B_IS_IN_TREE(p_s_parent = PATH_OFFSET_PBUFFER(p_s_chk_path, n_path_offset)) )
      return &MAX_KEY;
    /* Check whether position in the parent is correct. */
    if ( (n_position = PATH_OFFSET_POSITION(p_s_chk_path, n_path_offset)) > B_NR_ITEMS(p_s_parent) )
       return &MAX_KEY;
    /* Check whether parent at the path really points to the child. */
    if ( B_N_CHILD_NUM(p_s_parent, n_position) !=
	 PATH_OFFSET_PBUFFER(p_s_chk_path, n_path_offset + 1)->b_blocknr )
      return &MAX_KEY;
    /* Return delimiting key if position in the parent is not equal to zero. */
    if ( n_position )
      return B_N_PDELIM_KEY(p_s_parent, n_position - 1);
  }
  /* Return MIN_KEY if we are in the root of the buffer tree. */
  if ( PATH_OFFSET_PBUFFER(p_s_chk_path, FIRST_PATH_ELEMENT_OFFSET)->b_blocknr ==
                                      p_s_sb->u.reiserfs_sb.s_rs->s_root_block )
    return &MIN_KEY;
  return  &MAX_KEY;
}


/* Get delimiting key of the buffer at the path and its right neighbor. */
inline	struct  key * get_rkey  (
	                struct path         * p_s_chk_path,
                        struct super_block  * p_s_sb
                      ) {
  int                   n_position,
    			n_path_offset = p_s_chk_path->path_length;
  struct buffer_head  * p_s_parent;

#ifdef REISERFS_CHECK
  if ( n_path_offset < FIRST_PATH_ELEMENT_OFFSET )
    reiserfs_panic(p_s_sb,"PAP-5030: get_rkey: illegal offset in the path");
#endif

  while ( n_path_offset-- > FIRST_PATH_ELEMENT_OFFSET ) {

#ifdef REISERFS_CHECK
    if ( ! buffer_uptodate(PATH_OFFSET_PBUFFER(p_s_chk_path, n_path_offset)) )
      reiserfs_panic(p_s_sb, "PAP-5040: get_rkey: parent is not uptodate");
#endif

    /* Parent at the path is not in the tree now. */
    if ( ! B_IS_IN_TREE(p_s_parent = PATH_OFFSET_PBUFFER(p_s_chk_path, n_path_offset)) )
      return &MIN_KEY;
    /* Check whether position in the parrent is correct. */
    if ( (n_position = PATH_OFFSET_POSITION(p_s_chk_path, n_path_offset)) > B_NR_ITEMS(p_s_parent) )
      return &MIN_KEY;
    /* Check whether parent at the path really points to the child. */
    if ( B_N_CHILD_NUM(p_s_parent, n_position) !=
                                        PATH_OFFSET_PBUFFER(p_s_chk_path, n_path_offset + 1)->b_blocknr )
      return &MIN_KEY;
    /* Return delimiting key if position in the parent is not the last one. */
    if ( n_position != B_NR_ITEMS(p_s_parent) )
      return B_N_PDELIM_KEY(p_s_parent, n_position);
  }
  /* Return MAX_KEY if we are in the root of the buffer tree. */
  if ( PATH_OFFSET_PBUFFER(p_s_chk_path, FIRST_PATH_ELEMENT_OFFSET)->b_blocknr ==
       p_s_sb->u.reiserfs_sb.s_rs->s_root_block )
    return &MAX_KEY;
  return  &MIN_KEY;
}


/* Check whether a key is contained in the tree rooted from a buffer at a path. */
/* This works by looking at the left and right delimiting keys for the buffer in the last path_element in
   the path.  These delimiting keys are stored at least one level above that buffer in the tree. If the
   buffer is the first or last node in the tree order then one of the delimiting keys may be absent, and in
   this case get_lkey and get_rkey return a special key which is MIN_KEY or MAX_KEY. */
static  inline  int key_in_buffer (
                      struct path         * p_s_chk_path, /* Path which should be checked.  */
                      struct key          * p_s_key,      /* Key which should be checked.   */
                      struct super_block  * p_s_sb        /* Super block pointer.           */
		      ) {

#ifdef REISERFS_CHECK
  if ( ! p_s_key || p_s_chk_path->path_length < FIRST_PATH_ELEMENT_OFFSET ||
       p_s_chk_path->path_length > MAX_HEIGHT )
    reiserfs_panic(p_s_sb, "PAP-5050: key_in_buffer:  pointer to the key(%p) is NULL or illegal path length(%d)",
		   p_s_key, p_s_chk_path->path_length);
  
  if ( PATH_PLAST_BUFFER(p_s_chk_path)->b_dev == NODEV )
    reiserfs_panic(p_s_sb, "PAP-5060: key_in_buffer: device must not be NODEV");
#endif

  if ( COMP_KEYS(get_lkey(p_s_chk_path, p_s_sb), p_s_key) == 1 )
    return 0;
  if ( COMP_KEYS(p_s_key, get_rkey(p_s_chk_path, p_s_sb)) != -1 )
    return 0;
  return 1;
}


inline void decrement_bcount(
              struct buffer_head  * p_s_bh
            ) { 
  if ( p_s_bh ) {
    if ( p_s_bh->b_count ) {
      p_s_bh->b_count--;
      return;
    }
    reiserfs_panic(NULL, "PAP-5070: decrement_bcount: trying to free free buffer %b", p_s_bh);
  }
}


/* Decrement b_count field of the all buffers in the path. */
void decrement_counters_in_path (
              struct path * p_s_search_path
            ) {
  int n_path_offset = p_s_search_path->path_length;

#ifdef REISERFS_CHECK
  if ( n_path_offset < ILLEGAL_PATH_ELEMENT_OFFSET ||
       n_path_offset > EXTENDED_MAX_HEIGHT - 1 )
    reiserfs_panic(NULL, "PAP-5080: decrement_counters_in_path: illegal path offset of %d", n_path_offset);
#endif

  while ( n_path_offset > ILLEGAL_PATH_ELEMENT_OFFSET )
    decrement_bcount(PATH_OFFSET_PBUFFER(p_s_search_path, n_path_offset--));
  p_s_search_path->path_length = ILLEGAL_PATH_ELEMENT_OFFSET;
}


/* Release all buffers in the path. */
void  pathrelse (
        struct path * p_s_search_path
      ) {
  int n_path_offset = p_s_search_path->path_length;

#ifdef REISERFS_CHECK
  if ( n_path_offset < ILLEGAL_PATH_ELEMENT_OFFSET )
    reiserfs_panic(NULL, "PAP-5090: pathrelse: illegal path offset");
#endif
  
  while ( n_path_offset > ILLEGAL_PATH_ELEMENT_OFFSET ) 
    brelse(PATH_OFFSET_PBUFFER(p_s_search_path, n_path_offset--));

  p_s_search_path->path_length = ILLEGAL_PATH_ELEMENT_OFFSET;
}


#ifdef SEARCH_BY_KEY_READA

static int search_by_key_reada (struct super_block * s, int blocknr)
{
  struct buffer_head * bh;
  int repeat;
  
  repeat = CARRY_ON;
  if (blocknr == 0)
    return CARRY_ON;

  bh = reiserfs_getblk (s->s_dev, blocknr, s->s_blocksize, &repeat);
  
  if (!buffer_uptodate (bh)) {
    ll_rw_block (READA, 1, &bh);
    repeat = SCHEDULE_OCCURRED;
  }
  bh->b_count --;
  return repeat;
}

#endif

/**************************************************************************
 * Algorithm   SearchByKey                                                *
 *             look for item in the Disk S+Tree by its key                *
 * Input:  p_s_sb   -  super block                                        *
 *         p_s_key  - pointer to the key to search                        *
 * Output: true value -  1 - found,  0 - not found                        *
 *         p_s_search_path - path from the root to the needed leaf        *
 **************************************************************************/

/* This function fills up the path from the root to the leaf as it
   descends the tree looking for the key.  It uses reiserfs_bread to
   try to find buffers in the cache given their block number.  If it
   does not find them in the cache it reads them from disk.  For each
   node search_by_key finds using reiserfs_bread it then uses
   bin_search to look through that node.  bin_search will find the
   position of the block_number of the next node if it is looking
   through an internal node.  If it is looking through a leaf node
   bin_search will find the position of the item which has key either
   equal to given key, or which is the maximal key less than the given
   key.  search_by_key returns a path that must be checked for the
   correctness of the top of the path but need not be checked for the
   correctness of the bottom of the path */
int search_by_key(
                  struct super_block  * p_s_sb,         /* Super block.                           */
                  struct key          * p_s_key,        /* Key to search.                         */
                  struct path         * p_s_search_path,/* This structure was allocated and initialized by
                                                           the calling function. It is filled up by this
                                                           function.  */
                  int                 * p_n_repeat,     /* Whether schedule occured. */
                  int                   n_stop_level,   /* How far down the tree to search.*/
                  int                   n_bread_par     /* Whether to search even if it requires disk I/O, this is
                                                           either READ_BLOCKS or DONT_READ_BLOCKS or 0. Hans doesn't
                                                           know what 0 means, it seems to evaluate to DONT_READ_BLOCKS,
                                                           but it is bad style to not use the macro.... there is a
                                                           #define of search by key with no explanation that can allow
                                                           it to happen.... */
                  ) {
    kdev_t                      n_dev           = p_s_sb->s_dev;
    int                         n_repeat,
                                n_block_number  = p_s_sb->u.reiserfs_sb.s_rs->s_root_block,
                                n_block_size    = p_s_sb->s_blocksize;
    struct buffer_head  *       p_s_bh;
    struct path_element *       p_s_last_element;
    int				n_node_level, n_retval;
    int 			right_neighbor_of_leaf_node;

#ifdef REISERFS_CHECK
    int n_repeat_counter = 0;
#endif

    /* As we add each node to a path we increase its count.  This means that we must be careful to
       release all nodes in a path before we either discard the path struct or re-use the path
       struct, as we do here. */

    decrement_counters_in_path(p_s_search_path);

    *p_n_repeat = CARRY_ON;
    right_neighbor_of_leaf_node = 0;

    /* With each iteration of this loop we search through the items in the current node, and
       calculate the next current node(next path element) for the next iteration of this loop.. */
    while ( 1 ) {

#ifdef REISERFS_CHECK
      if ( !(++n_repeat_counter % 50000) )
	printk ("PAP-5100: search_by_key(pid %u): there were %d searches from the tree_root lokking for key %p\n",
			  current->pid, n_repeat_counter, p_s_key);
#endif

      /* prep path to have another element added to it. */
      p_s_last_element = PATH_OFFSET_PELEMENT(p_s_search_path, ++p_s_search_path->path_length);
      n_repeat = CARRY_ON;

      if ( n_bread_par == READ_BLOCKS ) { 
	/* schedule read of right neighbor */
#ifdef SEARCH_BY_KEY_READA
	n_repeat |= search_by_key_reada (p_s_sb, right_neighbor_of_leaf_node);
#endif

	/* Read the next tree node, and set the last element in the path to have a pointer to it. */
	if ( ! (p_s_bh = p_s_last_element->pe_buffer =
		reiserfs_bread(n_dev, n_block_number, n_block_size, &n_repeat)) ) {
	  p_s_search_path->path_length --;
	  pathrelse(p_s_search_path);
	  *p_n_repeat |= n_repeat;
	  return ITEM_NOT_FOUND;	/* IO error */
	}
      }
      else { /* We are looking for the next tree node in cache. */
	p_s_bh = p_s_last_element->pe_buffer = reiserfs_getblk(n_dev, n_block_number, n_block_size, &n_repeat);
      }

      *p_n_repeat |= n_repeat;

      /* It is possible that schedule occured. We must check whether the key to search is still in
	 the tree rooted from the current buffer. If not then repeat search from the root. */
      if ( n_repeat != CARRY_ON && ((buffer_uptodate (p_s_bh) && !B_IS_IN_TREE (p_s_bh)) ||
				    (! key_in_buffer(p_s_search_path, p_s_key, p_s_sb))) ) { /* in fact this checks whether path is correct */
	decrement_counters_in_path(p_s_search_path);
	
	/* Get the root block number so that we can repeat the search starting from the root. */
	n_block_number  = p_s_sb->u.reiserfs_sb.s_rs->s_root_block;

	right_neighbor_of_leaf_node = 0;

	/* repeat search from the root */
	continue;
      }
      
#ifdef REISERFS_CHECK

      if ( ! key_in_buffer(p_s_search_path, p_s_key, p_s_sb) )
	reiserfs_panic(p_s_sb, "PAP-5130: search_by_key: key is not in the buffer");
      if ( cur_tb ) {
/*	print_tb (init_mode, init_item_pos, init_pos_in_item, &init_tb, "5140");*/
	reiserfs_panic(p_s_sb, "PAP-5140: search_by_key: schedule occurred in do_balance!");
      }

#endif

      if ( ! buffer_uptodate(p_s_bh) ) {

#ifdef REISERFS_CHECK
	if ( n_bread_par != DONT_READ_BLOCKS )
	  reiserfs_panic(p_s_sb, "PAP-5150: search_by_key: buffer is not uptodate in case of READ_BLOCKS");
#endif

	return ITEM_NOT_FOUND; /* We can not continue search in the cache. */
      }

      /* ok, we have acquired next formatted node in the tree */
      n_node_level = B_BLK_HEAD(p_s_bh)->blk_level;

#ifdef REISERFS_CHECK

      if (n_node_level < n_stop_level)
	reiserfs_panic (p_s_sb, "vs-5152: search_by_key: tree level is less than stop level (%d)",
			n_node_level, n_stop_level);

#endif

      n_retval = bin_search (p_s_key, B_N_PITEM_HEAD(p_s_bh, 0), B_NR_ITEMS(p_s_bh),
		       ( n_node_level == DISK_LEAF_NODE_LEVEL ) ? IH_SIZE : KEY_SIZE, &(p_s_last_element->pe_position));
      if (n_node_level == n_stop_level)
	return n_retval;

      /* we are not in the stop level */
      if (n_retval == ITEM_FOUND)
	/* item has been found, so we choose the pointer which is to the right of the found one */
	p_s_last_element->pe_position++;
      /* if item was not found we choose the position which is to the left of the found item. This
	 requires no code, bin_search did it already.*/


      /* So we have chosen a position in the current node which is an
	 internal node.  Now we calculate child block number by position in the node. */
      n_block_number = B_N_CHILD_NUM(p_s_bh, p_s_last_element->pe_position);

#ifdef SEARCH_BY_KEY_READA
      /* if we are going to read leaf node, then calculate its right neighbor if possible */
      if (n_node_level == DISK_LEAF_NODE_LEVEL + 1 && p_s_last_element->pe_position < B_NR_ITEMS (p_s_bh))
	right_neighbor_of_leaf_node = B_N_CHILD_NUM(p_s_bh, p_s_last_element->pe_position + 1);
#endif
    }
}


/* Form the path to an item and position in this item which contains file byte defined by p_s_key. If there
    is no such item corresponding to the key, we point the path to the item with maximal key less than
    p_s_key, and *p_n_pos_in_item is set to one past the last entry/byte in the item.  If searching for
    entry in a directory item, and it is not found, *p_n_pos_in_item is set to one entry more than the entry with
    maximal key which is less than the sought key.  

    Note that if there is no entry in this same node which is one more, then we point to an imaginary entry.

    for direct items, the position is in units of bytes, for
    indirect items the position is in units of blocknr entries, for directory items the position is in units
    of directory entries.  */
int search_for_position_by_key (
      struct super_block  * p_s_sb,         /* Pointer to the super block.          */
      struct key          * p_s_key,        /* Key to search.                       */
      struct path         * p_s_search_path,/* Filled up by this function.          */
      int		  * p_n_pos_in_item,/* returned value, which is the found position in the item */
      int                 * p_n_repeat	    /* Whether schedule occured. */
    ) {
  struct item_head    * p_s_ih;
  int                   n_blk_size;

  /* If searching for directory entry. */
  if ( KEY_IS_DIRECTORY_KEY(p_s_key) )
    return  search_by_entry_key(p_s_sb, p_s_key, p_s_search_path, p_n_pos_in_item, p_n_repeat);

  /* If not searching for directory entry. */

  /* If item is found. */
  if ( search_by_key(p_s_sb, p_s_key, p_s_search_path, p_n_repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) == ITEM_FOUND )  {

#ifdef REISERFS_CHECK
    if ( ! B_N_PITEM_HEAD(PATH_PLAST_BUFFER(p_s_search_path),
			  PATH_LAST_POSITION(p_s_search_path))->ih_item_len )
      reiserfs_panic(p_s_sb, "PAP-5165: search_for_position_by_key: item length equals zero");
#endif

    *p_n_pos_in_item = 0;
    return POSITION_FOUND;
  }

#ifdef REISERFS_CHECK
  if ( ! PATH_LAST_POSITION(p_s_search_path) )
    reiserfs_panic(p_s_sb, "PAP-5170: search_for_position_by_key: position equals zero");
#endif

  /* Item is not found. Set path to the previous item. */
  p_s_ih = B_N_PITEM_HEAD(PATH_PLAST_BUFFER(p_s_search_path), --PATH_LAST_POSITION(p_s_search_path));
  n_blk_size = p_s_sb->s_blocksize;

/*#ifdef REISERFS_CHECK */
  if ( COMP_SHORT_KEYS(&(p_s_ih->ih_key), p_s_key) ) {
    reiserfs_panic(p_s_sb, "PAP-5180: search_for_position_by_key: found item %h belongs to an other object %k",
		   p_s_ih, p_s_key);
  }

  if ( ! I_IS_STAT_DATA_ITEM(p_s_ih) && ((KEY_IS_INDIRECT_KEY(p_s_key) && ! I_IS_INDIRECT_ITEM(p_s_ih)) ||
					 (KEY_IS_DIRECT_KEY(p_s_key) && ! I_IS_DIRECT_ITEM(p_s_ih))) ) {
    print_block (PATH_PLAST_BUFFER(p_s_search_path), PRINT_LEAF_ITEMS, 
		 PATH_LAST_POSITION (p_s_search_path) - 2,
		 PATH_LAST_POSITION (p_s_search_path) + 2);
    reiserfs_panic(p_s_sb, "PAP-5190: search_for_position_by_key: found item %h type does not match to the expected one %k",
		   p_s_key, p_s_ih);
  }
/*#endif*/

  /* Needed byte is contained in the item pointed to by the path.*/
  if ( I_K_KEY_IN_ITEM(p_s_ih, p_s_key, n_blk_size) )  {
    *p_n_pos_in_item = p_s_key->k_offset - p_s_ih->ih_key.k_offset;
    if ( I_IS_INDIRECT_ITEM(p_s_ih) )
      *p_n_pos_in_item /= n_blk_size;
    return POSITION_FOUND;
  }

  /* Needed byte is not contained in the item pointed to by the path. Set *p_n_pos_in_item out of the
     item. */
  if ( I_IS_INDIRECT_ITEM(p_s_ih) )
    *p_n_pos_in_item = I_UNFM_NUM(p_s_ih);
  else
    *p_n_pos_in_item = p_s_ih->ih_item_len;
  return POSITION_NOT_FOUND;
}


/* Compare given item and item pointed to by the path. */
int comp_items(
      struct item_head  * p_s_ih,
      struct path       * p_s_path
    ) {
  struct buffer_head  * p_s_bh;
  struct item_head    * p_s_path_item;

  /* Last buffer at the path is not in the tree. */
  if ( ! B_IS_IN_TREE(p_s_bh = PATH_PLAST_BUFFER(p_s_path)) )
    return 1;

#ifdef REISERFS_CHECK
    if ( p_s_bh->b_dev == NODEV )
      reiserfs_panic(0, "PAP-5200: comp_items: device is invalid");
#endif

  /* Last path position is invalid. */
  if ( PATH_LAST_POSITION(p_s_path) >= B_NR_ITEMS(p_s_bh) )
    return 1;
  /* Get item at the path. */
  p_s_path_item = PATH_PITEM_HEAD(p_s_path);
  /* Compare keys. */
  if ( COMP_KEYS(p_s_path_item, p_s_ih) )
    return 1;
  /* Compare other items fields. */
  if ( p_s_path_item->u.ih_free_space != p_s_ih->u.ih_free_space ||
       p_s_path_item->ih_item_len != p_s_ih->ih_item_len ||
       p_s_path_item->ih_item_location != p_s_ih->ih_item_location )
    return 1;
  /* Items are equal. */
  return 0;
}


/*  If the path points to a directory or direct item, calculate mode and the size cut, for balance.
    If the path points to an indirect item, remove some number of its unformatted nodes.
    In case of file truncate calculate whether this item must be deleted/truncated or last
    unformatted node of this item will be converted to a direct item.
    This function returns a determination of what balance mode the calling function should employ. */
static char  prepare_for_delete_or_cut(
    struct reiserfs_transaction_handle *th,
    struct inode * inode,
    struct path         * p_s_path,
    struct key          * p_s_item_key,
    int                 * p_n_pos_in_item,
    int                 * p_n_removed,      /* Number of unformatted nodes which were removed
					       from end of the file. */
    int                 * p_n_cut_size,
    unsigned long         n_new_file_length, /* MAX_KEY_OFFSET in case of delete. */
    int		      preserve_mode,
    int * was_unfm_suspected_recipient
    ) {
    struct super_block  * p_s_sb = inode->i_sb;
    struct item_head    * p_s_ih = PATH_PITEM_HEAD(p_s_path);
    struct buffer_head  * p_s_bh = PATH_PLAST_BUFFER(p_s_path);

#ifdef REISERFS_CHECK
    int n_repeat_counter = 0;
#endif

    /* Stat_data item. */
    if ( I_IS_STAT_DATA_ITEM(p_s_ih) ) {

#ifdef REISERFS_CHECK
	if ( n_new_file_length != MAX_KEY_OFFSET )
	    reiserfs_panic(p_s_sb, "PAP-5210: prepare_for_delete_or_cut: mode must be M_DELETE");
#endif

	*p_n_cut_size = -(IH_SIZE + p_s_ih->ih_item_len);
	return M_DELETE;
    }

    /* Directory item. */
    if ( I_IS_DIRECTORY_ITEM(p_s_ih) ) {
	if (p_s_ih->ih_key.k_offset == DOT_OFFSET && n_new_file_length == MAX_KEY_OFFSET) {
#ifdef REISERFS_CHECK
	    if (p_s_ih->ih_key.k_uniqueness != DIRENTRY_UNIQUENESS/*DOT_UNIQUENESS*/ || I_ENTRY_COUNT (p_s_ih) != 2)
		reiserfs_panic(p_s_sb,"PAP-5220: prepare_for_delete_or_cut: "
			       "empty directory item has uniqueness==%lu and entry count==%d", 
			       p_s_ih->ih_key.k_uniqueness, I_ENTRY_COUNT (p_s_ih));
#endif
	    *p_n_cut_size = -(IH_SIZE + p_s_ih->ih_item_len);
	    return M_DELETE; /* Delete the directory item containing "." and ".." entry. */
	}

	if ( I_ENTRY_COUNT(p_s_ih) == 1 )  {
	    *p_n_cut_size = -(IH_SIZE + p_s_ih->ih_item_len);
	    return M_DELETE; /* Delete the directory item such as there is one record only in this item. */
	}
	*p_n_cut_size = -(DEH_SIZE +
			  I_DEH_N_ENTRY_LENGTH(p_s_ih, B_I_DEH(p_s_bh,p_s_ih) +
					       *p_n_pos_in_item, *p_n_pos_in_item));
	return M_CUT; /* Cut one record from the directory item. */
    }

#ifdef REISERFS_CHECK
    if ( ! p_s_ih->ih_key.k_offset )
	reiserfs_panic(p_s_sb, "PAP-5230: prepare_for_delete_or_cut: k_offset is NULL");
#endif

    /* Direct item. */
    if ( I_IS_DIRECT_ITEM(p_s_ih) ) {
	if ( n_new_file_length == MAX_KEY_OFFSET ) { /* Case of delete. */
	    *p_n_cut_size = -(IH_SIZE + p_s_ih->ih_item_len);
	    return M_DELETE; /* Delete this item. */
	}
	/* Case of truncate. */
	if ( n_new_file_length < p_s_ih->ih_key.k_offset )  {
	    *p_n_cut_size = -(IH_SIZE + p_s_ih->ih_item_len);
	    return M_DELETE; /* Delete this item. */
	}
	/* Calculate first position and size for cutting from item. */
	*p_n_cut_size = -(p_s_ih->ih_item_len -
			  (*p_n_pos_in_item = n_new_file_length + 1 - p_s_ih->ih_key.k_offset));
	return M_CUT; /* Cut from this item. */
    }

    /* Case of an indirect item. */
    {
	int                   n_unfm_number,    /* Number of the item unformatted nodes. */
	    n_counter,
	    n_repeat,
	    n_retry,        /* Set to one if there is unformatted node buffer in use. */
	    n_blk_size;
	unsigned long       * p_n_unfm_pointer; /* Pointer to the unformatted node number. */
	struct item_head      s_ih;           /* Item header. */
	char                  c_mode;           /* Returned mode of the balance. */
	struct buffer_head  * p_s_un_bh;


	n_blk_size = p_s_sb->s_blocksize;

	/* Search for the needed object indirect item until there are no unformatted nodes to be removed. */
	do  {
	    /* Copy indirect item header to a temp variable. */
	    copy_item_head(&s_ih, PATH_PITEM_HEAD(p_s_path));
	    /* Calculate number of unformatted nodes in this item. */
	    n_unfm_number = I_UNFM_NUM(&s_ih);

#ifdef REISERFS_CHECK
	    if ( ! I_IS_INDIRECT_ITEM(&s_ih) || ! n_unfm_number ||
		 *p_n_pos_in_item + 1 !=  n_unfm_number ) {
		printk("n_unfm_number = %d *p_n_pos_in_item = %d\n",n_unfm_number, *p_n_pos_in_item);
		reiserfs_panic(p_s_sb, "PAP-5240: prepare_for_delete_or_cut: illegal item %h", &s_ih);
	    }
#endif

	    /* Calculate balance mode and position in the item to remove unformatted nodes. */
	    if ( n_new_file_length == MAX_KEY_OFFSET ) {/* Case of delete. */
		*p_n_pos_in_item = 0;
		*p_n_cut_size = -(IH_SIZE + s_ih.ih_item_len);
		c_mode = M_DELETE;
	    }
	    else  { /* Case of truncate. */
		if ( n_new_file_length < s_ih.ih_key.k_offset )  {
		    *p_n_pos_in_item = 0;
		    *p_n_cut_size = -(IH_SIZE + s_ih.ih_item_len);
		    c_mode = M_DELETE; /* Delete this item. */
		}
		else  {
		    /* indirect item must be truncated starting from *p_n_pos_in_item-th position */
		    *p_n_pos_in_item = (n_new_file_length + n_blk_size - s_ih.ih_key.k_offset ) / n_blk_size;

#ifdef REISERFS_CHECK
		    if ( *p_n_pos_in_item > n_unfm_number ) 
			reiserfs_panic(p_s_sb, "PAP-5250: prepare_for_delete_or_cut: illegal position in the item");
#endif

		    /* Either convert last unformatted node of indirect item to direct item or increase
		       its free space.  */
		    if ( *p_n_pos_in_item == n_unfm_number )  {
			*p_n_cut_size = 0; /* Nothing to cut. */
			return M_CONVERT; /* Maybe convert last unformatted node to the direct item. */
		    }
		    /* Calculate size to cut. */
		    *p_n_cut_size = -(s_ih.ih_item_len - *p_n_pos_in_item * UNFM_P_SIZE);

		    c_mode = M_CUT;     /* Cut from this indirect item. */
		}
	    }

#ifdef REISERFS_CHECK
	    if ( n_unfm_number <= *p_n_pos_in_item ) 
		reiserfs_panic(p_s_sb, "PAP-5260: prepare_for_delete_or_cut: illegal position in the indirect item");
#endif

	    /* pointers to be cut */
	    n_unfm_number -= *p_n_pos_in_item;
	    /* Set pointer to the last unformatted node pointer that is to be cut. */
	    p_n_unfm_pointer = (unsigned long *)B_I_PITEM(PATH_PLAST_BUFFER(p_s_path),&s_ih) + I_UNFM_NUM(&s_ih) - 1 - *p_n_removed;

	    /* We go through the unformatted nodes pointers of the indirect item and look for
	       the unformatted nodes in the cache. If we found some of them we free it and zero
	       corresponding indirect item entry. If some unformatted node has b_count > 1 we must
	       not free this unformatted node since it is in use. */
	    for ( n_retry = 0, n_counter = *p_n_removed;
		  n_counter < n_unfm_number; n_counter++, p_n_unfm_pointer-- )  {
		if (comp_items(&s_ih, p_s_path))
		    break;
#ifdef REISERFS_CHECK
		if (p_n_unfm_pointer < (unsigned long *)B_I_PITEM(PATH_PLAST_BUFFER(p_s_path),&s_ih) ||
		    p_n_unfm_pointer > (unsigned long *)B_I_PITEM(PATH_PLAST_BUFFER(p_s_path),&s_ih) + I_UNFM_NUM(&s_ih) - 1)
		    reiserfs_panic (p_s_sb, "vs-5265: prepare_for_delete_or_cut: pointer out of range");
#endif
		if ( ! *p_n_unfm_pointer )  { /* Hole, nothing to remove. */
		    if ( ! n_retry )
			(*p_n_removed)++;
		    continue;
		}
		/* Search for the buffer in cache. */
		n_repeat = CARRY_ON;
		p_s_un_bh = reiserfs_get_hash_table(p_s_sb->s_dev, *p_n_unfm_pointer,
						    n_blk_size, &n_repeat);
		/* Current item was shifted from buffer pointed to by the path. */
		if ( n_repeat != CARRY_ON && comp_items(&s_ih, p_s_path) )  {
		    brelse(p_s_un_bh);
		    break;
		}

		/* Block is in use. */
		/* BUG, find a better test -- CLM */
		if ( p_s_un_bh && p_s_un_bh->b_count != 1)  {
		    if ((buffer_journaled(p_s_un_bh) || buffer_journal_dirty(p_s_un_bh)) && p_s_un_bh->b_count == 2) {
			;
		    } else {

#ifdef REISERFS_CHECK_ONE_PROCESS
			reiserfs_panic(p_s_sb, "PAP-5270: prepare_for_delete_or_cut: b_count != 1");
#endif

			n_retry = 1;
			brelse(p_s_un_bh);
			continue;
		    }
		}
      
		if ( ! n_retry )
		    (*p_n_removed)++;
      
#ifdef REISERFS_CHECK
		if ( p_s_un_bh && (*p_n_unfm_pointer != p_s_un_bh->b_blocknr || buffer_locked (p_s_un_bh)))
		    reiserfs_panic(p_s_sb, "PAP-5280: prepare_for_delete_or_cut: blocks numbers are different");	
#endif

		{
		    __u32 block_addr = *p_n_unfm_pointer;
		    *p_n_unfm_pointer = 0;
		    journal_mark_dirty(th, p_s_sb, PATH_PLAST_BUFFER(p_s_path));
		    if (p_s_un_bh) {
			mark_buffer_clean (p_s_un_bh);
			brelse (p_s_un_bh);
		    }
		    reiserfs_free_block(th, p_s_sb, block_addr);
		    /* non-atomic refile_buffer is allowed */
		    COMPLETE_BITMAP_DIRTING_AFTER_FREEING (p_s_sb, block_addr / (p_s_sb->s_blocksize * 8));		    
		}
#if 0
		/* journal BUG, we should be dealing with this! */
		if (preserve_mode == PRESERVE_INDIRECT_TO_DIRECT) {
		    /* preserve block in case of indirect_to_direct conversion */
		    if (!p_s_un_bh || !is_buffer_suspected_recipient (p_s_sb, p_s_un_bh)) {
			add_to_preserve (*p_n_unfm_pointer, p_s_sb);
		    } else {
			if (was_unfm_suspected_recipient == 0)
			    reiserfs_panic (p_s_sb, "vs-5282: prepare_for_delete_or_cut: can not set \'was unfm suspect recipient\' flag");
			*was_unfm_suspected_recipient = 1;
		    }
		    /* leaves contents of this node falled to are marked as suspected recipient already */
		    unmark_suspected_recipient (p_s_sb, p_s_un_bh);
		    if (n_unfm_number != 1)
			reiserfs_panic (p_s_sb, "PAP-5285: prepare_for_delete_or_cut: "
					"indirect_to_direct must cut only one pointer (not %d)", n_unfm_number);
		} else {
		    __u32 block_addr = *p_n_unfm_pointer;
 
		    *p_n_unfm_pointer = 0;
		    journal_mark_dirty(th, p_s_sb, PATH_PLAST_BUFFER(p_s_path));
 
		    /* Free block and buffer in case of removal or cutting. */
		    unmark_suspected_recipient (p_s_sb, p_s_un_bh);
		    if (p_s_un_bh) {
			mark_buffer_clean (p_s_un_bh);
			brelse (p_s_un_bh);
		    }
 
		    reiserfs_free_block(th, p_s_sb, block_addr);
		    /* non-atomic refile_buffer is allowed */
		    COMPLETE_BITMAP_DIRTING_AFTER_FREEING (p_s_sb, block_addr / (p_s_sb->s_blocksize * 8));

		}
#endif /*0*/
		inode->i_blocks -= p_s_sb->s_blocksize / 512;
	    } /* for */

	    /* There is block in use. */
	    if ( n_retry )  {

#ifdef REISERFS_CHECK
		if ( *p_n_removed >= n_unfm_number )
		    reiserfs_panic(p_s_sb, "PAP-5290: prepare_for_delete_or_cut: illegal case");
		if ( !(++n_repeat_counter % 50000) ) {
		    printk ("5300: new file length = %ld\n", n_new_file_length);
		    reiserfs_warning("PAP-5300: prepare_for_delete_or_cut: (pid %u): "
				     "could not delete item %k in (%d) iterations. Still trying",
				     current->pid, p_s_item_key, n_repeat_counter);
		}
#endif

#ifdef __KERNEL__
		current->policy |= SCHED_YIELD;
		schedule();
#endif
	    }
	    /* This loop can be optimized. */
	} while ( *p_n_removed < n_unfm_number &&
		  search_for_position_by_key(p_s_sb, p_s_item_key, p_s_path, p_n_pos_in_item, &n_repeat) == POSITION_FOUND );

#ifdef REISERFS_CHECK
	if ( *p_n_removed < n_unfm_number )
	    reiserfs_panic(p_s_sb, "PAP-5310: prepare_for_delete_or_cut: indirect item is not found");

	if ( comp_items(&s_ih, p_s_path) ) {
	    printk("*p_n_removed = %d n_unfm_number = %d\n",*p_n_removed, n_unfm_number);
	    reiserfs_panic(p_s_sb, "PAP-5312: prepare_for_delete_or_cut: path to item %h has been unexpectedly changed",
			   &s_ih);
	}
#endif

	if (c_mode == M_CUT)
	    *p_n_pos_in_item *= UNFM_P_SIZE;
	return c_mode;
    }
}


/* Calculate bytes number which will be deleted or cutted in the balance. */
int calc_deleted_bytes_number(
      struct  tree_balance  * p_s_tb,
      char                    c_mode
    ) {
  int                     n_del_size;
  struct  item_head     * p_s_ih = PATH_PITEM_HEAD(p_s_tb->tb_path);

  if ( I_IS_STAT_DATA_ITEM(p_s_ih) )
    return 0;

  if ( I_IS_DIRECTORY_ITEM(p_s_ih) )
    return EMPTY_DIR_SIZE; /* We delete emty directoris only. */

  n_del_size = ( c_mode == M_DELETE ) ? p_s_ih->ih_item_len : -p_s_tb->insert_size[0];

  if ( I_IS_INDIRECT_ITEM(p_s_ih) )
    n_del_size = (n_del_size/UNFM_P_SIZE)*
      (PATH_PLAST_BUFFER(p_s_tb->tb_path)->b_size) - p_s_ih->u.ih_free_space;
  return n_del_size;
}

static void init_tb_struct(
              struct tree_balance * p_s_tb,
	      struct super_block  * p_s_sb,
	      struct path         * p_s_path,
              int                   n_size
            ) {
 memset (p_s_tb,'\0',sizeof(struct tree_balance));
 p_s_tb->tb_sb = p_s_sb;
 p_s_tb->tb_path = p_s_path;
 PATH_OFFSET_PBUFFER(p_s_path, ILLEGAL_PATH_ELEMENT_OFFSET) = NULL;
 PATH_OFFSET_POSITION(p_s_path, ILLEGAL_PATH_ELEMENT_OFFSET) = 0;
 p_s_tb->insert_size[0] = n_size;
}


#if 0
/* bh contains direct item to be preserved, unfm is unformatted node direct item is copied to, unfm
   becomes suspected recipient */
static void preserve_direct_item (struct tree_balance * tb, struct buffer_head * unfm)
{
/*  struct buffer_info bi;

  bi.bi_bh = PATH_PLAST_BUFFER (tb->tb_path);
  bi.bi_parent = PATH_H_PPARENT (tb->tb_path, 0);
  bi.bi_position = PATH_H_B_ITEM_ORDER (tb->tb_path, 0);*/

  /* unfm is to become suspected recipient */
  preserve_shifted (tb, &PATH_PLAST_BUFFER (tb->tb_path), PATH_H_PPARENT (tb->tb_path, 0), PATH_H_B_ITEM_ORDER (tb->tb_path, 0), unfm);
}


/* it is called in reiserfs_cut_from_item before call to do_balance, which is going to cut last
   unformatted pointer of indirect item. Direct item is in tree already, and buffers containing it
   are marked as suspected recipients. Last unformatted pointer is preserved by (added to preserve list)
   prepare_for_cut_or_delete and corresponding node is forgotten */
void preserve_indirect_item (struct tree_balance * tb)
{
/*  struct buffer_info bi;
  
  bi.bi_bh = PATH_PLAST_BUFFER (tb->tb_path);
  bi.bi_parent = PATH_H_PPARENT (tb->tb_path, 0);
  bi.bi_position = PATH_H_B_ITEM_ORDER (tb->tb_path, 0);*/
  preserve_shifted (tb, &PATH_PLAST_BUFFER (tb->tb_path), PATH_H_PPARENT (tb->tb_path, 0), PATH_H_B_ITEM_ORDER (tb->tb_path, 0), 0);
}


/* it is called in reiserfs_cut_from_item before call to do_balance if
   it is cutting old entry in reiserfs_rename. Node containing new
   entry is marked as suspected recipient */
void preserve_entry (struct tree_balance * tb)
{
  preserve_indirect_item (tb);
}

#endif /*0*/

/* Delete object item. */
int reiserfs_delete_item(
      struct reiserfs_transaction_handle *th,
      struct inode * p_s_inode,
      struct path         * p_s_path,     /* Path to the deleted item.            */
      int		  * p_n_pos_in_item,
      struct key          * p_s_item_key, /* Key to search for the deleted item.  */
      struct buffer_head  * p_s_un_bh,    /* NULL or unformatted node pointer.    */
      int                 preserve_mode   /* can be PRESERVE_DIRECT_TO_INDIRECT or NOTHING_SPECIAL */
    ) {
  struct super_block * p_s_sb = p_s_inode->i_sb;
  struct buffer_head *bh ;
  struct tree_balance   s_del_balance;
  struct item_head      s_ih;
  int                   n_repeat,
                        n_ret_value,
                        n_del_size,
                        n_removed;

#ifdef REISERFS_CHECK
  char                  c_mode;
  int			n_iter = 0;
#endif

  init_tb_struct(&s_del_balance, p_s_sb, p_s_path, 0);
  s_del_balance.preserve_mode = preserve_mode;

  while ( 1 ) {
    n_removed = 0;

#ifdef REISERFS_CHECK
    n_iter++;
    c_mode =
#endif

    prepare_for_delete_or_cut(th, p_s_inode, p_s_path, p_s_item_key, p_n_pos_in_item, &n_removed, &n_del_size, MAX_KEY_OFFSET, NOTHING_SPECIAL, 0);

#ifdef REISERFS_CHECK
    if ( c_mode != M_DELETE )
      reiserfs_panic(p_s_sb, "PAP-5320: reiserfs_delete_item: mode must be M_DELETE");
#endif

    copy_item_head(&s_ih, PATH_PITEM_HEAD(p_s_path));
    s_del_balance.insert_size[0] = n_del_size;

#ifdef REISERFS_CHECK
    if ( ( ! KEY_IS_STAT_DATA_KEY(p_s_item_key) && ! KEY_IS_DIRECTORY_KEY(p_s_item_key) &&
	   ! I_K_KEY_IN_ITEM(PATH_PITEM_HEAD(s_del_balance.tb_path), p_s_item_key, p_s_sb->s_blocksize))  ||
	 p_s_item_key->k_uniqueness != PATH_PITEM_HEAD(s_del_balance.tb_path)->ih_key.k_uniqueness ) {
      reiserfs_panic(p_s_sb, "PAP-5325: reiserfs_delete_item: (iteration %d): "
		     "key %k does not correspond to the found item %h", n_iter, p_s_item_key,
		     PATH_PITEM_HEAD(s_del_balance.tb_path));
    }

    if ( KEY_IS_DIRECTORY_KEY(p_s_item_key) && (p_s_item_key->k_uniqueness != DIRENTRY_UNIQUENESS/*DOT_DOT_UNIQUENESS*/ &&
						I_ENTRY_COUNT(PATH_PITEM_HEAD(s_del_balance.tb_path)) != 2) )
      reiserfs_panic(p_s_sb, "PAP-5327: reiserfs_delete_item(%d): key does not correspond to the item(directory case)", n_iter);

    if ( PATH_LAST_POSITION(s_del_balance.tb_path) >= B_NR_ITEMS(PATH_PLAST_BUFFER(s_del_balance.tb_path)) ) {
      reiserfs_panic(p_s_sb, "PAP-5330: reiserfs_delete_item: invalid item number (%d) iter = %d, must be < %d. item to delete key %k", 
		     PATH_LAST_POSITION(s_del_balance.tb_path), n_iter,
		     B_NR_ITEMS(PATH_PLAST_BUFFER(s_del_balance.tb_path)), p_s_item_key);
    }
#endif

    n_ret_value = fix_nodes(th, M_DELETE, &s_del_balance, 0, NULL);

#ifdef REISERFS_CHECK_ONE_PROCESS
    if ( n_ret_value == PATH_INCORRECT )
      reiserfs_panic(p_s_sb,"PAP-5340: reiserfs_delete_item: illegal returned value");
#endif

    if ( n_ret_value != SCHEDULE_OCCURRED && n_ret_value != PATH_INCORRECT )
      break;
    /* schedule() occured while make_balance() worked */
    if ( search_for_position_by_key(p_s_sb, p_s_item_key, p_s_path, p_n_pos_in_item, &n_repeat) == POSITION_NOT_FOUND )
      reiserfs_panic(p_s_sb, "PAP-5350: reiserfs_delete_item: item to delete does not exist");
  }
  if ( n_ret_value == NO_DISK_SPACE || n_ret_value == IO_ERROR ) {
    unfix_nodes(th, &s_del_balance);
    return 0;
  }
  journal_lock_dobalance(p_s_sb) ;
 
  /* Here n_ret_value equals CARRY_ON. */
  n_ret_value = calc_deleted_bytes_number(&s_del_balance, M_DELETE);

  if ( p_s_un_bh )  {
    /* We are deleting direct items in a tail, that we are converting
       into an unformatted node. */

#ifdef REISERFS_CHECK
    if ( ! I_IS_DIRECT_ITEM(&s_ih) || ! buffer_uptodate(p_s_un_bh) || 
       ((p_s_un_bh->b_count != 1) && !buffer_journaled(p_s_un_bh)) ) {
      reiserfs_panic(p_s_sb,"PAP-5370: reiserfs_delete_item: illegal unformatted node buffer %b or item type %h)",
		     p_s_un_bh, &s_ih);
    }
#endif

    memcpy(p_s_un_bh->b_data + (s_ih.ih_key.k_offset - 1) % (p_s_sb->s_blocksize),
	   B_I_PITEM(PATH_PLAST_BUFFER(p_s_path), &s_ih), n_ret_value);
#ifdef REISERFS_CHECK
    if ( preserve_mode != PRESERVE_DIRECT_TO_INDIRECT )
      reiserfs_panic(p_s_sb, "PAP-5380: reiserfs_delete_item: "
		     "you need to change the code to check converting_tail before preserving");
#endif /* REISERFS_CHECK */

    /* brelse (p_s_un_bh) will complete mark_buffer_dirty */
    /* reiserfs_mark_buffer_dirty(p_s_un_bh, 0); journal victim */
    /* BUG? should I be journaling more here, should I journal the p_s_un_bh at all */
    bh = PATH_PLAST_BUFFER(p_s_path) ;
    journal_mark_dirty(th, p_s_sb, p_s_un_bh) ;


    /* preserve node containing direct item that is being deleted and
       mark unformatted node (p_s_un_bh) getting contents of direct
       item as suspected recipient */
/*    preserve_direct_item (&s_del_balance, p_s_un_bh);*/

  }

  /* Perform balancing after all resources will be collected at once. */ 
  do_balance(th, &s_del_balance, 0, NULL, NULL, M_DELETE, REISERFS_KERNEL_MEM, 0/* zeros number */);
  journal_unlock_dobalance(p_s_sb) ;

  /* Return deleted body length */ 
  return n_ret_value;
}


/* Summary Of Mechanisms For Handling Collisions Between Processes:

 deletion of the body of the object is performed by iput(), with the
 result that if multiple processes are operating on a file, the
 deletion of the body of the file is deferred until the last process
 that has an open inode performs its iput().

 writes and truncates are protected from collisions by use of
 semaphores.

 creates, linking, and mknod are protected from collisions with other
 processes by making the reiserfs_add_entry() the last step in the
 creation, and then rolling back all changes if there was a collision.
 - Hans
*/

/* Delete all items of an object. */
/* This would work faster if it did not use search by key to access data one node at a time, but instead
   resembled read more by doing read ahead. */
void  reiserfs_delete_object(
	struct reiserfs_transaction_handle *th, 
        struct inode  * p_s_inode       /* Pointer to the object inode. */
      ) {
  struct path           s_search_path;  /* Path to the last object item. */
  struct key            s_item_key;     /* Key to search for a file item. */  
  unsigned long         n_obj_size;     /* Object size. */
  int                   n_repeat,
                        n_deleted,      /* Number of deleted bytes. */
                        n_pos_in_item,  /* Found position in the item. */
    			n_is_last_item = 1;

  struct super_block *  p_s_sb = p_s_inode->i_sb;

  
  init_path (&s_search_path);
  /* Copy key of the object stat_data. */
  copy_key(&s_item_key, INODE_PKEY(p_s_inode));

  /* Get object size. */
  n_obj_size = p_s_inode->i_size;
  /* Case of a directory. */
  if ( S_ISDIR(p_s_inode->i_mode) ) {

#ifdef REISERFS_CHECK
  /* reiserfs_delete_object is called to delete a directory only for empty directories. */
    if ( n_obj_size != EMPTY_DIR_SIZE && n_obj_size != 0 )
      reiserfs_panic (p_s_sb, "PAP-5390: reiserfs_delete_object: bad empty directory sdize (%lu)", n_obj_size);
#endif

    /* Set key to search for the ".." directory entry. */
    s_item_key.k_offset = DOT_DOT_OFFSET;
    s_item_key.k_uniqueness = DIRENTRY_UNIQUENESS/*DOT_DOT_UNIQUENESS*/;
  }
  else  {
    /* Set key to search for the last file byte. */
    if ( (s_item_key.k_offset = n_obj_size) >= p_s_inode->u.reiserfs_i.i_first_direct_byte )
      s_item_key.k_uniqueness = TYPE_DIRECT;
    else
      s_item_key.k_uniqueness = TYPE_INDIRECT;
  }
  /* Delete object body. */
  while ( n_obj_size )  {
    /* Search for the last object item. */
    if ( search_for_position_by_key(p_s_sb, &s_item_key, &s_search_path, &n_pos_in_item, &n_repeat) == POSITION_NOT_FOUND ) {
      if (  n_is_last_item ) {
	struct item_head * p_s_ih;

	n_is_last_item = 0;
	p_s_ih = PATH_PITEM_HEAD(&s_search_path);

	if ( COMP_SHORT_KEYS(&s_item_key, &(p_s_ih->ih_key)) )
	  reiserfs_panic (p_s_sb, "PAP-5400: reiserfs_delete_object: item to delete doesn't exist");

	if ( I_IS_STAT_DATA_ITEM(p_s_ih) ) {
	  n_obj_size = 0;

#ifdef REISERFS_INFO
	  printk("reiserfs_delete_object: file size calculated by last file item(%lu) less than file size in inode(%lu)\n",
		 n_obj_size, p_s_inode->i_size);
#endif

	  break;
	}
	s_item_key.k_offset = n_obj_size = p_s_ih->ih_key.k_offset + I_BYTES_NUMBER(p_s_ih, p_s_sb->s_blocksize) - 1;
	n_pos_in_item--;

#ifdef REISERFS_INFO
	printk("reiserfs_delete_object: file size calculated by last file item(%lu) less than file size in inode(%lu)\n",
	       n_obj_size, p_s_inode->i_size);
#endif

      }
      else {
	reiserfs_panic (p_s_sb, "PAP-5410: reiserfs_delete_object: item %k to delete doesn't exist", &s_item_key);
      }

    }

    /* Delete last object item. */
    n_deleted = reiserfs_delete_item(th, p_s_inode, &s_search_path, &n_pos_in_item, &s_item_key, NULL, NOTHING_SPECIAL);

#ifdef REISERFS_CHECK
    if ( n_deleted <= 0 )
	    reiserfs_panic(p_s_sb, "reiser-5420: reiserfs_delete_object: this code needs to be fixed to handle ENOSPC");
    if ( n_deleted > n_obj_size )
	    reiserfs_panic (p_s_sb, "PAP-5430: reiserfs_delete_object: " 
                    "reiserfs_delete_item returns too big number");
#endif

    n_obj_size -= n_deleted;

#ifdef REISERFS_CHECK
    if ( n_obj_size && s_item_key.k_offset < n_deleted )
	    reiserfs_panic (p_s_sb, "PAP-5440: reiserfs_delete_object: illegal search key offset");
#endif
    /* Update key to search for the new last object item. */
    if ( (s_item_key.k_offset -= n_deleted) < p_s_inode->u.reiserfs_i.i_first_direct_byte )
      s_item_key.k_uniqueness = TYPE_INDIRECT;

    if (journal_transaction_should_end(th, th->t_blocks_allocated)) {
      int orig_len_alloc = th->t_blocks_allocated ;
      struct super_block *orig_super = th->t_super ;
      p_s_inode->i_size = n_obj_size ;
      p_s_inode->i_ctime = CURRENT_TIME ;
      p_s_inode->i_mtime = CURRENT_TIME ;
      decrement_counters_in_path(&s_search_path);
      if_in_ram_update_sd(th, p_s_inode) ;
      journal_end(th, orig_super, orig_len_alloc) ;
      journal_begin(th, orig_super, orig_len_alloc) ;
      reiserfs_update_inode_transaction(p_s_inode) ;
    }
  }

  /* Set key to search for the object stat_data. */  
  s_item_key.k_offset = SD_OFFSET;
  s_item_key.k_uniqueness = SD_UNIQUENESS;
  /* Search for the object stat_data. */
  if ( search_by_key(p_s_sb, &s_item_key, &s_search_path, &n_repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) == ITEM_NOT_FOUND ) {
    print_block (PATH_PLAST_BUFFER (&s_search_path), 0, -1, -1);
    reiserfs_panic (p_s_sb, "PAP-5450: reiserfs_delete_object: stat_data %k is not found", &s_item_key);
  }

  /* Delete object stat_data. */
  if ( reiserfs_delete_item(th, p_s_inode, &s_search_path, &n_pos_in_item, &s_item_key, NULL, NOTHING_SPECIAL) < 0 )
    reiserfs_panic (p_s_sb, "PAP: 5455: reiserfs_delete_object: reiserfs_delete_item: this code needs to be fixed");

#ifdef REISERFS_CHECK
  s_item_key.k_offset = MAX_KEY_OFFSET;
  s_item_key.k_uniqueness = MAX_KEY_UNIQUENESS;
  /* Try to find item of the deleted object. */
  if ( search_by_key (p_s_sb, &s_item_key, &s_search_path, &n_repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) == ITEM_FOUND )
    reiserfs_panic(p_s_sb,"PAP: 5460: reiserfs_delete_object: there is the item of deleted object");

  PATH_LAST_POSITION(&s_search_path)--;
  if (!COMP_SHORT_KEYS (&(PATH_PITEM_HEAD(&s_search_path)->ih_key), &s_item_key)) {
    print_block (PATH_PLAST_BUFFER (&s_search_path), PRINT_LEAF_ITEMS,
		 PATH_LAST_POSITION(&s_search_path) - 2, PATH_LAST_POSITION(&s_search_path) + 2);
    reiserfs_panic(p_s_sb,"PAP-5470: reiserfs_delete_object: there is the item %h of deleted object %k. Inode key %k",
		   PATH_PITEM_HEAD(&s_search_path), &s_item_key, INODE_PKEY (p_s_inode));
  }
  decrement_counters_in_path(&s_search_path);
#endif

  p_s_inode->i_size = 0;
}


/*********************** Inode part **************************************/
int increment_i_read_sync_counter(
      struct inode  * p_s_inode
    ) {
  int n_repeat = CARRY_ON;

#ifdef REISERFS_CHECK
  int n_repeat_counter = 0;
#endif

  /* Call schedule while this file is being converted. */
  while ( p_s_inode->u.reiserfs_i.i_is_being_converted )  {
#ifdef REISERFS_CHECK
    if (p_s_inode->u.reiserfs_i.i_read_sync_counter)
      reiserfs_panic (p_s_inode->i_sb, "PAP-5480: increment_i_read_sync_counter: file is read (synced) already");
    if ( !(++n_repeat_counter % 15000) )
      printk ("increment_i_read_sync_counter: (inode=%lu, pid=%d, counter=%d)\n", p_s_inode->i_ino, current->pid, n_repeat_counter);
#endif
    n_repeat |= SCHEDULE_OCCURRED;
    current->policy |= SCHED_YIELD;
    schedule();
  }

  p_s_inode->u.reiserfs_i.i_read_sync_counter++;
  return n_repeat;
}


void  decrement_i_read_sync_counter(
        struct inode  * p_s_inode
      ) {

#ifdef REISERFS_CHECK
  if ( ! p_s_inode->u.reiserfs_i.i_read_sync_counter )
    reiserfs_panic (p_s_inode->i_sb, "PAP-5490: increment_i_read_sync_counter: read_sync_counter is zero");
#endif

  p_s_inode->u.reiserfs_i.i_read_sync_counter--;
}


int lock_inode_to_convert(
      struct inode  * p_s_inode
    ) {
  int n_repeat = CARRY_ON;

#ifdef REISERFS_CHECK
  int n_repeat_counter = 0;
#endif

  /* Call schedule() while there is read from this file. */
  while ( p_s_inode->u.reiserfs_i.i_read_sync_counter ) {
#ifdef REISERFS_CHECK
    if (p_s_inode->u.reiserfs_i.i_is_being_converted)
      reiserfs_panic (p_s_inode->i_sb, "PAP-5495: lock_inode_to_convert: file is being truncated (or appended) already");
    if ( !(++n_repeat_counter % 15000) )
      printk ("lock_inode_to_convert: (inode=%lu, pid=%d, counter=%d)\n", p_s_inode->i_ino, current->pid, n_repeat_counter);
#endif
    n_repeat |= SCHEDULE_OCCURRED;
    current->policy |= SCHED_YIELD;
    schedule();
  }

#ifdef REISERFS_CHECK
  if ( p_s_inode->u.reiserfs_i.i_is_being_converted || p_s_inode->u.reiserfs_i.i_read_sync_counter )
    reiserfs_panic (p_s_inode->i_sb, "PAP-5500: lock_inode_to_convert: illegal case");
#endif

  /* Mark file as ready to convert. */
  p_s_inode->u.reiserfs_i.i_is_being_converted = 1;
  return n_repeat;
}


void  unlock_inode_after_convert(
        struct inode * p_s_inode
      ) {

#ifdef REISERFS_CHECK
  if ( p_s_inode->u.reiserfs_i.i_is_being_converted != 1 ||
                                        p_s_inode->u.reiserfs_i.i_read_sync_counter )
    reiserfs_panic (p_s_inode->i_sb, "PAP-5510: unlock_inode_after_convert: illegal case");
#endif

  /* Read is possible. */
  p_s_inode->u.reiserfs_i.i_is_being_converted = 0;
}

/************ End of the inode part ***************************/






/* Convert an unformatted node to a direct item. 
   Returns number of deleted bytes or -1 if io error encountered. */
static int indirect_to_direct(
      struct reiserfs_transaction_handle *th,
      struct inode        * p_s_inode,          /* Pointer to the file inode.                 */
      struct super_block  * p_s_sb,             /* Pointer to the super block.                */
      struct path         * p_s_path,           /* Pointer to the path to the indirect item.  */
      struct key          * p_s_item_key,       /* Key to search for the last file byte.      */
      unsigned long         n_new_file_size,    /* New file size.                             */
      char                * p_c_mode
    ) {
  struct buffer_head  * p_s_unfm_bh;              /* Pointer to the converted unformatted node
                                                    buffer.                                   */
  struct item_head      s_ih;
  unsigned long         n_unfm_number = 0; 	/* Unformatted node block number              */
  int                   n_pos_in_item,
                        n_repeat_or_retval, /* this variable is overloaded to be used for two purposes:
					       tracking whether schedule occured, and for use as
					       a temporary variable */
                        n_block_size = p_s_sb->s_blocksize;

  p_s_sb->u.reiserfs_sb.s_indirect2direct ++;
  /* Copy item at the path. */
  copy_item_head(&s_ih, PATH_PITEM_HEAD(p_s_path) );
  /* Don't read while we are converting the unformatted node. */
  if ( (n_repeat_or_retval = lock_inode_to_convert(p_s_inode)) )
    /* Check whether saved item is at the path. */
    n_repeat_or_retval = comp_items(&s_ih,p_s_path);
  if ( n_repeat_or_retval == CARRY_ON )
    /* Calculate last unformatted node number. */
    n_unfm_number = B_I_POS_UNFM_POINTER(PATH_PLAST_BUFFER(p_s_path), &s_ih, I_UNFM_NUM(&s_ih) - 1);

  /* We get the pointer to the unformatted to be converted into a direct item. */
  while ( 1 ) {
    if ( n_repeat_or_retval != CARRY_ON ) {
      /* Search for the indirect item. */
      if ( search_for_position_by_key(p_s_sb, p_s_item_key, p_s_path, &n_pos_in_item, &n_repeat_or_retval) == POSITION_NOT_FOUND )
	reiserfs_panic(p_s_sb, "PAP-5520: indirect_to_direct: item to convert does not exist");
      copy_item_head(&s_ih, PATH_PITEM_HEAD(p_s_path) );
      n_unfm_number = B_I_POS_UNFM_POINTER(PATH_PLAST_BUFFER(p_s_path), &s_ih, I_UNFM_NUM(&s_ih) - 1);
    }
    p_s_unfm_bh = NULL;
    if ( n_unfm_number )  {
      /* Read unformatted node to convert. */
      n_repeat_or_retval = CARRY_ON;
      p_s_unfm_bh = reiserfs_bread(p_s_sb->s_dev, n_unfm_number, p_s_sb->s_blocksize, &n_repeat_or_retval);
      if (!p_s_unfm_bh) {
	*p_c_mode = M_SKIP_BALANCING;
	pathrelse (p_s_path);
	return -1;
      }
      /* Current item was shifted from buffer at the path. */
      if ( n_repeat_or_retval != CARRY_ON && comp_items(&s_ih, p_s_path) )  {
        brelse(p_s_unfm_bh);
        continue;
      }

#if defined(REISERFS_CHECK) && !defined(PACKING_LOCALITY_READ_AHEAD)

      if ( p_s_unfm_bh->b_count != 1 && !buffer_journaled(p_s_unfm_bh)) {
        reiserfs_panic (p_s_sb, "PAP-5530: indirect_to_direct: (read counter %d, converted %d)"
			" converted block (%d) must not be in use (b_count==%d)",
			p_s_inode->u.reiserfs_i.i_read_sync_counter, 
			p_s_inode->u.reiserfs_i.i_is_being_converted, n_unfm_number, p_s_unfm_bh->b_count);
      }

#endif

    }
    break;
  }

  /* Set direct item header to insert. */
  s_ih.ih_key.k_offset += (I_UNFM_NUM (&s_ih) - 1) * n_block_size;
  n_pos_in_item = s_ih.ih_key.k_offset;	/*(s_ih.ih_key.k_offset -= (s_ih.ih_key.k_offset - 1) % n_block_size);*/
  s_ih.ih_key.k_uniqueness    = TYPE_DIRECT;
  s_ih.u.ih_free_space          = MAX_US_INT;
  n_repeat_or_retval = s_ih.ih_item_len = n_new_file_size % n_block_size;
  PATH_LAST_POSITION(p_s_path)++;

  /* Insert new direct item in the tree. This insert must mark nodes getting a new item as suspected recipient */
  /* Vladimir, LOOK journal **** was preserve indirect to direct */
  if ( reiserfs_insert_item(th, p_s_sb, p_s_path, &s_ih,
                      ( p_s_unfm_bh ) ? p_s_unfm_bh->b_data : NULL, REISERFS_KERNEL_MEM, 0/*zero bytes*/, NOTHING_SPECIAL) < 0 ) {
    /* No disk memory. So we can not convert last unformatted node to the direct item.
       In this case we mark that node has just 'n_new_file_size % n_block_size'
       bytes of the file.*/
    struct item_head * p_s_ih;

    if ( search_for_position_by_key(p_s_sb, p_s_item_key, p_s_path, &n_pos_in_item, &n_repeat_or_retval) == POSITION_NOT_FOUND )
      reiserfs_panic(p_s_sb, "PAP-5540: indirect_to_direct: item to convert does not exist");
    n_repeat_or_retval = (p_s_ih = PATH_PITEM_HEAD(p_s_path))->u.ih_free_space;
    p_s_ih->u.ih_free_space = n_block_size - n_new_file_size % n_block_size;

#ifdef REISERFS_CHECK
    if ( n_repeat_or_retval > p_s_ih->u.ih_free_space )
      reiserfs_panic (p_s_sb, "PAP-5550: indirect_to_direct: illegal new ih_free_space");
#endif

    n_repeat_or_retval = p_s_ih->u.ih_free_space - n_repeat_or_retval;
    *p_c_mode = M_SKIP_BALANCING;

    /* non-atomic mark_buffer_dirty is allowed here */
    /* mark_buffer_dirty(PATH_PLAST_BUFFER(p_s_path), 0); journal victim */
    journal_mark_dirty(th, p_s_sb, PATH_PLAST_BUFFER(p_s_path));
    unlock_inode_after_convert(p_s_inode);
    pathrelse(p_s_path);
  }
  else {
    /* We have inserted new direct item and must remove last unformatted node. */
    *p_c_mode = M_CUT;
    /* Set position of its first byte to inode (for read needs) */
    p_s_inode->u.reiserfs_i.i_first_direct_byte = n_pos_in_item;
    p_s_inode->i_blocks += p_s_sb->s_blocksize / 512;
  }

  brelse(p_s_unfm_bh);
  /* We have inserted new direct item and must remove last unformatted node. */
/*  *p_c_mode = M_CUT;*/
  return n_repeat_or_retval;
}



int maybe_indirect_to_direct(
      struct reiserfs_transaction_handle *th,
      struct inode        * p_s_inode,
      struct super_block  * p_s_sb,
      struct path         * p_s_path,
      struct key          * p_s_item_key,
      unsigned long         n_new_file_size,
      char                * p_c_mode
    ) {
  int n_block_size = p_s_sb->s_blocksize;
  int cut_bytes;

  /* We can store tail of the file in an unformatted node. */ 
  if ( dont_have_tails (p_s_sb) ||
       STORE_TAIL_IN_UNFM(n_new_file_size, n_new_file_size % n_block_size, n_block_size) ) { /* tail too long */
    /* Change ih->u.ih_free_space in the indirect item defined by path. */
    struct item_head  * p_s_ih = PATH_PITEM_HEAD(p_s_path);
    int                 n_old_free_space = p_s_ih->u.ih_free_space;

    *p_c_mode = M_SKIP_BALANCING;
    p_s_ih->u.ih_free_space = n_block_size - n_new_file_size % n_block_size;
    
#ifdef REISERFS_CHECK
    if ( n_old_free_space >= p_s_ih->u.ih_free_space )
      reiserfs_panic (p_s_sb, "PAP-5560: maybe_indirect_to_direct: tail is too small");
#endif

    cut_bytes = p_s_ih->u.ih_free_space - n_old_free_space;

    /* non-atomic mark_buffer_dirty is allowed here */
    /* mark_buffer_dirty(PATH_PLAST_BUFFER(p_s_path), 0); */
    journal_mark_dirty(th, p_s_sb, PATH_PLAST_BUFFER(p_s_path)) ;
    pathrelse(p_s_path);
    return cut_bytes;
  }
  /* Permorm the conversion to a direct_item. */
  return indirect_to_direct(th, p_s_inode, p_s_sb, p_s_path, p_s_item_key, n_new_file_size, p_c_mode);
}


/* we did indirect_to_direct conversion. And we have inserted direct
   item successesfully, but there were no disk space to cut unfm
   pointer being converted. Therefore we have to delete inserted
   direct item(s) */
static void indirect_to_direct_roll_back (struct reiserfs_transaction_handle *th, struct inode * inode, struct path * path)
{
  struct key tail_key;
  int tail_len;
  int pos_in_item;
  int repeat_or_removed;


  copy_key (&tail_key, INODE_PKEY (inode));
  tail_key.k_offset = inode->i_size + 1;
  tail_key.k_uniqueness = TYPE_DIRECT;
  tail_len = tail_key.k_offset % inode->i_sb->s_blocksize - 1;
  while (tail_len) {
    /* look for the last byte of the tail */
    if (search_for_position_by_key (inode->i_sb, &tail_key, path, &pos_in_item, &repeat_or_removed) == POSITION_NOT_FOUND)
      reiserfs_panic (inode->i_sb, "vs-5615: indirect_to_direct_roll_back: found invalid item");
#ifdef REISERFS_CHECK
    if (pos_in_item != PATH_PITEM_HEAD (path)->ih_item_len - 1)
      reiserfs_panic (inode->i_sb, "vs-5616: indirect_to_direct_roll_back: appended bytes found");
#endif
    PATH_LAST_POSITION (path) --;
	
    repeat_or_removed = reiserfs_delete_item (th, inode, path, &pos_in_item, &tail_key, 0, NOTHING_SPECIAL);
#ifdef REISERFS_CHECK
    if (repeat_or_removed <= 0 || repeat_or_removed > tail_len)
      reiserfs_panic (inode->i_sb, "vs-5617: indirect_to_direct_roll_back: "
		      "there was tail %d bytes, removed item length %d bytes",
		      tail_len, repeat_or_removed);
#endif
    tail_len -= repeat_or_removed;
    tail_key.k_offset -= repeat_or_removed;
  }
  printk ("indirect_to_direct_roll_back: indirect_to_direct conversion has been rolled back due to lack of disk space\n");
  inode->u.reiserfs_i.i_first_direct_byte = NO_BYTES_IN_DIRECT_ITEM;
  mark_inode_dirty (inode);
}


/* (Truncate or cut entry) or delete object item. */
int reiserfs_cut_from_item(
      struct reiserfs_transaction_handle *th,
      struct inode        * p_s_inode,
      struct super_block  * p_s_sb,
      struct path         * p_s_path,
      int                 * p_n_pos_in_item,
      struct key          * p_s_item_key,
      unsigned long         n_new_file_size,
      int		    preserve_mode	/* can be PRESERVE_RENAMING or NOTHING SPECIAL */
    ) {
  /* Every function which is going to call do_balance must first
     create a tree_balance structure.  Then it must fill up this
     structure by using the init_tb_struct and fix_nodes functions.
     After that we can make tree balancing. */
  struct tree_balance s_cut_balance;
  int                 n_repeat,
                      n_cut_size,        /* Amount to be cut. */
                      /* n_ret_value = CARRY_ON, */
		      n_fix_ret_value = CARRY_ON, /* return value from fix_nodes and do_balance */
		      n_count_ret_value = 0,      /* return value from indirect->direct */
                      n_removed = 0,     /* Number of the removed unformatted nodes. */
  		      n_is_inode_locked = 0;
  char                c_mode;            /* Mode of the balance. */
  int was_unfm_suspected_recipient = 0;

  init_tb_struct(&s_cut_balance, p_s_sb, p_s_path, n_cut_size);
  s_cut_balance.preserve_mode = preserve_mode;

  /* Repeat this loop until we either cut the item without needing to balance, or we fix_nodes without
     schedule occuring */
  while ( 1 ) {
      /* Determine the balance mode, position of the first byte to be cut, and size to be cut.
	 In case of the indirect item free unformatted nodes which are pointed to by
	 the cut pointers. */

    /* Vladimir, LOOK, journal **** first nothing special was preserving indirect to direct */
    c_mode = prepare_for_delete_or_cut(th, p_s_inode, p_s_path, p_s_item_key, p_n_pos_in_item, &n_removed, &n_cut_size, 
    				       n_new_file_size,
				       n_is_inode_locked ? NOTHING_SPECIAL : NOTHING_SPECIAL, &was_unfm_suspected_recipient);
    if ( c_mode == M_CONVERT )  {
	/* convert last unformatted node to direct item or adjust its ih_free_space */
#ifdef REISERFS_CHECK
      if ( n_fix_ret_value != CARRY_ON )
        reiserfs_panic (p_s_sb, "PAP-5570: reiserfs_cut_from_item: can not convert twice");
#endif

      n_count_ret_value = maybe_indirect_to_direct (th, p_s_inode, p_s_sb, p_s_path, p_s_item_key,
					      n_new_file_size, &c_mode);
      if (n_count_ret_value == -1)
	return 0;
      /* We have cut all item bytes and must stop. */
      if ( c_mode == M_SKIP_BALANCING )
        break;
      n_is_inode_locked = 1;
      /* So, we have performed the first part of the conversion:
	 inserting the new direct item.  Now we are removing the last
	 unformatted node pointer. Set key to search for it. */
      p_s_item_key->k_uniqueness = TYPE_INDIRECT;
      n_new_file_size -= n_new_file_size % p_s_sb->s_blocksize;
      p_s_item_key->k_offset = n_new_file_size + 1;
      if ( search_for_position_by_key(p_s_sb, p_s_item_key, p_s_path, p_n_pos_in_item, &n_repeat) == POSITION_NOT_FOUND ){
	print_block (PATH_PLAST_BUFFER (p_s_path), 3, PATH_LAST_POSITION (p_s_path) - 1, PATH_LAST_POSITION (p_s_path) + 1);
	reiserfs_panic(p_s_sb, "PAP-5580: reiserfs_cut_from_item: item to convert does not exist (%k)", p_s_item_key);
      }
      continue;
    }

    s_cut_balance.insert_size[0] = n_cut_size;

    n_fix_ret_value = fix_nodes(th, c_mode, &s_cut_balance, *p_n_pos_in_item, NULL);
 
#ifdef REISERFS_CHECK_ONE_PROCESS
    if ( n_fix_ret_value == PATH_INCORRECT )
      reiserfs_panic(p_s_sb, "PAP-5600: reiserfs_cut_from_item: "
		     "illegal returned value");
#endif

    if ( n_fix_ret_value != SCHEDULE_OCCURRED && n_fix_ret_value != PATH_INCORRECT )
      break;

    /* else schedule() occured while fix_nodes() worked */
    if ( search_for_position_by_key(p_s_sb, p_s_item_key, p_s_path, p_n_pos_in_item, &n_repeat) == POSITION_NOT_FOUND )
      reiserfs_panic(p_s_sb, "PAP-5610: reiserfs_cut_from_item: item to delete does not exist");
  } /* while */

  if ( n_fix_ret_value == NO_DISK_SPACE || n_fix_ret_value == IO_ERROR || n_count_ret_value == NO_DISK_SPACE) {
    if ( n_is_inode_locked ) {
      indirect_to_direct_roll_back(th, p_s_inode, p_s_path);
    }
    unfix_nodes (th, &s_cut_balance);
    return 0;
  }

  if ( c_mode != M_SKIP_BALANCING ) {
    journal_lock_dobalance(p_s_sb) ;

#ifdef REISERFS_CHECK
/*    if ( n_ret_value >= calc_deleted_bytes_number(&s_cut_balance, c_mode) )
      reiserfs_panic (p_s_sb, "PAP-5630: reiserfs_cut_from_item: returned value is too big");*/
    if (n_fix_ret_value != CARRY_ON)
      reiserfs_panic (p_s_sb, "PAP-5630: ret_value is other than CARRY_ON");
    if ( c_mode == M_PASTE || c_mode == M_INSERT )
      reiserfs_panic (p_s_sb, "PAP-5640: reiserfs_cut_from_item: illegal mode");
#endif
      /* Calculate number of bytes that need to be cut from the item.  how could this have been right? */
      /* it was n_ret_value = calc - n_ret_value.  We know from above that n_ret_value was CARRY_ON
      ** or we would be reiserfs_panic'ing.  So in the error case (with reiserfs_check off), we were subtracting some number
      ** of bytes, for no apparent reason.
      */
    n_count_ret_value = calc_deleted_bytes_number(&s_cut_balance, c_mode) ;

    if ( c_mode == M_DELETE ) {
      struct item_head * p_s_ih = B_N_PITEM_HEAD(PATH_PLAST_BUFFER(s_cut_balance.tb_path), PATH_LAST_POSITION(s_cut_balance.tb_path));

      if ( I_IS_DIRECT_ITEM(p_s_ih) && p_s_ih->ih_key.k_offset % p_s_sb->s_blocksize == 1 ) {

#ifdef REISERFS_CHECK
	if ( p_s_inode->u.reiserfs_i.i_first_direct_byte != p_s_ih->ih_key.k_offset )
	  reiserfs_panic (p_s_sb, "PAP-5650: reiserfs_cut_from_item: illegal first direct byte position");
#endif

	p_s_inode->u.reiserfs_i.i_first_direct_byte = NO_BYTES_IN_DIRECT_ITEM;
	p_s_inode->i_blocks -= p_s_sb->s_blocksize / 512;
      }
    }

    if (n_is_inode_locked) {
      /* we are going to cut last unfm ptr, preserve it first. unfm node block number is on preserve list already */
#ifdef REISERFS_CHECK
      if (!I_IS_INDIRECT_ITEM (PATH_PITEM_HEAD (s_cut_balance.tb_path)))
	reiserfs_panic (p_s_sb, "vs-5652: reiserfs_cut_from_item: item must be indirect %h", PATH_PITEM_HEAD (s_cut_balance.tb_path));
      if (c_mode == M_DELETE && -(PATH_PITEM_HEAD (s_cut_balance.tb_path)->ih_item_len + IH_SIZE) != s_cut_balance.insert_size[0]) {
	reiserfs_panic (p_s_sb, "vs-5653: reiserfs_cut_from_item: "
			"can not complete indirect_to_direct conversion of %h (DELETE, insert_size==%d)",
			PATH_PITEM_HEAD (s_cut_balance.tb_path), s_cut_balance.insert_size[0]);
      }
      if (c_mode == M_CUT && s_cut_balance.insert_size[0] != -UNFM_P_SIZE) {
	reiserfs_panic (p_s_sb, "vs-5654: reiserfs_cut_from_item: can not complete indirect_to_direct conversion of %h (CUT, insert_size==%d)",
			PATH_PITEM_HEAD (s_cut_balance.tb_path), s_cut_balance.insert_size[0]);
      }
#endif
      /* we should not preserve indirect item if unformatted node was marked as suspected recipient */
/*
      if (!was_unfm_suspected_recipient)
	preserve_indirect_item (&s_cut_balance);
*/
    }
/*
    if (preserve_mode == PRESERVE_RENAMING)
      preserve_entry (&s_cut_balance);
*/
    do_balance(th, &s_cut_balance, *p_n_pos_in_item, NULL, NULL, c_mode, REISERFS_KERNEL_MEM, 0/* zero number */);
    journal_unlock_dobalance(p_s_sb) ;
    if ( n_is_inode_locked )
      unlock_inode_after_convert(p_s_inode);
  } /* ! SKIP_BALANCING */

  return n_count_ret_value;
}

int reiserfs_file_release(struct inode *p_s_inode, struct file *p_s_filp) {
  struct path           s_search_path;  /* Path to the current object item. */
  struct item_head    * p_s_ih;         /* Pointer to an item header. */
  struct key            s_item_key;     /* Key to search for a previous file item. */
  unsigned long         n_file_size,    /* Old file size. */
                        n_new_file_size;/* New file size. */
  int                   n_deleted,      /* Number of deleted or truncated bytes. */
                        n_pos_in_item,  /* Found position in an item. */
  			n_repeat;   
  int windex ;
  struct reiserfs_transaction_handle th ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3; 

  if ( ! (S_ISREG(p_s_inode->i_mode) || S_ISDIR(p_s_inode->i_mode) || S_ISLNK(p_s_inode->i_mode)) )
    return 0;

  /* only pack when reiserfs_file_write tells us to by setting i_pack_on_close */
  if (!p_s_inode->u.reiserfs_i.i_pack_on_close || dont_have_tails (p_s_inode->i_sb)) {
    return 0 ;
  }
  p_s_inode->u.reiserfs_i.i_pack_on_close = 0 ;
  journal_begin(&th, p_s_inode->i_sb, jbegin_count) ;
  windex = push_journal_writer("reiserfs_release_inode") ;
  reiserfs_update_inode_transaction(p_s_inode) ;
  init_path (&s_search_path);

  /* Copy key of the first object item. */
  copy_key(&s_item_key, INODE_PKEY(p_s_inode));

  /* New file size is the same as the original.  We are calling cut_from_item so it will do all the
  ** indirect->direct work for us
  */
  n_new_file_size = p_s_inode->i_size;

  /* Form key to search for the last file item. */
  s_item_key.k_offset = MAX_KEY_OFFSET; /* pasted from truncate, I should be able to put the file size here right? */
  if ( p_s_inode->u.reiserfs_i.i_first_direct_byte != NO_BYTES_IN_DIRECT_ITEM ) {
    /* already packed, we're done */
    pop_journal_writer(windex) ;
    journal_end(&th, p_s_inode->i_sb, jbegin_count) ;
    return 0 ;
  } else {
    s_item_key.k_uniqueness = TYPE_INDIRECT;
  }
  if ( search_for_position_by_key(p_s_inode->i_sb, &s_item_key, &s_search_path, &n_pos_in_item, &n_repeat) == POSITION_FOUND )
    reiserfs_panic (p_s_inode->i_sb, "PAP-5660: reiserfs_file_release: "
		      "object item has too big offset");

  /* pasted from truncate.  Why am I doing this? */
  n_pos_in_item--;

  /* Calculate old size of the file. Pasted from truncate.  I'm keeping this incase the inode is wrong some how,
  ** but I really don't need it
  */
  p_s_ih = PATH_PITEM_HEAD(&s_search_path);
  if ( I_IS_STAT_DATA_ITEM(p_s_ih) )
    n_file_size = 0;
  else
    n_file_size = p_s_ih->ih_key.k_offset + I_BYTES_NUMBER(p_s_ih,p_s_inode->i_sb->s_blocksize) - 1;

  if ( n_file_size == 0 || n_file_size != n_new_file_size ||
       (STORE_TAIL_IN_UNFM( I_BYTES_NUMBER(p_s_ih,p_s_inode->i_sb->s_blocksize), n_file_size, p_s_inode->i_sb->s_blocksize))
  ) {
    pathrelse(&s_search_path);
    pop_journal_writer(windex) ;
    journal_end(&th, p_s_inode->i_sb, jbegin_count) ;
    return 0;
  }

  /* Update key to search for the last file item. */
  s_item_key.k_offset = n_file_size;

  /* Cut or delete file item. */
  n_deleted = reiserfs_cut_from_item(&th, p_s_inode, p_s_inode->i_sb, &s_search_path, &n_pos_in_item, &s_item_key, n_new_file_size, 
                                     NOTHING_SPECIAL);

#ifdef REISERFS_CHECK
  if ( n_deleted > n_file_size ){
    reiserfs_panic (p_s_inode->i_sb, "PAP-5670: reiserfs_file_release: "
		    "reiserfs_file_release returns too big number: deleted %d, file_size %lu, item_key %k",
		    n_deleted, n_file_size, &s_item_key);
  }
#endif

#ifdef REISERFS_CHECK
  if ( n_file_size > n_new_file_size )
    reiserfs_panic (p_s_inode->i_sb, "PAP-5680: reiserfs_file_release: object item did not find");
#endif

  /* note, FS corruption after crash if I don't update the stat data.  Why? */
  p_s_inode->i_mtime = p_s_inode->i_ctime = CURRENT_TIME;
  if_in_ram_update_sd (&th, p_s_inode);
  pop_journal_writer(windex) ;
  journal_end(&th, p_s_inode->i_sb, jbegin_count) ;
  return 0 ;
}

/* Truncate file to the new size. */
void  reiserfs_truncate_file(
        struct  inode * p_s_inode       /* Pointer to the file inode. New size
                                            already marked in the inode. */
      ) {
  struct path           s_search_path;  /* Path to the current object item. */
  struct item_head    * p_s_ih;         /* Pointer to an item header. */
  struct key            s_item_key;     /* Key to search for a previous file item. */
  unsigned long         n_file_size,    /* Old file size. */
                        n_new_file_size;/* New file size. */
  int                   n_deleted,      /* Number of deleted or truncated bytes. */
                        n_pos_in_item,  /* Found position in an item. */
  			n_repeat;   
  int windex ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3 ;
  struct reiserfs_transaction_handle th ;

  if ( ! (S_ISREG(p_s_inode->i_mode) || S_ISDIR(p_s_inode->i_mode) || S_ISLNK(p_s_inode->i_mode)) )
    return;

  journal_begin(&th, p_s_inode->i_sb, jbegin_count) ;
  windex = push_journal_writer("reiserfs_truncate_file") ;
  init_path (&s_search_path);
  reiserfs_update_inode_transaction(p_s_inode) ;

  /* Copy key of the first object item. */
  copy_key(&s_item_key, INODE_PKEY(p_s_inode));

  /* Get new file size. */
  n_new_file_size = p_s_inode->i_size;

  /* Form key to search for the last file item. */
  s_item_key.k_offset = MAX_KEY_OFFSET; /* We don't know old size of the file. */
  if ( p_s_inode->u.reiserfs_i.i_first_direct_byte != NO_BYTES_IN_DIRECT_ITEM )
    s_item_key.k_uniqueness = TYPE_DIRECT;
  else
    s_item_key.k_uniqueness = TYPE_INDIRECT;

  if ( search_for_position_by_key(p_s_inode->i_sb, &s_item_key, &s_search_path, &n_pos_in_item, &n_repeat) == POSITION_FOUND )
    reiserfs_panic (p_s_inode->i_sb, "PAP-5660: reiserfs_truncate_file: "
		      "object item has too big offset");
  n_pos_in_item--;

  /* Calculate old size of the file. */
  p_s_ih = PATH_PITEM_HEAD(&s_search_path);
  if ( I_IS_STAT_DATA_ITEM(p_s_ih) )
    n_file_size = 0;
  else
    n_file_size = p_s_ih->ih_key.k_offset + I_BYTES_NUMBER(p_s_ih,p_s_inode->i_sb->s_blocksize) - 1;

  if ( n_file_size == 0 || n_file_size <= n_new_file_size ) {
#ifdef REISERFS_INFO
    printk ("reiserfs_truncate_file: old file size = %lu < new file size = %lu\n", n_file_size, n_new_file_size);
#endif
    pathrelse(&s_search_path);
    pop_journal_writer(windex) ;
    journal_end(&th, p_s_inode->i_sb, jbegin_count) ;
    return;
  }
  


  /* Update key to search for the last file item. */
  s_item_key.k_offset = n_file_size;

  do  {
    /* Cut or delete file item. */
    n_deleted = reiserfs_cut_from_item(&th, p_s_inode, p_s_inode->i_sb, &s_search_path, &n_pos_in_item, 
                                       &s_item_key, n_new_file_size, NOTHING_SPECIAL);

#ifdef REISERFS_CHECK
    if ( n_deleted > n_file_size ){
      reiserfs_panic (p_s_inode->i_sb, "PAP-5670: reiserfs_truncate_file: "
		      "reiserfs_truncate_file returns too big number: deleted %d, file_size %lu, item_key %k",
		      n_deleted, n_file_size, &s_item_key);
    }
#endif

    /* Change key to search the last file item. */
    if ( (s_item_key.k_offset = (n_file_size -= n_deleted)) < p_s_inode->u.reiserfs_i.i_first_direct_byte )
      s_item_key.k_uniqueness = TYPE_INDIRECT;

    if (journal_transaction_should_end(&th, th.t_blocks_allocated)) {
      int orig_len_alloc = th.t_blocks_allocated ;
      p_s_inode->i_ctime = CURRENT_TIME ;
      p_s_inode->i_mtime = CURRENT_TIME ;
      decrement_counters_in_path(&s_search_path);
      if_in_ram_update_sd(&th, p_s_inode) ;
      journal_end(&th, p_s_inode->i_sb, orig_len_alloc) ;
      journal_begin(&th, p_s_inode->i_sb, orig_len_alloc) ;
      reiserfs_update_inode_transaction(p_s_inode) ;
    }

    /* While there are bytes to truncate and previous file item is presented in the tree. */
  } while ( n_file_size > n_new_file_size &&
	    search_for_position_by_key(p_s_inode->i_sb, &s_item_key, &s_search_path, &n_pos_in_item, &n_repeat) == POSITION_FOUND )  ;

#ifdef REISERFS_CHECK
  if ( n_file_size > n_new_file_size )
    reiserfs_panic (p_s_inode->i_sb, "PAP-5680: reiserfs_truncate_file: object item did not find");
#endif

  p_s_inode->i_mtime = p_s_inode->i_ctime = CURRENT_TIME;
  if_in_ram_update_sd (&th, p_s_inode);
  pop_journal_writer(windex) ;
  journal_end(&th, p_s_inode->i_sb, jbegin_count) ;
}


/* Paste bytes to the existing item. Returns bytes number pasted into the item. */
int reiserfs_paste_into_item(
	struct reiserfs_transaction_handle *th,
	struct super_block  * p_s_sb,	   	/* Pointer to the supoer block.	*/
	struct path         * p_s_search_path,	/* Path to the pasted item.          */
	int                 * p_n_pos_in_item,	/* Paste position in the item above. */
	struct key          * p_s_key,        	/* Key to search for the needed item.*/
	const char          * p_c_body,       	/* Pointer to the bytes to paste.    */
	int                   n_pasted_size,  	/* Size of pasted bytes.             */
	int                   n_mem_mode,     	/* Copy from KERNEL or USER buffer.  */
	int		      n_zeros_num	/* Number of zeros to be pasted.     */
	) {
    struct tree_balance s_paste_balance;
    int                 n_fix_nodes_res,
      			n_repeat;

    if ( n_pasted_size < 0 )
      reiserfs_panic(p_s_sb, "PAP-5690: reiserfs_paste_into_item: illegal pasted size");

    init_tb_struct(&s_paste_balance, p_s_sb, p_s_search_path, n_pasted_size);
    s_paste_balance.preserve_mode = NOTHING_SPECIAL;
    while ( (n_fix_nodes_res = fix_nodes(th, M_PASTE, &s_paste_balance, *p_n_pos_in_item, NULL)) == SCHEDULE_OCCURRED ||
	    n_fix_nodes_res == PATH_INCORRECT )  {

#ifdef REISERFS_CHECK_ONE_PROCESS
      if ( n_fix_nodes_res == PATH_INCORRECT )
	reiserfs_panic(p_s_sb, "PAP-5700: reiserfs_paste_into_item: illegal returned value");
#endif

      /* schedule() occurred while fix_balance() worked */
      if ( search_for_position_by_key (p_s_sb, p_s_key, p_s_search_path, p_n_pos_in_item, &n_repeat) == POSITION_FOUND ) {
	reiserfs_panic (p_s_sb, "PAP-5710: reiserfs_paste_into_item: entry or pasted byte (%k) exists", p_s_key);
      }
#ifdef REISERFS_CHECK
      {
	struct item_head * found_ih = B_N_PITEM_HEAD (PATH_PLAST_BUFFER (p_s_search_path),
						      PATH_LAST_POSITION (p_s_search_path));

	if (I_IS_DIRECT_ITEM (found_ih)) {
	  if (found_ih->ih_key.k_offset + I_BYTES_NUMBER (found_ih, p_s_sb->s_blocksize) !=
	      p_s_key->k_offset ||
	      I_BYTES_NUMBER (found_ih, p_s_sb->s_blocksize) != *p_n_pos_in_item)
	    reiserfs_panic (p_s_sb, "PAP-5720: reiserfs_paste_into_item: found direct item (offset=%lu, length=%d) or position (%d) does not match to key (offset=%lu)",
			    found_ih->ih_key.k_offset, found_ih->ih_item_len, *p_n_pos_in_item, p_s_key->k_offset);
	}
	if (I_IS_INDIRECT_ITEM (found_ih)) {
	  if (found_ih->ih_key.k_offset + I_BYTES_NUMBER (found_ih, p_s_sb->s_blocksize) != p_s_key->k_offset || 
	      I_UNFM_NUM (found_ih) != *p_n_pos_in_item ||
	      found_ih->u.ih_free_space != 0)
	    reiserfs_panic (p_s_sb, "PAP-5730: reiserfs_paste_into_item: "
			  "found indirect item (offset=%lu, unfm pointers=%d, free_space=%d) or position (%d) does not match to key (%lu)",
			    found_ih->ih_key.k_offset, I_UNFM_NUM (found_ih), found_ih->u.ih_free_space,
			    *p_n_pos_in_item, p_s_key->k_offset);
	}
      }
#endif
    }

    /* Perform balancing after all resources are collected by fix_nodes, and accessing
      them will not risk triggering schedule. */
    if ( n_fix_nodes_res == CARRY_ON ) {
      journal_lock_dobalance(p_s_sb) ;

      if ( s_paste_balance.insert_size[0] < 0 )
	reiserfs_panic (p_s_sb, "PAP-5740: reiserfs_paste_into_item: insert_size = %d\n",s_paste_balance.insert_size[0]);

      do_balance(th, &s_paste_balance, *p_n_pos_in_item, NULL, p_c_body, M_PASTE, n_mem_mode, n_zeros_num);
      journal_unlock_dobalance(p_s_sb) ;
      return (n_pasted_size);
    }
    unfix_nodes(th, &s_paste_balance);
    return NO_DISK_SPACE; /* No disk space or io error. */
}


/* Insert new item into the buffer at the path. */
int reiserfs_insert_item(
			 struct reiserfs_transaction_handle *th,
			 struct super_block  * 	p_s_sb,           /* Pointer to the super block.          */
			 struct path         * 	p_s_path,         /* Path to the inserteded item.         */
			 struct item_head    * 	p_s_ih,           /* Pointer to the item header to insert.*/
			 const char          * 	p_c_body,         /* Pointer to the bytes to insert.      */
			 int                   	n_mem_mode,       /* Copy from KERNEL or USER buffer.     */
			 int			n_zeros_num,
			 int			preserve_mode	  /* can be
								     PRESERVE_INDIRECT_TO_DIRECT or
								     NOTHING_SPECIAL. if
								     PRESERVE_INDIRECT_TO_DIRECT,
								     mark buffers new item gets into
								     as suspected recipients */
			 ) {
    struct tree_balance s_ins_balance;
    int                 n_fix_nodes_res,
      			n_repeat;


    init_tb_struct(&s_ins_balance, p_s_sb, p_s_path, IH_SIZE + p_s_ih->ih_item_len);
    s_ins_balance.preserve_mode = preserve_mode;

    p_s_ih->ih_reserved = 0;
    if (p_c_body == 0)
      n_zeros_num = p_s_ih->ih_item_len;


    while ( (n_fix_nodes_res = fix_nodes(th, M_INSERT, &s_ins_balance, 0, p_s_ih)) == SCHEDULE_OCCURRED ||
	    n_fix_nodes_res == PATH_INCORRECT ) {


#ifdef REISERFS_CHECK_ONE_PROCESS
      if ( n_fix_nodes_res == PATH_INCORRECT )
	reiserfs_panic(p_s_sb, "PAP-5750: reiserfs_insert_item: illegal returned value");
#endif

      /* schedule occurred while fix_nodes() worked */
      if ( search_by_key(p_s_sb, &(p_s_ih->ih_key), p_s_path, &n_repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) == ITEM_FOUND )
	reiserfs_panic (p_s_sb, "PAP-5760: reiserfs_insert_item: inserted item exists (%k)", &(p_s_ih->ih_key));
    }
    /* make balancing after all resources will be collected at a time */ 
    if ( n_fix_nodes_res == CARRY_ON ) {
      journal_lock_dobalance(p_s_sb) ;
      do_balance (th, &s_ins_balance, 0, p_s_ih, p_c_body, M_INSERT, n_mem_mode, n_zeros_num);
      journal_unlock_dobalance(p_s_sb) ;
      return p_s_ih->ih_item_len;
    }

    unfix_nodes(th, &s_ins_balance);
    return NO_DISK_SPACE; /* No disk space or io error */
}





/*********************** range_read code ***************************************/

/* It is interesting to consider why this code is so
   complicated.... It seems like it ought to be simpler.*/


/*  Get data buffer from cache which contains data (byte or directory record) of some object.
    This data has minimal possible key >= than *p_range_begin and <= than *p_range_end.
    In other words get first buffer contains data with key from range
    [*p_key,*p_range_end].
    If it is not possible (needed buffer is not in the cache) prepare (not uptodate) buffer
    at path from root to the needed buffer. Don't read any blocks.
    Returns:    1) via return value     0 if there is not an needed buffer, 1 otherwise;

                2) via pp_s_buf:        NULL if the needed buffer is not in memory, 
                                        or pointer to the needed buffer, or pointer to the prepared buffer;

                3) via p_n_objectid:    Corresponding object id if pointer above points to an unformatted node, MAX_KEY_OBJECTID otherwise.

                4) Recalculated head of the range in p_s_range_head */

int get_buffer_by_range(
      struct super_block      *	p_s_sb,			/* Super block.								*/
      struct key              *	p_s_range_head,		/* Range begin.								*/
      struct key              *	p_s_range_end,  	/* Range end.								*/
      struct buffer_head     **	pp_s_buf,        	/* Returned value; result buffer.					*/
      unsigned long	      *	p_n_objectid		/* Returned value; corresponding object id if *pp_s_buf points to an
							   unformatted node, MAX_KEY_OBJECTID in other cases.			*/
) {			

  int                   n_res,
    			n_pos_in_buffer,
    			n_repeat,
   			n_item_num,
  			n_pos_in_item;
  struct path		s_path;
  struct buffer_head  * p_s_bh;             	/* current buffer                       */
  struct key	        s_min_key,
  		      *	p_s_rkey;
  struct item_head    * p_s_ih,
			s_ih;
  unsigned long		n_unfm_pointer;

#ifdef REISERFS_CHECK
  int			n_repeat_counter = 0;
#endif

  init_path (&s_path);

repeat:

#ifdef REISERFS_CHECK
  if ( ! (++n_repeat_counter % 10000) ) {
    reiserfs_panic(p_s_sb, "PAP-5765: get_buffer_by_range: counter(%d) too big. range_head %k", n_repeat_counter, p_s_range_head);
  }
#endif


  /* Search for the needed buffer in the range. */
  n_res = search_by_key(p_s_sb, p_s_range_head, &s_path, &n_repeat, DISK_LEAF_NODE_LEVEL, DONT_READ_BLOCKS);

  n_pos_in_buffer = PATH_LAST_POSITION(&s_path);

#ifdef REISERFS_CHECK
  if ( ! key_in_buffer (&s_path, p_s_range_head, p_s_sb) )
    reiserfs_panic(p_s_sb, " PAP: 5770: get_buffer_by_range: key is not in the buffer");
#endif

  if ( ! buffer_uptodate(p_s_bh = PATH_PLAST_BUFFER(&s_path)) ) {
    /* We can not get data buffer from range. Prepare buffer from the cache and recalculate right delimiting key. */
    p_s_rkey = get_rkey(&s_path, p_s_sb);

    if ( ! COMP_KEYS(p_s_rkey, &MIN_KEY) ) {

#ifdef REISERFS_CHECK_ONE_PROCESS
      reiserfs_panic(p_s_sb, "PAP-5780: get_buffer_by_range: can not get right delimiting key in case of one process");
#endif

#ifdef REISERFS_CHECK
      if ( n_repeat == CARRY_ON )
	reiserfs_panic(p_s_sb, "PAP-5790: get_buffer_by_range: get_rkey returns KEY_MIN");
#endif

      goto repeat;  /* We can not recalculate right delimiting key to continue search for the buffers in the range.
		       Do that by old one. */
    }
    *pp_s_buf = p_s_bh;


#ifdef REISERFS_CHECK
    if ( ! key_in_buffer(&s_path, p_s_range_head, p_s_sb) )
      reiserfs_panic(p_s_sb, "PAP-5791: get_buffer_by_range: key is not in the path");
    if ( s_path.path_length < FIRST_PATH_ELEMENT_OFFSET )
      reiserfs_panic(p_s_sb, "PAP-5792: get_buffer_by_range: path length is too small");
#endif
    
    copy_key(p_s_range_head, p_s_rkey);
    
    s_path.path_length--;
    decrement_counters_in_path(&s_path);
    *p_n_objectid = MAX_KEY_OBJECTID;
    return 1;
  }

  /* last buffer on the path is uptodate */

#ifdef REISERFS_CHECK
  if ( COMP_KEYS(B_N_PKEY(p_s_bh, 0), p_s_range_head) == 1 || COMP_KEYS(B_PRIGHT_DELIM_KEY(p_s_bh), p_s_range_head ) < 1 ) {
    reiserfs_panic(p_s_sb, "PAP-5795: range head key %k we looked for is not in the buffer", p_s_range_head);
  }
  if ( ! B_IS_ITEMS_LEVEL(p_s_bh) || (n_res == ITEM_NOT_FOUND && ! n_pos_in_buffer) )
    reiserfs_panic(p_s_sb, "PAP-5800: get_buffer_by_range: last buffer on the path is not leaf or returned position is 0");
#endif

  p_s_ih = B_N_PITEM_HEAD(p_s_bh, n_pos_in_buffer);
  n_item_num = B_NR_ITEMS(p_s_bh);
  /* Now we are defining the data which are placed in buffer p_s_bh and has minimal key
     more than or equal to *p_range_begin. */
  if ( n_res == ITEM_FOUND ) { /* Item was found in the tree. */
    n_pos_in_item = 0;
    copy_key(&s_min_key,&(p_s_ih->ih_key));
  }

  else {  /* Item was not found in the tree. */
    /* Calculate min_key which is the minimal key of the byte or directory entry
       more or equal than p_range_begin. */
    n_pos_in_item = MAX_INT;
    /*  Previous item is item of the same object we are looking for. */
    if ( ! COMP_SHORT_KEYS(p_s_range_head, &((p_s_ih - 1)->ih_key)) ) {
      if ( I_IS_DIRECTORY_ITEM(p_s_ih - 1) ) {
	/* Search in the directory item for the entry that has minimal key more or equal than *p_s_range_head. */
	bin_search_in_dir_item (p_s_ih - 1, B_I_DEH(p_s_bh, p_s_ih - 1), p_s_range_head, &n_pos_in_item);
	if ( n_pos_in_item < I_ENTRY_COUNT(p_s_ih - 1) ) {
	/* Previous item contains needed directory entry. */
	  p_s_ih--;
	  PATH_LAST_POSITION(&s_path)--;
	  n_pos_in_buffer--;
	  copy_key(&s_min_key, &p_s_ih->ih_key);
	  s_min_key.k_offset = B_I_DEH(p_s_bh, p_s_ih)[n_pos_in_item].deh_offset;
	  s_min_key.k_uniqueness = DIRENTRY_UNIQUENESS;
	}
	else
	  /* key *p_s_range_head is greater than last entry in directory item */
	  n_pos_in_item = MAX_INT;
      }
      else {
	/* key *p_s_range_head is key of regular file */
	if ( I_K_KEY_IN_ITEM(p_s_ih - 1, p_s_range_head, p_s_bh->b_size) ) {
	  /* Previous item contains needed byte. */
	  p_s_ih--;
	  PATH_LAST_POSITION(&s_path)--;
	  n_pos_in_buffer--;
	  copy_key(&s_min_key, p_s_range_head);
	  n_pos_in_item = p_s_range_head->k_offset - p_s_ih->ih_key.k_offset;
	  if ( I_IS_INDIRECT_ITEM(p_s_ih) )
	    n_pos_in_item /= p_s_bh->b_size;
	}
      }
    }

    if ( n_pos_in_item == MAX_INT ) {
      /* key we looked for is not in the item */
      if ( n_pos_in_buffer == n_item_num )
	copy_key(&s_min_key, &MIN_KEY);
      else {
 	n_pos_in_item = 0;
	copy_key(&s_min_key, &(p_s_ih->ih_key));
      }
    }
  }

  if ( COMP_KEYS(&s_min_key, p_s_range_end) == 1 ) {
 
/******************************************
    if ( ! key_in_buffer(&s_path, p_s_range_head, p_s_sb) )
      reiserfs_panic(p_s_sb, "PAP-2: get_buffer_by_range: path length is too small");
*********************************************/

    *pp_s_buf = NULL;
    copy_key(p_s_range_head, &s_min_key);
    decrement_counters_in_path(&s_path);
    *p_n_objectid = MAX_KEY_OBJECTID;
    return 0;    /* There is no buffer in the range in the tree. */
  }

  if ( ! COMP_KEYS(&s_min_key, &MIN_KEY) ) {
    /* This leaf buffer is not in the range. */
    p_s_rkey = get_rkey(&s_path, p_s_sb);
    if ( ! COMP_KEYS(p_s_rkey, &MIN_KEY) ) {

#ifdef REISERFS_CHECK_ONE_PROCESS
      reiserfs_panic(p_s_sb, "PAP-5810: get_buffer_by_range: can not get right delimiting key in case of one process");
#endif

#ifdef REISERFS_CHECK
      if ( n_repeat == CARRY_ON )
	reiserfs_panic(p_s_sb, "PAP-5820: get_buffer_by_range: get_rkey returns MIN_KEY");
#endif

      goto repeat; /* We can not recalculate right delimiting key to continue search for the buffer in the range.
		      Do that by old one. */
    }

    if ( ! COMP_KEYS(p_s_rkey, &MAX_KEY) ) {

#ifdef REISERFS_CHECK
      if ( ! key_in_buffer(&s_path, p_s_range_head, p_s_sb) )
	reiserfs_panic(p_s_sb, "PAP-5830: get_buffer_by_range: key_in_buffer returned 0");
#endif

      *pp_s_buf = NULL;
      copy_key(p_s_range_head, &MAX_KEY);
      decrement_counters_in_path(&s_path);
      *p_n_objectid = MAX_KEY_OBJECTID;
      return 0;    /* There is no buffer in the range in the tree. */
    }

    *pp_s_buf = NULL;
    copy_key(p_s_range_head, p_s_rkey); /* Reset range head and continue search for the buffer in the range. */
    decrement_counters_in_path(&s_path);
    *p_n_objectid = MAX_KEY_OBJECTID;
    return 1;
  }


  if ( ! I_IS_INDIRECT_ITEM(p_s_ih) ) { /*  We have direct or directory item which contains byte or
                                          directory record in the range in the buffer p_s_bh. */
    /* Look for the next indirect item in the buffer */
    for ( p_s_ih++, n_pos_in_buffer++; n_pos_in_buffer < n_item_num; n_pos_in_buffer++, p_s_ih++ )
      if ( I_IS_INDIRECT_ITEM(p_s_ih) )
        break;

    if ( n_pos_in_buffer == n_item_num ) {
      /* indirect item was not found. */
      p_s_rkey = get_rkey(&s_path, p_s_sb);
      if ( ! COMP_KEYS(p_s_rkey, &MIN_KEY) ) {

#ifdef REISERFS_CHECK_ONE_PROCESS
	reiserfs_panic(p_s_sb, "PAP-5830: get_buffer_by_range: can not get right delimiting key in case of one process");
#endif

#ifdef REISERFS_CHECK
	if ( n_repeat == CARRY_ON )
	  reiserfs_panic(p_s_sb, "PAP-5840: get_buffer_by_range: get_rkey returns MIN_KEY");
#endif
	
	goto repeat; /* We can not recalculate right delimiting key to continue search for the buffers in the range.
			Do that by old one. */
      }
      
#ifdef REISERFS_CHECK
      if ( ! key_in_buffer(&s_path, p_s_range_head, p_s_sb) )
	reiserfs_panic(p_s_sb, "PAP-5850: get_buffer_by_range: key_in_buffer returned 0");

      if ( COMP_KEYS(B_N_PKEY(p_s_bh, 0), p_s_range_head) == 1 || COMP_KEYS(B_PRIGHT_DELIM_KEY(p_s_bh), p_s_range_head ) < 1 ) {
	reiserfs_panic(p_s_sb, "PAP-5860: get_buffer_by_range: range_head key %k is not in the last buffer on the path",
		       p_s_range_head);
      }
#endif

      copy_key(p_s_range_head, p_s_rkey); /* Reset range_head. */
    }
    else /* indirect item was found. */ {

#ifdef REISERFS_CHECK
      if ( COMP_KEYS(B_N_PKEY(p_s_bh, 0), p_s_range_head) == 1 || COMP_KEYS(B_PRIGHT_DELIM_KEY(p_s_bh), p_s_range_head ) < 1 ) {
	reiserfs_panic(p_s_sb, "PAP-5870: get_buffer_by_range: range_head key %k is not in the last buffer on the path",
		       p_s_range_head);
      }
#endif

/******************************************
      if ( ! key_in_buffer(&s_path, p_s_range_head, p_s_sb) )
	reiserfs_panic(p_s_sb, "PAP-4: get_buffer_by_range: path length is too small");
*********************************************/

      copy_key(p_s_range_head, &(p_s_ih->ih_key)); /* Reset range head. */
    }

    *pp_s_buf = p_s_bh;

#ifdef REISERFS_CHECK
    if ( s_path.path_length < FIRST_PATH_ELEMENT_OFFSET )
      reiserfs_panic(0, "PAP-5880: get_buffer_by_range: path length is too small (%d)", s_path.path_length);
#endif

    s_path.path_length--;
    decrement_counters_in_path(&s_path);
    *p_n_objectid = MAX_KEY_OBJECTID;
    return 1;
  }

  /* Needed byte is located in an unformatted node. Check whether it is in cache.
      And if not prepare buffer to read it. */
  n_unfm_pointer = B_I_POS_UNFM_POINTER(p_s_bh, p_s_ih, n_pos_in_item);
  if ( ! n_unfm_pointer ) { /* This is a hole (nothing to read). */
    if ( n_pos_in_item + 1 == I_UNFM_NUM(p_s_ih) )
      if ( n_pos_in_buffer + 1 == B_NR_ITEMS(p_s_bh) ) {
	p_s_rkey = get_rkey(&s_path, p_s_sb);

	if ( ! COMP_KEYS(p_s_rkey, &MIN_KEY) )
	  goto repeat;

	copy_key(p_s_range_head, p_s_rkey);
      }
      else
	copy_key(p_s_range_head, &((p_s_ih + 1)->ih_key));
    else {
      copy_key(p_s_range_head, &(p_s_ih->ih_key));
      p_s_range_head->k_offset += (n_pos_in_item + 1)*(p_s_sb->s_blocksize);
    }

    *pp_s_buf = NULL;
    decrement_counters_in_path(&s_path);
    *p_n_objectid = p_s_ih->ih_key.k_objectid;
    return 1;
  } /* unformatted node pointer contains 0 */


  /* Copy found item header. */
  copy_item_head(&s_ih, p_s_ih);
  /* Get unformatted node buffer. */
  n_repeat = CARRY_ON;
  p_s_bh = reiserfs_getblk(p_s_sb->s_dev, n_unfm_pointer, p_s_sb->s_blocksize, &n_repeat);

  if ( n_repeat != CARRY_ON  && comp_items(&s_ih, &s_path) ) {

#ifdef REISERFS_CHECK_ONE_PROCESS
    reiserfs_panic(p_s_sb, "PAP-5890: get_buffer_by_range: item in the path is changed in case of one process");
#endif

    brelse(p_s_bh);
    goto repeat;
  }

#ifdef REISERFS_CHECK
  if ( comp_items(&s_ih, &s_path) )
    reiserfs_panic(p_s_sb, "PAP-5900: get_buffer_by_range: items must be equal");
#endif

  if ( n_pos_in_item + 1 == I_UNFM_NUM(p_s_ih) )
    if ( n_pos_in_buffer + 1 == B_NR_ITEMS(PATH_PLAST_BUFFER(&s_path)) ) {
      p_s_rkey = get_rkey(&s_path, p_s_sb);

      if ( ! COMP_KEYS(p_s_rkey, &MIN_KEY) ) {
	brelse(p_s_bh);
	goto repeat;
      }

      copy_key(p_s_range_head, p_s_rkey);
    }
    else
     copy_key(p_s_range_head, &((p_s_ih + 1)->ih_key));
  else {
    copy_key(p_s_range_head, &(p_s_ih->ih_key));
    p_s_range_head->k_offset += (n_pos_in_item + 1)*(p_s_sb->s_blocksize);
  }

  *pp_s_buf = p_s_bh;
  decrement_counters_in_path(&s_path);
  *p_n_objectid = s_ih.ih_key.k_objectid;
  return 1; 
}


int get_buffers_from_range(					/*  Returns length of the array of calculated
								    buffers which is less or equal than
								    max_nr_buffers_to_return.         			*/
      struct  super_block     *	p_s_sb,				/*  Pointer to the super block.				*/
      struct  key	      *	p_s_range_start,		/*  Minimal range key.                            	*/
      struct  key	      * p_s_range_end,			/*  Maximal range key.                    		*/
      struct  buffer_head    **	p_s_range_buffers,		/*  Returned array of pointers to buffer headers.
								    Must be allocated in calling function.		*/
      int     			n_max_nr_buffers_to_return	/*  Length of the allocated array.                	*/
    ) {
  struct key          * p_s_cur_key = p_s_range_start;
  struct buffer_head  * p_s_res_buffer;
  int			n_array_length = 0;
  unsigned long		n_objectid;

#ifdef REISERFS_CHECK
  if ( COMP_KEYS(p_s_range_start, p_s_range_end) == 1 ||
       ! COMP_KEYS(p_s_range_start, &MIN_KEY) || ! COMP_KEYS(p_s_range_start, &MAX_KEY) ||
       ! COMP_KEYS(p_s_range_end, &MIN_KEY) || ! COMP_KEYS(p_s_range_end, &MAX_KEY) )
    reiserfs_panic(p_s_sb, "PAP-5890: get_buffers_from_range: illegal range");
#endif

  /* While p_s_cur_key is in the range. */
  while ( COMP_KEYS(p_s_cur_key, p_s_range_end) != 1 )  {
    /* Calculate next buffer from range. */
    if ( ! get_buffer_by_range(p_s_sb, p_s_cur_key, p_s_range_end, &p_s_res_buffer, &n_objectid) )
      break; /* There are not more buffers in the range. */
    p_s_range_buffers[n_array_length++] = p_s_res_buffer;
    if ( n_array_length == n_max_nr_buffers_to_return )
      break;
  }
  return n_array_length;
}

/* ok, this is not good at all.  Sometimes, we need to search for just an 
** object id, without knowing the packing locality.  For the moment, this
** searches through every possible packing locality until it founds the
** object id in your key.  It starts by making a copy of the key, and setting
** the k_dir_id to 0.  Every other arg works like search_by_key.
**
** After this returns, the last item in the p_s_search_path will have the
** correct k_dir_id and k_objectid.
**
** This function is SLOW.  I mean really really SLOW.  Don't ever call it
** unless you have no way at all to get the packing locality.
**
*/
int search_by_objectid(
                  struct super_block  * p_s_sb,         /* Super block.                           */
                  struct key          * p_s_key,        /* Key to search. packing locality should be set to 0s */
                  struct path         * p_s_search_path,/* This structure was allocated and initialized by
                                                           the calling function. It is filled up by this
                                                           function.  */
                  int                 * p_n_repeat,     /* Whether schedule occured. */
                  int                   n_stop_level,   /* How far down the tree to search.*/
                  int                   n_bread_par     /* Whether to search even if it requires disk I/O, this is
                                                           either READ_BLOCKS or DONT_READ_BLOCKS or 0. Hans doesn't
                                                           know what 0 means, it seems to evaluate to DONT_READ_BLOCKS,
                                                           but it is bad style to not use the macro.... there is a
                                                           #define of search by key with no explanation that can allow
                                                           it to happen.... */
                  ) {
  struct key cur_key ;
  struct item_head *ih ;
  int loop_count = 0 ;
  int retval ;
  unsigned long *objectid_map  ;
  struct reiserfs_super_block *disk_sb ;
  unsigned long max_objectid ;

  /*  find the max possible objectid to search for */
  disk_sb = SB_DISK_SUPER_BLOCK(p_s_sb) ;
  objectid_map = (unsigned long *)(disk_sb + 1) ;
  max_objectid = objectid_map[disk_sb->s_oid_cursize - 1] ;

  copy_key(&cur_key, p_s_key) ;
  cur_key.k_dir_id = 0 ;
  while(1) {
    retval = search_by_key(p_s_sb, &cur_key, p_s_search_path, p_n_repeat, n_stop_level, n_bread_par) ;    
    ih = PATH_PITEM_HEAD(p_s_search_path) ;
    if (retval == ITEM_FOUND) {
      return retval ;
    } else if (retval == ITEM_NOT_FOUND) {
      cur_key.k_dir_id++ ;
      if (cur_key.k_dir_id > max_objectid) {
	reiserfs_warning("clm-1001: search_by_objectid: current key dir id %d is > than max object id %lu, giving up\n", cur_key.k_dir_id, max_objectid) ;
        return retval ;
      }
    } else {
      return retval ;
    }
    if ((++loop_count % 10000000) == 0) {
      reiserfs_warning("clm-1000: search_by_objectid, item not found after %d iterations looking for %k, last attempt %k\n", loop_count, p_s_key, &(ih->ih_key)) ;
    }
  }
  return retval ;
}
