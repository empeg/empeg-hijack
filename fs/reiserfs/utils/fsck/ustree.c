/*
 * Copyright 1996, 1997 Hans Reiser
 */
/*#include <stdio.h>
#include <string.h>*/
/*#include <asm/bitops.h>
#include "../include/reiserfs_fs.h"
#include "../include/reiserfs_fs_sb.h"
#include "../include/reiserfslib.h"*/
#include "fsck.h"

static inline int compare_keys (unsigned long * key1, unsigned long * key2, int length)
{
  for (; length--; ++key1, ++key2) {
    if ( *key1 < *key2 )
      return SECOND_GREATER;
    if ( *key1 > *key2 )
      return FIRST_GREATER;
  }
  
  return KEYS_IDENTICAL;
}


/* compare 3 components of key */
int comp_keys_3 (void * key1, void * key2)
{
  return compare_keys (key1, key2, 3);
}


/* compare 4 components of key */
int comp_dir_entries (void * key1, void * key2)
{
  return compare_keys (key1, key2, 1);
}

void init_tb_struct (struct tree_balance * tb, struct super_block  * s, struct path * path, int size)
{
  memset (tb, '\0', sizeof(struct tree_balance));
  tb->tb_sb = s;
  tb->tb_path = path;
  PATH_OFFSET_PBUFFER(path, ILLEGAL_PATH_ELEMENT_OFFSET) = NULL;
  PATH_OFFSET_POSITION(path, ILLEGAL_PATH_ELEMENT_OFFSET) = 0;
  tb->insert_size[0] = size;
}

struct tree_balance * cur_tb = 0;

void reiserfsck_paste_into_item (struct path * path, const char * body, int size)
{
  struct tree_balance tb;
  
  init_tb_struct (&tb, &g_sb, path, size);
  if (fix_nodes (0/*th*/, M_PASTE, &tb, path->pos_in_item, 0) != CARRY_ON)
    die ("reiserfsck_paste_into_item: fix_nodes failed");

  do_balance (0/*th*/, &tb, path->pos_in_item, 0, body, M_PASTE, REISERFS_KERNEL_MEM, 0);
}


void reiserfsck_insert_item (struct path * path, struct item_head * ih, const char * body)
{
  struct tree_balance tb;

  init_tb_struct (&tb, &g_sb, path, IH_SIZE + ih->ih_item_len);
  if (fix_nodes (0/*th*/, M_INSERT, &tb, 0, ih) != CARRY_ON)
    die ("reiserfsck_insert_item: fix_nodes failed");

  do_balance (0/*th*/, &tb, 0, ih, body, M_INSERT, REISERFS_KERNEL_MEM, 0);
}


static void free_unformatted_nodes (struct item_head * ih, struct buffer_head * bh)
{
  unsigned long * punfm = (unsigned long *)B_I_PITEM (bh, ih);
  int i;

  for (i = 0; i < I_UNFM_NUM (ih); i ++, punfm ++)
    if (*punfm) {
      struct buffer_head * to_be_forgotten;

      to_be_forgotten = find_buffer (g_sb.s_dev, *punfm, g_sb.s_blocksize);
      if (to_be_forgotten) {
	to_be_forgotten->b_count ++;
	bforget (to_be_forgotten);
      }
      reiserfs_free_block (0/*th*/, &g_sb, *punfm);
/* this is for check only */
      unmark_block_unformatted (*punfm);
    }
}


void reiserfsck_delete_item (struct path * path)
{
  struct tree_balance tb;
  struct item_head * ih = PATH_PITEM_HEAD (path);

  if (I_IS_INDIRECT_ITEM (ih))
    free_unformatted_nodes (ih, PATH_PLAST_BUFFER (path));

  init_tb_struct (&tb, &g_sb, path, -(IH_SIZE + ih->ih_item_len));
  if (fix_nodes (0/*th*/, M_DELETE, &tb, 0, 0) != CARRY_ON)
    die ("reiserfsck_delete_item: fix_nodes failed");

  do_balance (0/*th*/, &tb, 0, 0, 0, M_DELETE, REISERFS_KERNEL_MEM, 0);
}


