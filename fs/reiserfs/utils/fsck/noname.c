/*
 * Copyright 1996, 1997, 1998 Hans Reiser
 */

/*#include <stdio.h>*/
/*#include <string.h>*/
/*#include <sys/types.h>*/
/*#include <asm/bitops.h>
#include "../include/reiserfs_fs.h"
#include "../include/reiserfs_fs_sb.h"
#include "../include/reiserfslib.h"*/
#include "fsck.h"


void get_max_buffer_key (struct buffer_head * bh, struct key * key)
{
  struct item_head * ih;

  ih = B_N_PITEM_HEAD (bh, B_NR_ITEMS (bh) - 1);
  copy_key (key, &(ih->ih_key));

  if (KEY_IS_DIRECTORY_KEY (key)) {
    /* copy 3-rd and 4-th key components of the last entry */
    key->k_offset = B_I_DEH (bh, ih)[I_ENTRY_COUNT (ih) - 1].deh_offset;
    key->k_uniqueness = DIRENTRY_UNIQUENESS;
  } else if (!KEY_IS_STAT_DATA_KEY (key))
    /* get key of the last byte, which is contained in the item */
    key->k_offset += I_BYTES_NUMBER (ih, bh->b_size) - 1;

}


#if 0
int are_items_mergeable (struct item_head * left, struct item_head * right, int bsize)
{
  if (comp_keys (&left->ih_key, &right->ih_key) != SECOND_GREATER) {
    print_key (&(left->ih_key));
    print_key (&(right->ih_key));
    die ("are_items_mergeable: second key is not greater");
  }

  if (comp_short_keys (&left->ih_key, &right->ih_key) != KEYS_IDENTICAL)
    return NO;

  if (I_IS_DIRECTORY_ITEM (left)) {
    if (!I_IS_DIRECTORY_ITEM (right))
      die ("are_items_mergeable: right item must be of directory type");
    return 1;
  }

  if ((I_IS_DIRECT_ITEM (left) && I_IS_DIRECT_ITEM (right)) || 
      (I_IS_INDIRECT_ITEM (left) && I_IS_INDIRECT_ITEM (right)))
    return (left->ih_key.k_offset + I_BYTES_NUMBER (left, bsize) == right->ih_key.k_offset) ? 1 : 0;

  return 0;
}


static void decrement_key (struct key * key)
{
  unsigned long * key_field = (unsigned long *)key + REISERFS_FULL_KEY_LEN - 1;
  int i;

  for (i = 0; i < REISERFS_FULL_KEY_LEN; i ++, key_field--)
    if (*key_field) {
      (*key_field)--;
      break;
    }

  if (i == REISERFS_FULL_KEY_LEN)
    die ("decrement_key: zero key found");
}


/* get left neighbor of the leaf node */
static struct buffer_head * get_left_neighbor (struct path * path)
{
  struct key key;
  struct path path_to_left_neighbor;
  struct buffer_head * bh;

  copy_key (&key, B_N_PKEY (PATH_PLAST_BUFFER (path), 0));
  decrement_key (&key);  

  reiserfsck_search_by_key (&g_sb, &key, &path_to_left_neighbor, comp_keys);
  if (PATH_LAST_POSITION (&path_to_left_neighbor) == 0) {
    pathrelse (&path_to_left_neighbor);
    return 0;
  }
  bh = PATH_PLAST_BUFFER (&path_to_left_neighbor);
  bh->b_count ++;
  pathrelse (&path_to_left_neighbor);
  return bh;
}


int is_left_mergeable (struct path * path)
{
  struct item_head * right;
  struct buffer_head * bh;
  int retval;
  
  right = B_N_PITEM_HEAD (PATH_PLAST_BUFFER (path), 0);

  bh = get_left_neighbor (path);
  if (bh == 0) {
    return 0;
  }
  retval = are_items_mergeable (B_N_PITEM_HEAD (bh, B_NR_ITEMS (bh) - 1), right, bh->b_size);
  brelse (bh);
  return retval;
}


static struct buffer_head * get_right_neighbor (struct path * path)
{
  struct key key;
  struct key * rkey;
  struct path path_to_right_neighbor;
  struct buffer_head * bh;
  struct key maxkey = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};

  rkey = get_right_dkey (path);
  if (rkey == 0)
    copy_key (&key, &maxkey);
  else
    copy_key (&key, rkey);

  reiserfsck_search_by_key (&g_sb, &key, &path_to_right_neighbor, comp_keys);
  if (PATH_PLAST_BUFFER (&path_to_right_neighbor) == PATH_PLAST_BUFFER (path)) {
    pathrelse (&path_to_right_neighbor);
    return 0;
  }
  bh = PATH_PLAST_BUFFER (&path_to_right_neighbor);
  bh->b_count ++;
  pathrelse (&path_to_right_neighbor);
  return bh;
}


