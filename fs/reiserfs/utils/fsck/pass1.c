/*
 * Copyright 1996-1999 Hans Reiser
 */
#include "fsck.h"
#include <stdlib.h>
#include "reiserfs.h"


void build_the_tree (void);
int is_item_accessed (struct item_head * ih);
void mark_item_unaccessed (struct item_head * ih);
void mark_item_accessed (struct item_head * ih, struct buffer_head * bh);


/* allocates buffer head and copy buffer content */
static struct buffer_head * make_buffer (int dev, int blocknr, int size, char * data)
{
  struct buffer_head * bh;

  bh = getblk (dev, blocknr, size);
  if (buffer_uptodate (bh))
    die ("make_buffer: uptodate buffer found");
  memcpy (bh->b_data, data, size);
  set_bit (BH_Uptodate, (char *)&bh->b_state);
  return bh;
}


static void find_a_key (struct key * key, struct buffer_head * bh)
{
  int i;
  struct item_head * ih;
  struct reiserfs_de_head * deh;

  for (i = 0; i < B_NR_ITEMS (bh); i ++) {
    ih = B_N_PITEM_HEAD (bh, i);
    if (comp_short_keys (key, &(ih->ih_key)))
      continue;

    if (key->k_offset == MAX_KEY_OFFSET && key->k_uniqueness == MAX_KEY_OFFSET) {
      /* look for all items of this file */
      reiserfs_warning ("\nblock %d contains item of this file %k (item %d)\n", bh->b_blocknr, key, i);
      return;
    }

    /* key is specified precisely */
    if (!comp_keys (key, &(ih->ih_key))) {
      reiserfs_warning ("\nblock %d contains key %k (item %d)\n", bh->b_blocknr, key, i);
      return;
    }
    if (KEY_IS_DIRECTORY_KEY (key) && I_IS_DIRECTORY_ITEM (ih)) {
      int j;
      
      deh = B_I_DEH (bh, ih);
      for (j = 0; j < I_ENTRY_COUNT (ih); j ++, deh ++)
	if (deh->deh_offset == key->k_offset) {
	  reiserfs_warning ("\nblock %d contains key %k (item_pos %d, pos_in_item %d)\n", bh->b_blocknr, key, i, j);
	  return;
	}
    }
  }
}


/* analyse contents of indirect items. If it points to used blocks or
   to uninsertable node, which has to be inserted by items - we free
   those slots (putting 0-s), if not - mark pointed blocks as used */
static void handle_indirect_items (struct buffer_head * bh)
{
  int i, j;
  struct item_head * ih;

  for (i = 0, ih = B_N_PITEM_HEAD (bh, 0); i < B_NR_ITEMS (bh); i ++, ih ++) {
    if (I_IS_INDIRECT_ITEM(ih)) {
      __u32 * unp;
      
      /* check each pointer to unformatted node, if it is in the tree already, put 0 here */
      unp = (__u32 *)B_I_PITEM (bh, ih);
      for (j = 0; j < ih->ih_item_len / UNFM_P_SIZE; j ++) {
	if (unp[j] >= SB_BLOCK_COUNT (&g_sb) || /* invalid data block */
	    !was_block_used (unp[j]) ||	/* block is marked free in on
					   disk bitmap */
	    is_block_used (unp[j]) ||	/* that is either it looked
					   like leaf or other indirect
					   item contains this pointer
					   already */
	    is_block_uninsertable (unp[j])) {	/* block contains leaf
						   node, its insertion
						   has been postponed */
	  unp[j] = 0;
	  mark_buffer_dirty (bh, 0);
	  continue;
	}
	/* ok, mark that block is in tree and that it is unformatted node */
	mark_block_used (unp[j]);

	/* this is for check only */
	mark_block_unformatted (unp[j]);
      }
    }
  }
}

int g_unaccessed_items = 0;

int is_item_accessed (struct item_head * ih)
{
  return (ih->ih_reserved == 0) ? 1 : 0;
}


void mark_item_unaccessed (struct item_head * ih)
{
  g_unaccessed_items ++;
  ih->ih_reserved = MAX_US_INT;
}


void mark_item_accessed (struct item_head * ih, struct buffer_head * bh)
{
  g_unaccessed_items --;
  ih->ih_reserved = 0;
  mark_buffer_dirty (bh, 0);
}


/* used when leaf is inserted into tree by pointer
   1. set sd_nlinks to 0 in all stat data items
   2. mark all items as unaccessed
   */
static void reset_nlinks (struct buffer_head * bh)
{ 
  int i;
  struct item_head * ih;

  ih = B_N_PITEM_HEAD (bh, 0);
  for (i = 0; i < B_NR_ITEMS (bh); i ++, ih ++) {
    mark_item_unaccessed (ih);
    if (I_IS_STAT_DATA_ITEM (ih)) {
      add_event (STAT_DATA_ITEMS);
      B_I_STAT_DATA (bh, ih)->sd_nlink = 0;
    }
  }

  mark_buffer_dirty (bh, 0);
}