void reiserfsck_cut_from_item (struct path * path, int cut_size)
{
  struct tree_balance tb;
  struct item_head * ih;

  if (cut_size >= 0)
    die ("reiserfsck_cut_from_item: cut size == %d", cut_size);

  if (I_IS_INDIRECT_ITEM (ih = PATH_PITEM_HEAD (path))) {
    __u32 unfm_ptr = B_I_POS_UNFM_POINTER (PATH_PLAST_BUFFER (path), ih, I_UNFM_NUM (ih) - 1);
    if (unfm_ptr) {
      struct buffer_head * to_be_forgotten;

      to_be_forgotten = find_buffer (g_sb.s_dev, unfm_ptr, g_sb.s_blocksize);
      if (to_be_forgotten) {
        to_be_forgotten->b_count ++;
        bforget (to_be_forgotten);
      }
      reiserfs_free_block (0/*th*/, &g_sb, unfm_ptr);
/* this is for check only */
      unmark_block_unformatted (unfm_ptr);
    }
  }


  init_tb_struct (&tb, &g_sb, path, cut_size);
  if (fix_nodes (0/*th*/, M_CUT, &tb, path->pos_in_item, 0) != CARRY_ON)
    die ("reiserfsck_cut_from_item: fix_nodes failed");

  do_balance (0/*th*/, &tb, path->pos_in_item, 0, 0, M_CUT, REISERFS_KERNEL_MEM, 0);
}


/* uget_lkey is utils clone of stree.c/get_lkey */
struct key * uget_lkey (struct path * path)
{
  int pos, offset = path->path_length;
  struct buffer_head * bh;
  
  if (offset < FIRST_PATH_ELEMENT_OFFSET)
    die ("uget_lkey: illegal offset in the path (%d)", offset);


  /* While not higher in path than first element. */
  while (offset-- > FIRST_PATH_ELEMENT_OFFSET) {
    if (! buffer_uptodate (PATH_OFFSET_PBUFFER (path, offset)) )
      die ("uget_lkey: parent is not uptodate");

    /* Parent at the path is not in the tree now. */
    if (! B_IS_IN_TREE (bh = PATH_OFFSET_PBUFFER (path, offset)))
      die ("uget_lkey: buffer on the path is not in tree");

    /* Check whether position in the parent is correct. */
    if ((pos = PATH_OFFSET_POSITION (path, offset)) > B_NR_ITEMS (bh))
      die ("uget_lkey: invalid position (%d) in the path", pos);

    /* Check whether parent at the path really points to the child. */
    if (B_N_CHILD_NUM (bh, pos) != PATH_OFFSET_PBUFFER (path, offset + 1)->b_blocknr)
      die ("uget_lkey: invalid block number (%d). Must be %d",
	   B_N_CHILD_NUM (bh, pos), PATH_OFFSET_PBUFFER (path, offset + 1)->b_blocknr);

    /* Return delimiting key if position in the parent is not equal to zero. */
    if (pos)
      return B_N_PDELIM_KEY(bh, pos - 1);
  }

  /* we must be in the root */
/*
  if (PATH_OFFSET_PBUFFER (path, FIRST_PATH_ELEMENT_OFFSET)->b_blocknr != SB_ROOT_BLOCK (&g_sb))
    die ("get_left_dkey: path does not start with the root");
*/

  /* there is no left delimiting key */
  return 0;
}


/* uget_rkey is utils clone of stree.c/get_rkey */
struct key * uget_rkey (struct path * path)
{
  int pos, offset = path->path_length;
  struct buffer_head * bh;

  if (offset < FIRST_PATH_ELEMENT_OFFSET)
    die ("uget_rkey: illegal offset in the path (%d)", offset);

  while (offset-- > FIRST_PATH_ELEMENT_OFFSET) {
    if (! buffer_uptodate (PATH_OFFSET_PBUFFER (path, offset)))
      die ("uget_rkey: parent is not uptodate");

    /* Parent at the path is not in the tree now. */
    if (! B_IS_IN_TREE (bh = PATH_OFFSET_PBUFFER (path, offset)))
      die ("uget_rkey: buffer on the path is not in tree");

    /* Check whether position in the parrent is correct. */
    if ((pos = PATH_OFFSET_POSITION (path, offset)) > B_NR_ITEMS (bh))
      die ("uget_rkey: invalid position (%d) in the path", pos);

    /* Check whether parent at the path really points to the child. */
    if (B_N_CHILD_NUM (bh, pos) != PATH_OFFSET_PBUFFER (path, offset + 1)->b_blocknr)
      die ("uget_rkey: invalid block number (%d). Must be %d",
	   B_N_CHILD_NUM (bh, pos), PATH_OFFSET_PBUFFER (path, offset + 1)->b_blocknr);

    /* Return delimiting key if position in the parent is not the last one. */
    if (pos != B_NR_ITEMS (bh))
      return B_N_PDELIM_KEY(bh, pos);
  }

  /* we must be in the root */
/*
  if (PATH_OFFSET_PBUFFER (path, FIRST_PATH_ELEMENT_OFFSET)->b_blocknr != SB_ROOT_BLOCK (&g_sb))
    die ("get_left_dkey: path does not start with the root");
*/
  /* there is no right delimiting key */
  return 0;
}


