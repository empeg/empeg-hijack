/*
 * Copyright 1996-1999 Hans Reiser
 */
#include "fsck.h"

#include "reiserfs.h"

void for_all_items_in_node (action_on_item_t action, struct si ** si, struct buffer_head * bh)
{
  int i;
  struct item_head * ih;

  for (i = 0, ih = B_N_PITEM_HEAD (bh, 0); i < B_NR_ITEMS (bh); i ++, ih ++)
    action (si, ih, B_I_PITEM (bh,ih));
#if 0
  int j;

  for (i = B_NR_ITEMS (bh) / 2, j = i + 1; ; i --, j ++) {
    if (i >= 0) {
      ih = B_N_PITEM_HEAD (bh, i);
      action (si, ih, B_I_PITEM (bh,ih));
    }
    if (j < B_NR_ITEMS (bh)) {
      ih = B_N_PITEM_HEAD (bh, j);
      action (si, ih, B_I_PITEM (bh,ih));
    }

/*    check_buffer_queues ();*/

    if (i <= 0 && j >= B_NR_ITEMS (bh) - 1)
      break;
  }
#endif
}


/* insert sd item if it does not exist, overwrite it otherwise */
static void put_sd_item_into_tree (struct item_head * comingih, char * item)
{
  struct item_head ih;
  struct path path;
  struct buffer_head * path_bh;
  int path_item_num;
  struct stat_data * psd;

  copy_key (&(ih.ih_key), &(comingih->ih_key));
  if (usearch_by_key (&g_sb, &(ih.ih_key), &path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys) == ITEM_FOUND) {
    /* overwrite stat data in the tree */
    path_bh = PATH_PLAST_BUFFER (&path);
    path_item_num = PATH_LAST_POSITION (&path);
    psd = B_N_STAT_DATA (path_bh, path_item_num);
    if (psd->sd_nlink != 0)
      die ("put_sd_item_into_tree: all stat data in the tree (at this moment) must have nllinks == 0 (not %d)",
	   psd->sd_nlink);
    if (psd->sd_mtime > ((struct stat_data *)item)->sd_mtime) {
      /* new sd is newer than the found one */
      memcpy (psd, item, SD_SIZE);
      psd->sd_nlink = 0;
      psd->sd_first_direct_byte = NO_BYTES_IN_DIRECT_ITEM;
      mark_buffer_dirty (PATH_PLAST_BUFFER (&path), 0);
    }
    pathrelse (&path);
  } else {
    struct stat_data sd;

    ih.ih_item_len = SD_SIZE;
    ih.u.ih_free_space = MAX_US_INT;
    mark_item_unaccessed (&ih);
    memcpy (&sd, item, SD_SIZE);
    sd.sd_nlink = 0;
    sd.sd_first_direct_byte = NO_BYTES_IN_DIRECT_ITEM;
    reiserfsck_insert_item (&path, &ih, (const char *)&sd);

    add_event (STAT_DATA_ITEMS);
  }
}


/* Keyed 32-bit hash function using TEA in a Davis-Meyer function */
/*
static unsigned long get_third_component (char * name, int len)
{
  if (!len || (len == 1 && name[0] == '.'))
    return DOT_OFFSET;
  if (len == 2 && name[0] == '.' && name[1] == '.')
    return DOT_DOT_OFFSET;
  return keyed_hash (name, len);
}
*/

static int reiserfsck_find_entry (struct key * key, struct reiserfs_de_head * deh, struct path * path)
{
  struct key entry_key;

  copy_key (&entry_key, key);
  entry_key.k_offset = deh->deh_offset;
  entry_key.k_uniqueness = DIRENTRY_UNIQUENESS;

  return usearch_by_entry_key (&g_sb, &entry_key, path);
}


/* this tries to put each item entry to the tree, if there is no items of
   the directory, insert item containing 1 entry */