/* ubitmap.c */
extern int from_journal;

static void insert_pointer (struct buffer_head * bh, struct path * path)
{
  struct tree_balance tb;
  struct item_head * ih;
  char * body;
  int memmode;
  int zeros_number;
  int retval;

  init_tb_struct (&tb, &g_sb, path, 0x7fff);
  tb.preserve_mode = NOTHING_SPECIAL;

  /* fix_nodes & do_balance must work for internal nodes only */
  ih = 0;
  retval = fix_nodes (0, M_INTERNAL, &tb, PATH_LAST_POSITION (path), ih);
  if (retval != CARRY_ON)
    die ("insert_pointer: no free space on device (retval == %d, used blocks from journal %d",
	 retval, from_journal);

  /* child_pos: we insert after position child_pos: this feature of the insert_child */
  /* there is special case: we insert pointer after
     (-1)-st key (before 0-th key) in the parent */
  if (PATH_LAST_POSITION (path) == 0 && path->pos_in_item == 0)
    PATH_H_B_ITEM_ORDER (path, 0) = -1;
  else {
    if (PATH_H_PPARENT (path, 0) == 0)
      PATH_H_B_ITEM_ORDER (path, 0) = 0;
/*    PATH_H_B_ITEM_ORDER (path, 0) = PATH_H_PPARENT (path, 0) ? PATH_H_B_ITEM_ORDER (path, 0) : 0;*/
  }

  ih = 0;
  body = (char *)bh;
  memmode = 0;
  zeros_number = 0;

  do_balance (0, &tb, path->pos_in_item, ih, body, M_INTERNAL, memmode, zeros_number);

  /* mark as used block itself and pointers to unformatted nodes */
  mark_block_used (bh->b_blocknr);

  /* this is for check only */
  mark_block_formatted (bh->b_blocknr);
  reset_nlinks (bh);
  handle_indirect_items (bh);

  /* statistic */
  add_event (GOOD_LEAVES);

}


/* return 1 if left and right can be joined. 0 otherwise */
int balance_condition_fails (struct buffer_head * left, struct buffer_head * right)
{
  if (B_FREE_SPACE (left) >= B_CHILD_SIZE (right) - 
      (are_items_mergeable (B_N_PITEM_HEAD (left, B_NR_ITEMS (left) - 1), B_N_PITEM_HEAD (right, 0), left->b_size) ? IH_SIZE : 0))
    return 1;
  return 0;
}


/* return 1 if new can be joined with last node on the path or with
   its right neighbor, 0 otherwise */
int balance_condition_2_fails (struct buffer_head * new, struct path * path)
{
  struct buffer_head * bh;
  struct key * right_dkey;
  int pos, used_space;
  struct path path_to_right_neighbor;

  bh = PATH_PLAST_BUFFER (path);


  if (balance_condition_fails (bh, new))
    /* new node can be joined with last buffer on the path */
    return 1;

  /* new node can not be joined with its left neighbor */

  right_dkey = uget_rkey (path);
  if (right_dkey == 0)
    /* there is no right neighbor */
    return 0;
  
  pos = PATH_H_POSITION (path, 1);
  if (pos == B_NR_ITEMS (bh = PATH_H_PBUFFER (path, 1))) {
    /* we have to read parent of right neighbor. For simplicity we
       call search_by_key, which will read right neighbor as well */
    init_path (&path_to_right_neighbor);
    if (usearch_by_key (&g_sb, right_dkey, &path_to_right_neighbor, 0, 
			DISK_LEAF_NODE_LEVEL, 0, comp_keys) != ITEM_FOUND)
      die ("get_right_neighbor_free_space: invalid right delimiting key");
    used_space =  B_CHILD_SIZE (PATH_PLAST_BUFFER (&path_to_right_neighbor));
    pathrelse (&path_to_right_neighbor);
  }
  else
    used_space = B_N_CHILD (bh, pos + 1)->dc_size;
  

  if (B_FREE_SPACE (new) >= used_space - 
      (are_items_mergeable (B_N_PITEM_HEAD (new, B_NR_ITEMS (new) - 1), (struct item_head *)right_dkey, new->b_size) ? IH_SIZE : 0))
    return 1;
  
  return 0;
}