static inline int ubin_search (void * key, void * base, int num, int width, int *ppos, comp_function_t comp_func)
{
  int   rbound, lbound, j;
  
  lbound = 0;
  rbound = num - 1;
  for (j = (rbound + lbound) / 2; lbound <= rbound; j = (rbound + lbound) / 2) {
    switch (comp_func ((void *)((char *)base + j * width), key ) ) {
    case SECOND_GREATER:
      lbound = j + 1; 
      continue;

    case FIRST_GREATER:
      rbound = j - 1;
      continue;

    case KEYS_IDENTICAL:
      *ppos = j;
      return KEY_FOUND;
    }
  }

  *ppos = lbound;
  return KEY_NOT_FOUND;
}


/* this searches in tree through items */
int usearch_by_key (struct super_block * s, struct key * key, struct path * path, int * repeat, int stop_level, int bread_par, 
		   comp_function_t comp_func)
{
  struct buffer_head * bh;
  unsigned long block = s->u.reiserfs_sb.s_rs->s_root_block;
  struct path_element * curr;

  if (comp_func == 0)
    comp_func = comp_keys;
  if (repeat)
    *repeat = CARRY_ON;

  path->path_length = ILLEGAL_PATH_ELEMENT_OFFSET;
  while (1) {
    curr = PATH_OFFSET_PELEMENT (path, ++ path->path_length);
    bh = curr->pe_buffer = bread (s->s_dev, block, s->s_blocksize);
    if (ubin_search (key, B_N_PKEY (bh, 0), B_NR_ITEMS (bh),
		    B_IS_ITEMS_LEVEL (bh) ? IH_SIZE : KEY_SIZE, &(curr->pe_position), comp_func) == KEY_FOUND) {
      /* key found, return if this is leaf level */
      if (B_BLK_HEAD (bh)->blk_level <= stop_level) {
	path->pos_in_item = 0;
	return KEY_FOUND;
      }
      curr->pe_position ++;
    } else {
      /* key not found in the node */
      if (B_BLK_HEAD (bh)->blk_level <= stop_level)
	return KEY_NOT_FOUND;
    }
    block = B_N_CHILD_NUM (bh, curr->pe_position);
  }
  die ("search_by_key: you can not get here");
  return 0;
}


/* key is key of directory entry. This searches in tree through items
   and in the found directory item as well */
int usearch_by_entry_key (struct super_block * s, struct key * key, struct path * path)
{
  struct buffer_head * bh;
  struct item_head * ih;
  struct key tmpkey;

  if (usearch_by_key (s, key, path, 0, DISK_LEAF_NODE_LEVEL, 0, comp_keys) == KEY_FOUND) {
    path->pos_in_item = 0;
    return ENTRY_FOUND;
  }

  bh = PATH_PLAST_BUFFER (path);
  if (PATH_LAST_POSITION (path) == 0) {
    /* previous item does not exist, that means we are in leftmost
       leaf of the tree */
    if (uget_lkey (path) != 0)
      die ("search_by_entry_key: invalid position after search_by_key");
    if (comp_short_keys ((unsigned long *)B_N_PKEY (bh, 0), (unsigned long *)key) == KEYS_IDENTICAL) {
      path->pos_in_item = 0;
      return ENTRY_NOT_FOUND;
    }
    path->pos_in_item = 0;
    return DIRECTORY_NOT_FOUND;
  }

  /* take previous item */
  PATH_LAST_POSITION (path) --;
  ih = PATH_PITEM_HEAD (path);
  if (comp_short_keys ((unsigned long *)ih, (unsigned long *)key) != KEYS_IDENTICAL || !I_IS_DIRECTORY_ITEM (ih)) {
    struct key * next_key;

    PATH_LAST_POSITION (path) ++;
    /* previous item belongs to another object or is stat data, check next item */
    if (PATH_LAST_POSITION (path) < B_NR_ITEMS (PATH_PLAST_BUFFER (path))) {
      /* found item is not last item of the node */
      next_key = B_N_PKEY (PATH_PLAST_BUFFER (path), PATH_LAST_POSITION (path));
      if (comp_short_keys ((unsigned long *)next_key, (unsigned long *)key) != KEYS_IDENTICAL) {
	path->pos_in_item = 0;
	return DIRECTORY_NOT_FOUND;
      }
      if (!KEY_IS_DIRECTORY_KEY (next_key))
	/* there is an item in the tree, but it is not a directory item */
	return REGULAR_FILE_FOUND;
    } else {
      /* found item is last item of the node */
      next_key = uget_rkey (path);
      if (next_key == 0 || comp_short_keys ((unsigned long *)next_key, (unsigned long *)key) != KEYS_IDENTICAL) {
	/* there is not any part of such directory in the tree */
	path->pos_in_item = 0;
	return DIRECTORY_NOT_FOUND;
      }
      if (!KEY_IS_DIRECTORY_KEY (next_key))
	/* there is an item in the tree, but it is not a directory item */
	return REGULAR_FILE_FOUND;    
      
      copy_key (&tmpkey, next_key);
      pathrelse (path);
      if (usearch_by_key (s, &tmpkey, path, 0, DISK_LEAF_NODE_LEVEL, 0, comp_keys) != KEY_FOUND || PATH_LAST_POSITION (path) != 0)
	die ("search_by_entry_key: item not found by corresponding delimiting key");
    }
    /* next item is the part of this directory */
    path->pos_in_item = 0;
    return ENTRY_NOT_FOUND;
  }

  /* previous item is part of desired directory */
  if (ubin_search (&(key->k_offset), B_I_DEH (bh, ih), I_ENTRY_COUNT (ih), DEH_SIZE, &(path->pos_in_item), comp_dir_entries) == KEY_FOUND)
    return ENTRY_FOUND;
  return ENTRY_NOT_FOUND;
}