int is_right_mergeable (struct path * path)
{
  struct item_head * left;
  struct buffer_head * bh;
  int retval;
  
  left = B_N_PITEM_HEAD (PATH_PLAST_BUFFER (path), B_NR_ITEMS (PATH_PLAST_BUFFER (path)) - 1);

  bh = get_right_neighbor (path);
  if (bh == 0) {
    return 0;
  }
  retval = are_items_mergeable (left, B_N_PITEM_HEAD (bh, 0), bh->b_size);
  brelse (bh);
  return retval;
}

#endif /*0*/


#if 0
/* retunrs 1 if buf looks like a leaf node, 0 otherwise */
static int is_leaf (char * buf)
{
  struct block_head * blkh;
  struct item_head * ih;
  int used_space;
  int prev_location;
  int i;

  blkh = (struct block_head *)buf;
  ih = (struct item_head *)(buf + BLKH_SIZE) + blkh->blk_nr_item - 1;
  used_space = BLKH_SIZE + IH_SIZE * blkh->blk_nr_item + (g_sb.s_blocksize - ih->ih_item_location);
  if (used_space != g_sb.s_blocksize - blkh->blk_free_space)
    return 0;
  ih = (struct item_head *)(buf + BLKH_SIZE);
  prev_location = g_sb.s_blocksize;
  for (i = 0; i < blkh->blk_nr_item; i ++, ih ++) {
    if (ih->ih_item_location >= g_sb.s_blocksize || ih->ih_item_location < IH_SIZE * blkh->blk_nr_item)
      return 0;
    if (ih->ih_item_len < 1 || ih->ih_item_len > MAX_ITEM_LEN (g_sb.s_blocksize))
      return 0;
    if (prev_location - ih->ih_item_location != ih->ih_item_len)
      return 0;
    prev_location = ih->ih_item_location;
  }

  return 1;
}


/* retunrs 1 if buf looks like an internal node, 0 otherwise */
static int is_internal (char * buf)
{
  struct block_head * blkh;
  int used_space;

  blkh = (struct block_head *)buf;
  used_space = BLKH_SIZE + KEY_SIZE * blkh->blk_nr_item + DC_SIZE * (blkh->blk_nr_item + 1);
  if (used_space != g_sb.s_blocksize - blkh->blk_free_space)
    return 0;
  return 1;
}


/* sometimes unfomatted node looks like formatted, if we check only
   block_header. This is the reason, why it is so complicated. We
   believe only when free space and item locations are ok 
   */
int not_formatted_node (char * buf)
{
  struct block_head * blkh;

  blkh = (struct block_head *)buf;

  if (blkh->blk_level < DISK_LEAF_NODE_LEVEL || blkh->blk_level > MAX_HEIGHT)
    /* blk_level is out of range */
    return 1;

  if (blkh->blk_nr_item < 1 || blkh->blk_nr_item > (g_sb.s_blocksize - BLKH_SIZE) / IH_SIZE)
    /* item number is out of range */
    return 1;

  if (blkh->blk_free_space > g_sb.s_blocksize - BLKH_SIZE - IH_SIZE)
    /* free space is out of range */
    return 1;

  /* check format of nodes, such as we are not sure, that this is formatted node */
  if (blkh->blk_level == DISK_LEAF_NODE_LEVEL)
    return (is_leaf (buf) == 1) ? 0 : 1;
  return (is_internal (buf) == 1) ? 0 : 1;
}


int is_internal_node (char * buf)
{
  struct block_head * blkh;
  
  blkh = (struct block_head *)buf;
  if (blkh->blk_level != DISK_LEAF_NODE_LEVEL)
    return 1;
  return 0;
}

#endif /*0*/

/*
int ready_preserve_list (struct tree_balance * tb, struct buffer_head * bh)
{
  return 0;
}


void	preserve_shifted (
			  struct tree_balance * tb,
			  struct buffer_head **bh,
			  struct buffer_head * parent,
			  int position,
			  struct buffer_head * dest)
{
  return;
}
*/

#if 0

char * strs[] =
{"0%",".",".",".",".","20%",".",".",".",".","40%",".",".",".",".","60%",".",".",".",".","80%",".",".",".",".","100%"};

char progress_to_be[1024];
char current_progress[1024];

void str_to_be (char * buf, int prosents)
{
  int i;
  prosents -= prosents % 4;
  buf[0] = 0;
  for (i = 0; i <= prosents / 4; i ++)
    strcat (buf, strs[i]);
}


void print_how_far (unsigned long * passed, unsigned long total)
{
  int n;

  if (*passed == 0)
    current_progress[0] = 0;

  if (*passed >= total) {
    printf/*die*/ ("print_how_far: total %lu has been reached already. cur=%lu\n", total, ++(*passed));
    return;
  }

  (*passed) ++;
  n = ((double)((double)(*passed) / (double)total) * (double)100);

  str_to_be (progress_to_be, n);

  if (strlen (current_progress) != strlen (progress_to_be))
    printf ("%s", progress_to_be + strlen (current_progress));

  strcat (current_progress, progress_to_be + strlen (current_progress));


  fflush (stdout);
}
#endif