/* inserts pointer to leaf into tree if possible. If not, marks node as uninsrtable */
static void try_to_insert_pointer_to_leaf (struct buffer_head * new_bh)
{
  struct path path;
  struct buffer_head * bh;			/* last path buffer */
  struct key * first_bh_key, last_bh_key;	/* first and last keys of new buffer */
  struct key last_path_buffer_last_key, * right_dkey;
  int ret_value;

  path.path_length = ILLEGAL_PATH_ELEMENT_OFFSET;

  if (is_block_used (new_bh->b_blocknr))
    /* block could get into tree already if its number was used by
       some indirect item */
    goto cannot_insert;

  first_bh_key = B_N_PKEY (new_bh, 0);

  /* try to find place in the tree for the first key of the coming node */
/*
  if (KEY_IS_DIRECTORY_KEY (first_bh_key))
    ret_value = search_by_entry_key (&g_sb, first_bh_key, &path);
  else if (KEY_IS_STAT_DATA_KEY (first_bh_key)) {
    ret_value = reiserfsck_search_by_key (&g_sb, first_bh_key, &path, comp_keys);
    path.pos_in_item = 0;
  } else
    ret_value = search_by_position (&g_sb, first_bh_key, &path);
  if (ret_value == KEY_FOUND || ret_value == ENTRY_FOUND || ret_value == BYTE_FOUND)
    goto cannot_insert;
*/
  ret_value = usearch_by_key (&g_sb, first_bh_key, &path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys);
  if (ret_value == KEY_FOUND)
    goto cannot_insert;


  /* get max key in the new node */
  get_max_buffer_key (new_bh, &last_bh_key);
  bh = PATH_PLAST_BUFFER (&path);
  if (comp_keys ((unsigned long *)B_N_PKEY (bh, 0), (unsigned long *)&last_bh_key) == FIRST_GREATER) {
    /* new buffer falls before the leftmost leaf */
    if (balance_condition_fails (new_bh, bh))
      goto cannot_insert;

    if (uget_lkey (&path) != 0 || PATH_LAST_POSITION (&path) != 0)
      die ("try_to_insert_pointer_to_leaf: bad search result");

    path.pos_in_item = 0;
    goto insert;
  }

  /* get max key of buffer, that is in tree */
  get_max_buffer_key (bh, &last_path_buffer_last_key);
  if (comp_keys (&last_path_buffer_last_key, first_bh_key) != SECOND_GREATER)
    /* first key of new buffer falls in the middle of node that is in tree */
    goto cannot_insert;

  right_dkey = uget_rkey (&path);
  if (right_dkey && comp_keys (right_dkey, &last_bh_key) != FIRST_GREATER) {
    goto cannot_insert;
  }

  if (balance_condition_2_fails (new_bh, &path))
    goto cannot_insert;

insert:
  insert_pointer (new_bh, &path);

  goto out;

cannot_insert:
  /* statistic */
  add_event (UNINSERTABLE_LEAVES);
  mark_block_uninsertable (new_bh->b_blocknr);

out:
  pathrelse (&path);
  brelse (new_bh);
  return;
}




static int tree_is_empty (void)
{
  return (SB_ROOT_BLOCK (&g_sb) == ~0) ? 1 : 0;
}


static void make_single_leaf_tree (struct buffer_head * bh)
{
  /* tree is empty, make tree root */
  SB_ROOT_BLOCK (&g_sb) = bh->b_blocknr;
  SB_TREE_HEIGHT (&g_sb) = 2;

  mark_block_used (bh->b_blocknr);

  /* this is for check only */
  mark_block_formatted (bh->b_blocknr);
  
  /* set stat data nlinks fields to 0, mark all items as unaccessed, analyse contents of indirect
     items */
  reset_nlinks (bh);
  handle_indirect_items (bh);

  /* statistic */
  add_event (GOOD_LEAVES);

  brelse (bh);
}

/*
int g_found_internals, g_freed_internals, g_allocated, g_freed, g_new_internals, g_really_freed;
*/

/* reads the device by set of 8 blocks, takes leaves and tries to
   insert them into tree */