/* key is key of byte in the regular file. This searches in tree
   through items and in the found item as well */
int usearch_by_position (struct super_block * s, struct key * key, struct path * path)
{
  struct buffer_head * bh;
  struct item_head * ih;

  if (usearch_by_key (s, key, path, 0, DISK_LEAF_NODE_LEVEL, 0, comp_keys_3) == KEY_FOUND) {
    ih = PATH_PITEM_HEAD (path);
    if (!I_IS_DIRECT_ITEM (ih) && !I_IS_INDIRECT_ITEM (ih))
      return DIRECTORY_FOUND;
    path->pos_in_item = 0;
    return BYTE_FOUND;
  }

  bh = PATH_PLAST_BUFFER (path);
  ih = PATH_PITEM_HEAD (path);
  if (PATH_LAST_POSITION (path) == 0) {
    /* previous item does not exist, that means we are in leftmost leaf of the tree */
    if (comp_short_keys ((unsigned long *)B_N_PKEY (bh, 0), (unsigned long *)key) == KEYS_IDENTICAL) {
      if (!I_IS_DIRECT_ITEM (ih) && !I_IS_INDIRECT_ITEM (ih))
	return DIRECTORY_FOUND;
      return BYTE_NOT_FOUND;
    }
    return FILE_NOT_FOUND;
  }

  /* take previous item */
  PATH_LAST_POSITION (path) --;
  ih = PATH_PITEM_HEAD (path);
  if (comp_short_keys ((unsigned long *)&ih->ih_key, (unsigned long *)key) != KEYS_IDENTICAL ||
      I_IS_STAT_DATA_ITEM (ih)) {
    struct key * next_key;

    /* previous item belongs to another object or is a stat data, check next item */
    PATH_LAST_POSITION (path) ++;
    if (PATH_LAST_POSITION (path) < B_NR_ITEMS (PATH_PLAST_BUFFER (path)))
      /* next key is in the same node */
      next_key = B_N_PKEY (PATH_PLAST_BUFFER (path), PATH_LAST_POSITION (path));
    else
      next_key = uget_rkey (path);
    if (next_key == 0 || comp_short_keys ((unsigned long *)next_key, (unsigned long *)key) != KEYS_IDENTICAL) {
      /* there is no any part of such file in the tree */
      path->pos_in_item = 0;
      return FILE_NOT_FOUND;
    }

    if (KEY_IS_DIRECTORY_KEY (next_key)) {
      reiserfs_warning ("\ndirectory with the same key %d found\n", next_key);
      return DIRECTORY_FOUND;
    }
    /* next item is the part of this file */
    path->pos_in_item = 0;
    return BYTE_NOT_FOUND;
  }

  if (I_IS_DIRECTORY_ITEM (ih)) {
    return DIRECTORY_FOUND;
  }
  if (I_IS_STAT_DATA_ITEM (ih)) {
    PATH_LAST_POSITION (path) ++;
    return FILE_NOT_FOUND;
  }

  /* previous item is part of desired file */
  if (I_K_KEY_IN_ITEM (ih, key, bh->b_size)) {
    path->pos_in_item = key->k_offset - ih->ih_key.k_offset;
    if ( I_IS_INDIRECT_ITEM (ih) )
      path->pos_in_item /= bh->b_size;
    return BYTE_FOUND;
  }

  path->pos_in_item = I_IS_INDIRECT_ITEM (ih) ? I_UNFM_NUM (ih) : ih->ih_item_len;
  return BYTE_NOT_FOUND;
}