static void put_directory_item_into_tree (struct item_head * comingih, char * item)
{
  /*  struct item_head * ih;*/
  struct reiserfs_de_head * deh;
  int i, retval;
  struct path path;
  int size;
  char * buf, * entry;
  struct item_head tmpih;

  /*ih = B_N_PITEM_HEAD (bh, item_num);*/
  deh = (struct reiserfs_de_head *)item;/*B_I_DEH (bh, comingih);*/

  for (i = 0; i < I_ENTRY_COUNT (comingih); i ++, deh ++) {
    entry = item + deh->deh_location;
    retval = reiserfsck_find_entry (&(comingih->ih_key), deh, &path);
    switch (retval) {
    case ENTRY_FOUND:
      pathrelse (&path);
      break;

    case ENTRY_NOT_FOUND:
      /* paste_into_item accepts entry to paste as buffer, beginning
         with entry header and body, that follows it */
      buf = getmem (size = I_DEH_N_ENTRY_LENGTH (comingih, deh, i) + DEH_SIZE);
      memcpy (buf, deh, DEH_SIZE);
      ((struct reiserfs_de_head *)buf)->deh_location = 0;
      memcpy (buf + DEH_SIZE, entry, size - DEH_SIZE);

      reiserfsck_paste_into_item (&path, buf, size);

      freemem (buf);
      break;

    case DIRECTORY_NOT_FOUND:
      buf = getmem (size = I_DEH_N_ENTRY_LENGTH (comingih, deh, i) + DEH_SIZE);
      memcpy (buf, deh, DEH_SIZE);
      ((struct reiserfs_de_head *)buf)->deh_location = DEH_SIZE;
      memcpy (buf + DEH_SIZE, entry, size - DEH_SIZE);
      copy_key (&(tmpih.ih_key), &(comingih->ih_key));
      tmpih.ih_item_len = size;
      tmpih.u.ih_entry_count = 1;
      mark_item_unaccessed (&tmpih);
      
      reiserfsck_insert_item (&path, &tmpih, buf);

      freemem (buf);
      break;

    case REGULAR_FILE_FOUND:
      /* this should never happen. */
      goto end;
    }

    /*&&&&&&&&&&&&&&&&&*/
/*    reiserfsck_check_pass1 ();*/
    /*&&&&&&&&&&&&&&&&&*/
  }
 end:

}


/* If item is item of regular file (direct or indirect item) - this
   file is in tree (with first byte) - write to it. If this file is in
   tree (without first byte) - delete what we have in tree, create
   file again keeping what we already had in tree this file is not in
   tree - create hole at the beginning of file if necessary and write
   to file */
void put_regular_file_item_into_tree (struct item_head * ih, char * item)
{
  reiserfsck_file_write (ih, item);
}


void insert_item_separately (struct si ** si, struct item_head * ih, char * item)
{
  if (I_IS_STAT_DATA_ITEM (ih)) {
    put_sd_item_into_tree (ih, item);
  } else if (I_IS_DIRECTORY_ITEM (ih)) {
    put_directory_item_into_tree (ih, item);
  } else {
    put_regular_file_item_into_tree (ih, item);
  }

  
}


/* uninsertable blocks are marked by 0s in
   g_uninsertable_leaf_bitmap during the pass 1. They still must be not in the tree */
void take_bad_blocks_put_into_tree (void)
{
  struct buffer_head * bh;
  int i, j;
  __u32 bb_counter = 0;

  if ( opt_fsck == 0 )
    fprintf (stderr, "Pass 2 - ");


  for (i = 0; i < SB_BMAP_NR (&g_sb); i ++) {
    j = find_first_zero_bit (g_uninsertable_leaf_bitmap[i], g_sb.s_blocksize * 8);
    while (j < g_sb.s_blocksize * 8) {
      bh = bread (g_sb.s_dev, i * g_sb.s_blocksize * 8 + j, g_sb.s_blocksize);

      if (is_block_used (bh->b_blocknr))
	die ("take_bad_blocks_put_into_tree: block %d can not be in tree", bh->b_blocknr);
      /* this must be leaf */
      if (not_formatted_node (bh->b_data, g_sb.s_blocksize) || is_internal_node (bh->b_data)) {
	reiserfs_panic (0, "take_bad_blocks_put_into_tree: buffer (%b %z) must contain leaf", bh, bh);
      }

      for_all_items_in_node (insert_item_separately, 0, bh);

      if ( opt_fsck == 0 )
	print_how_far (&bb_counter, get_event (UNINSERTABLE_LEAVES));

      brelse (bh);

      j = find_next_zero_bit (g_uninsertable_leaf_bitmap[i], g_sb.s_blocksize * 8, j + 1);
    }
  }

  if (bb_counter != get_event (UNINSERTABLE_LEAVES))
    die ("take_bad_blocks_put_into_tree: found bad block %d, must be %d", 
	 bb_counter, get_event (UNINSERTABLE_LEAVES));

  if ( opt_fsck == 0 )
    printf ("\n");
}