void build_the_tree (void)
{
    int i, j, k;
    struct buffer_head * bbh, * bh;
    __u32 handled_blocks = 0;
    struct si * saved_items = 0;

    if ( opt_fsck == 0 )
	fprintf (stderr, "Pass 1 - ");

    for (i = 0; i < SB_BMAP_NR (&g_sb); i ++)
	for (j = 0; j < g_sb.s_blocksize; j ++) {
	    /* make sure, that we are not out of the device */
	    if (i * g_sb.s_blocksize * 8 + j * 8 == SB_BLOCK_COUNT (&g_sb))
		goto out_of_bitmap;

	    if (i * g_sb.s_blocksize * 8 + j * 8 + 8 > SB_BLOCK_COUNT (&g_sb))
		die ("build_the_tree: Out of bitmap");

	    if (SB_AP_BITMAP (&g_sb)[i]->b_data[j] == 0) {
		/* all blocks are free */
		if (opt_what_to_scan == SCAN_USED_PART)
		    continue;
	    }

	    bbh = bread (g_sb.s_dev, i * g_sb.s_blocksize + j, g_sb.s_blocksize * 8);
	    for (k = 0; k < 8; k ++) {
		unsigned long block;

		if ((SB_AP_BITMAP (&g_sb)[i]->b_data[j] & (1 << k)) == 0) {
		    /* k-th block is free */
		    if (opt_what_to_scan == SCAN_USED_PART)
			continue;
		}

		if ( opt_fsck == 0 )
		    print_how_far (&handled_blocks, g_blocks_to_read);

		block = i * g_sb.s_blocksize * 8 + j * 8 + k;
		if (not_data_block (&g_sb, block)) {
		    /* skip not data area of the filesystem (journal,
                       bitmaps, reserved space) */
		    continue;
		}

		if (not_formatted_node (bbh->b_data + k * g_sb.s_blocksize, g_sb.s_blocksize))
		    continue;
		if (is_internal_node (bbh->b_data + k * g_sb.s_blocksize) == 1 && opt_fsck_mode == FSCK_REBUILD) {
/*		    g_found_internals ++;*/
		    if (!is_block_used (block)) {
/*			g_freed_internals ++;*/
			reiserfs_free_internal_block (&g_sb, i * g_sb.s_blocksize * 8 + j * 8 + k);
		    } else
			/* block is used in new tree already. There was an
			   indirect item, pointing to it. We keep information
			   about it for check only */
			/*mark_formatted_pointed_by_indirect (block)*/;

		    continue;
		}
	  
		/* leaf node found */
		bh = make_buffer (g_sb.s_dev, block, g_sb.s_blocksize, bbh->b_data + k * g_sb.s_blocksize);

		/* */
		if (opt_fsck_mode == FSCK_FIND_ITEM) {
		    find_a_key (&key_to_find, bh);
		    brelse (bh);
		    continue;
		}

	  
		if (is_block_used (block)) {
		    /* block is used in new tree already. There was an indirect
		       item, pointing to it. We keep information about it for
		       check only */
		    /*	  mark_formatted_pointed_by_indirect (block);*/

		    add_event (LEAVES_USED_BY_INDIRECT_ITEMS);
		    /* Rather than try to find UNP to this block we save its
		       items and will put them into tree at the end of pass 1 */
		    for_all_items_in_node (save_item, &saved_items, bh);
		    brelse (bh);
		    continue;
		}

		if (is_leaf_bad (bh)) {
		    /* leaf is bad: directory item structure corrupted, or something else */
		    /*	  mark_formatted_pointed_by_indirect (block);*/
		    if (opt_verbose)
			reiserfs_warning ("\nbuild_the_tree: bad leaf encountered: %lu\n", bh->b_blocknr);
		    add_event (LEAVES_USED_BY_INDIRECT_ITEMS);
		    /* Save good items only to put them into tree at the end of this pass */
		    for_all_items_in_node (save_item, &saved_items, bh);
		    brelse (bh);
		    continue;
		}

		if (tree_is_empty () == 1) {
		    make_single_leaf_tree (bh);
		    continue;
		}

		/* if the leaf node can not be inserted into tree by pointer,
		   we postpone its insertion at the end of the pass 1 */
		try_to_insert_pointer_to_leaf (bh);

/*
		if (opt_check == 1)
		    reiserfsck_check_pass1 ();
*/
	    }

	    bforget (bbh);
	}


 out_of_bitmap:

/*
    check_bitmaps (&g_sb);

    printf ("bitmap scanning completed: allocated %d, freed %d *really freed %d, new internals %d, found internals %d (freed %d)\n",
	    g_allocated, g_freed, g_really_freed, g_new_internals, g_found_internals, g_freed_internals);fflush (stdout);
*/

    if (opt_fsck_mode == FSCK_FIND_ITEM)
	return;

    /* this checks what has been built (if -c option is set) */
/*    reiserfsck_check_pass1 ();*/

    /* put saved items into tree. These items were in leaves, those
       could not be inserted into tree because some indirect items point
       to those leaves. Rather than lookup for corresponding unfm
       pointers in the tree, we save items of those leaves and put them
       into tree separately */
    if ( opt_fsck == 0 )
	printf ("\nPass 1a - ");
    put_saved_items_into_tree (saved_items);
    if ( opt_fsck == 0 )
	printf ("done\n");

    /* end of pass 1 */
    if ( opt_fsck == 0 )
	printf ("\n");

    /* this works only if -c specified  */
    reiserfsck_check_pass1 ();


    if (opt_stop_point == STOP_AFTER_PASS1)
	return;

    /* pass 2 */
    take_bad_blocks_put_into_tree ();

    reiserfsck_check_pass1 ();

}










