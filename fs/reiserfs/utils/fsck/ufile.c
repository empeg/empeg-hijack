/*
 * Copyright 1996, 1997, 1998 Hans Reiser
 */
#include "fsck.h"

#if 0
static int is_bad_sd (struct item_head * ih, char * item)
{
  struct stat_data * sd = (struct stat_data *)item;

  if (!S_ISDIR (sd->sd_mode) && !S_ISREG(sd->sd_mode) &&
      !S_ISCHR (sd->sd_mode) && !S_ISBLK(sd->sd_mode) &&
      !S_ISLNK (sd->sd_mode)) {
    reiserfs_warning ("is_bad_sd: \
stat data item (%h) has sd_mode 0%o. Skipped\n", ih, sd->sd_mode);
    return 1;
  }
  if ((sd->sd_first_direct_byte != NO_BYTES_IN_DIRECT_ITEM &&
       sd->sd_first_direct_byte >= sd->sd_size) ||
      sd->sd_size > MAX_INT) {
    reiserfs_warning ("is_bad_sd: \
stat data item (%h) has sd_size %d, first direct byte %d\n", ih, sd->sd_size,
		      sd->sd_first_direct_byte);
    return 1;
  }
  if (sd->sd_nlink > 100) {
    reiserfs_warning ("is_bad_sd: \
stat data item (%h) has sd_nlink %d\n", sd->sd_nlink);
    return 1;
  }
  return 0;
}


static int is_bad_directory (struct item_head * ih, char * item)
{
  int i;
  int namelen;
  struct reiserfs_de_head * deh = (struct reiserfs_de_head *)item;
  __u32 prev_offset = 0;
  __u16 prev_location = 0xffff;

  for (i = 0; i < I_ENTRY_COUNT (ih); i ++) {
    namelen = I_DEH_N_ENTRY_FILE_NAME_LENGTH (ih, deh + i, i);
    if (namelen > REISERFS_MAX_NAME_LEN (g_sb.s_blocksize)) {
      reiserfs_warning ("is_bad_directory: dir item %h has too long name (%d)\n", ih, namelen);
      return 1;
    }
    if (deh[i].deh_offset <= prev_offset) {
      reiserfs_warning ("is_bad_directory: dir item %h has invalid header array \
(offsets: prev %u, %d-th cur %u)\n", ih, prev_offset, i, deh[i].deh_offset);
      return 1;
    }
    prev_offset = deh[i].deh_offset;

    if (deh[i].deh_location >= prev_location) {
      reiserfs_warning ("is_bad_directory: dir item %h has invalid header array \
(locations: prev %u, %d-th cur %u)\n", ih, prev_location, i, deh[i].deh_location);
      return 1;
    }
  }

  return 0;
}


/* change incorrect block adresses by 0. Do not consider such item as incorrect */
static int is_bad_indirect (struct item_head * ih, char * item)
{
  int i;

  for (i = 0; i < I_UNFM_NUM (ih); i ++) {
    __u32 * ind = (__u32 *)item;

    if (ind[i] >= SB_BLOCK_COUNT (&g_sb)) {
      /*reiserfs_warning ("is_bad_indirect: block address (%lu) in indirect item. Super block block count == %u\n",
	      ind[i], SB_BLOCK_COUNT (&g_sb));*/
      ind[i] = 0;
      continue;
    }
    if (is_block_used (ind[i])) {
      ind[i] = 0;
      continue;
    }
  }
  return 0;
}


int is_bad_item (struct item_head * ih, char * item)
{
  if (I_IS_STAT_DATA_ITEM (ih))
    return is_bad_sd (ih, item);

  if (I_IS_DIRECTORY_ITEM (ih))
    return is_bad_directory (ih, item);

  if (I_IS_INDIRECT_ITEM (ih))
    return is_bad_indirect (ih, item);

  return 0;
}
#endif /* 0 */

int is_bad_item (struct item_head *, char *, int, int);

/* append item to end of list. Set head if it is 0. For indirect item
   set wrong unformatted node pointers to 0 */
void save_item (struct si ** head, struct item_head * ih, char * item)
{
    struct si * si, * cur;
    int i;

    if (is_bad_item (ih, item, g_sb.s_blocksize, g_sb.s_dev)) {
	return;
    }

    if (I_IS_INDIRECT_ITEM (ih))
	for (i = 0; i < I_UNFM_NUM (ih); i ++) {
	    __u32 * ind = (__u32 *)item;

	    if (ind[i] >= SB_BLOCK_COUNT (&g_sb) ||
		!was_block_used (ind[i]) ||
		is_block_used (ind[i]) ||
		is_block_uninsertable (ind[i])) {
		ind[i] = 0;
		continue;
	    }
	}

    si = getmem (sizeof (*si));
    si->si_dnm_data = getmem (ih->ih_item_len);
    memcpy (&(si->si_ih), ih, IH_SIZE);
    memcpy (si->si_dnm_data, item, ih->ih_item_len);


    if (*head == 0)
	*head = si;
    else {
	cur = *head;
	while (cur->si_next)
	    cur = cur->si_next;
	cur->si_next = si;
    }
    return;
}


/* this item is in tree. All unformatted pointer are correct. Do not
   check them */
static void save_item_2 (struct si ** head, struct item_head * ih, char * item)
{
    struct si * si, * cur;

    if (is_bad_item (ih, item, g_sb.s_blocksize, g_sb.s_dev)) {
	return;
    }

    si = getmem (sizeof (*si));
    si->si_dnm_data = getmem (ih->ih_item_len);
    memcpy (&(si->si_ih), ih, IH_SIZE);
    memcpy (si->si_dnm_data, item, ih->ih_item_len);


    if (*head == 0)
	*head = si;
    else {
	cur = *head;
	while (cur->si_next)
	    cur = cur->si_next;
	cur->si_next = si;
    }
    return;
}



static struct si * save_and_delete_file_item (struct si * si, struct path * path)
{
    struct buffer_head * bh = PATH_PLAST_BUFFER (path);
    struct item_head * ih = PATH_PITEM_HEAD (path);

    save_item_2 (&si, ih, B_I_PITEM (bh, ih));

    reiserfsck_delete_item (path);
    return si;
}


static struct si * remove_saved_item (struct si * si)
{
  struct si * tmp = si->si_next;

  freemem (si->si_dnm_data);
  freemem (si);
  return tmp;
}


void put_saved_items_into_tree (struct si * si)
{
  while (si) {
    insert_item_separately (&si, &(si->si_ih), si->si_dnm_data);
/*    reiserfsck_file_write (&(si->si_ih), si->si_dnm_data);*/
    si = remove_saved_item (si);
  }
}


/* path points to an item or behind last item of the node */
/*
static int next_item_of_other_object (struct key * key, struct path * path)
{
  struct key * next_key;

  if (PATH_LAST_POSITION (path) < B_NR_ITEMS (PATH_PLAST_BUFFER (path)))
    next_key = B_N_PKEY (PATH_PLAST_BUFFER (path), PATH_LAST_POSITION (path));
  else
    next_key = get_right_dkey (path);

  if (next_key == 0 || comp_short_keys (key, next_key) != KEYS_IDENTICAL)
    return YES;
  return NO;
}
*/


static int do_items_have_the_same_type (struct key * key1, struct key * key2)
{
  return (key1->k_uniqueness == key2->k_uniqueness) ? 1 : 0;
}

static int are_items_in_the_same_node (struct path * path)
{
  return (PATH_LAST_POSITION (path) < B_NR_ITEMS (PATH_PLAST_BUFFER (path)) - 1) ? 1 : 0;
}


static struct key * get_next_key (struct path * path)
{
  if (PATH_LAST_POSITION (path) < B_NR_ITEMS (PATH_PLAST_BUFFER (path)) - 1)
    return B_N_PKEY (PATH_PLAST_BUFFER (path), PATH_LAST_POSITION (path) + 1);
  return uget_rkey (path);
}


/* whether last unfm pointer must be and can be converted to direct item */
static int can_indirect_item_be_converted (struct item_head * ih)
{
  unsigned long file_size = ih->ih_key.k_offset + I_BYTES_NUMBER (ih, g_sb.s_blocksize) - 1;
  unsigned long tail_size = g_sb.s_blocksize - ih->u.ih_free_space;

  if (!STORE_TAIL_IN_UNFM (file_size, tail_size, g_sb.s_blocksize) &&
      I_IS_INDIRECT_ITEM (ih)/* && tail_size <= MAX_DIRECT_ITEM_LEN (g_sb.s_blocksize)*/)
    return 1;
  return 0;
}


int do_make_tails ()
{
  return 1;/*SB_MAKE_TAIL_FLAG (&g_sb) == MAKE_TAILS ? YES : NO;*/
}


static void cut_last_unfm_pointer (struct path * path, struct item_head * ih)
{
  ih->u.ih_free_space = 0;
  if (I_UNFM_NUM (ih) == 1)
    reiserfsck_delete_item (path);
  else
    reiserfsck_cut_from_item (path, -UNFM_P_SIZE);
}


static unsigned long indirect_to_direct (struct path * path)
{
  struct buffer_head * bh = PATH_PLAST_BUFFER (path);
  struct item_head * ih = PATH_PITEM_HEAD (path);
  unsigned long unfm_ptr;
  struct buffer_head * unfm_bh = 0;
  struct item_head ins_ih;
  char * buf;
  int len;
  unsigned long offset;


  add_event (INDIRECT_TO_DIRECT);

  unfm_ptr = B_I_POS_UNFM_POINTER (bh, ih, I_UNFM_NUM (ih) - 1);


  /* direct item to insert */
  ins_ih.ih_key.k_dir_id = ih->ih_key.k_dir_id;
  ins_ih.ih_key.k_objectid = ih->ih_key.k_objectid;
  ins_ih.ih_key.k_offset = ih->ih_key.k_offset + (I_UNFM_NUM (ih) - 1) * bh->b_size;
  offset = ins_ih.ih_key.k_offset;
  ins_ih.ih_key.k_uniqueness = TYPE_DIRECT;
  ins_ih.ih_item_len = g_sb.s_blocksize - ih->u.ih_free_space;
  len = ins_ih.ih_item_len;
  ins_ih.u.ih_free_space = MAX_US_INT;
  ins_ih.ih_reserved = 0;

  /* get buffer filled with 0s */
  buf = getmem (len);
  if (unfm_ptr) {
    unfm_bh = bread (bh->b_dev, unfm_ptr, bh->b_size);
    memcpy (buf, unfm_bh->b_data, ins_ih.ih_item_len);
    brelse (unfm_bh);
  }


  path->pos_in_item = I_UNFM_NUM (ih) - 1;
  cut_last_unfm_pointer (path, ih);

  /* insert direct item */
  if (usearch_by_key (&g_sb, &(ins_ih.ih_key), path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys) == ITEM_FOUND)
    die ("indirect_to_direct: key must be not found");
  reiserfsck_insert_item (path, &ins_ih, (const char *)(buf));


  freemem (buf);
  
  /* put to stat data offset of first byte in direct item */
  return offset;
}


/* when it returns, key->k_offset is offset of the last item of file */
int are_file_items_correct (struct key * key, unsigned long * size, int mark_passed_items, 
			    struct path * path_to_sd, struct stat_data ** sd)
{
    struct path path;
    int retval;
    struct item_head * ih;
    struct key * next_key;
    int symlink = 0;

    if (sd && ((*sd)->sd_mode & S_IFMT) == S_IFLNK)
	symlink = 1;

    *size = 0;
    key->k_offset = 1;
    key->k_uniqueness = TYPE_DIRECT;
    path.path_length = ILLEGAL_PATH_ELEMENT_OFFSET;

    do {
	retval = usearch_by_position (&g_sb, key, &path);
	if (retval == BYTE_FOUND && path.pos_in_item != 0)
	    die ("are_file_items_correct: all bytes we look for must be found at position 0");

	switch (retval) {
	case BYTE_FOUND:/**/
	    ih = PATH_PITEM_HEAD (&path);
	    key->k_uniqueness = ih->ih_key.k_uniqueness;
	    if (mark_passed_items == 1) {
		mark_item_accessed (ih, PATH_PLAST_BUFFER (&path));
	    }
	    next_key = get_next_key (&path);
	    if (next_key == 0 || comp_short_keys (key, next_key) != KEYS_IDENTICAL || 
		(!KEY_IS_INDIRECT_KEY (next_key) && !KEY_IS_DIRECT_KEY (next_key))) {
		/* next item does not exists or is of another object, therefore all items of file are correct */
		*size = key->k_offset + I_BYTES_NUMBER (ih, g_sb.s_blocksize) - 1;

		/* here is a problem: if file system being repaired
                   was full enough, then we should avoid
                   indirect_to_direct conversions. This is because
                   unformatted node we have to free will not get into
                   pool of free blocks, but new direct item is very
                   likely of big size, therefore it may require
                   allocation of new blocks. So, skip it for now */
		if (symlink && I_IS_INDIRECT_ITEM (ih)) {
/*
		if (0 && mark_passed_items == 1 && 
		    do_make_tails () == 1 && can_indirect_item_be_converted (ih) == 1) {
*/	    
		    struct key sd_key;
		    unsigned long first_direct_byte;

		    first_direct_byte = indirect_to_direct (&path);
		    /* we have to research stat data of object after converting */
		    pathrelse (path_to_sd);
		    copy_key (&sd_key, key);
		    sd_key.k_offset = SD_OFFSET;
		    sd_key.k_uniqueness = SD_UNIQUENESS;
		    if (usearch_by_key (&g_sb, &(sd_key), path_to_sd, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys) != ITEM_FOUND)
			die ("are_file_items_correct: stat data not found");
		    *sd = B_N_STAT_DATA (PATH_PLAST_BUFFER (path_to_sd), PATH_LAST_POSITION (path_to_sd));
		    /* last item of the file is direct item */
		    key->k_offset = first_direct_byte;
		    key->k_uniqueness = TYPE_DIRECT;
		} else
		    pathrelse (&path);
		return 1;
	    }
	    /* next item is item of this file */
	    if ((I_IS_INDIRECT_ITEM (ih) &&
		 ih->ih_key.k_offset + g_sb.s_blocksize * I_UNFM_NUM (ih) != next_key->k_offset) ||
		(I_IS_DIRECT_ITEM (ih) && ih->ih_key.k_offset + ih->ih_item_len != next_key->k_offset)) {
		/* next item has incorrect offset (hole or overlapping) */
		*size = key->k_offset + I_BYTES_NUMBER (ih, g_sb.s_blocksize) - 1;
		pathrelse (&path);
		return 0;
	    }
	    if (do_items_have_the_same_type (&(ih->ih_key), next_key) == 1 && are_items_in_the_same_node (&path) == 1) {
		/* two indirect items or two direct items in the same leaf */
		*size = key->k_offset + I_BYTES_NUMBER (ih, g_sb.s_blocksize) - 1;
		pathrelse (&path);
		return 0;
	    }
	    /* items are of different types or are in different nodes */
	    if (ih->ih_key.k_offset + I_BYTES_NUMBER (ih, g_sb.s_blocksize) != next_key->k_offset) {
		/* indirect item free space is not set properly */
		if (!I_IS_INDIRECT_ITEM (ih) || ih->u.ih_free_space == 0)
		    die ("are_file_items_correct: item must be indirect and must have invalid free space (%d)",
			 ih->u.ih_free_space);
	
		ih->u.ih_free_space = 0;
		mark_buffer_dirty (PATH_PLAST_BUFFER (&path), 0);
		if (ih->ih_key.k_offset + I_BYTES_NUMBER (ih, g_sb.s_blocksize) != next_key->k_offset)
		    die ("are_file_items_correct: invalid offset");
	    }
	    /* next item exists */
	    key->k_offset = next_key->k_offset;
	    pathrelse (&path);
	    break;

	case BYTE_NOT_FOUND:
	    if (key->k_offset != 1)
		die ("are_file_items_correct: byte can be not found only when it is first byte of file");
	    pathrelse (&path);
	    return 0;
      
	case FILE_NOT_FOUND:
	    if (key->k_offset != 1)
		die ("are_file_items_correct: there is no items of this file, byte 0 found though");
	    pathrelse (&path);
	    return 1;

	case DIRECTORY_FOUND:
	    pathrelse (&path);
	    return 0;
	}
    } while (1);

    die ("are_file_items_correct: code can not reach here");
    return 0;
}


/* file must have correct sequence of items and tail must be stored in
   unformatted pointer */
static int make_file_writeable (struct item_head * ih)
{
  struct key key;
  struct key * rkey;
  struct path path;
  struct item_head * path_ih;
  struct si * si = 0;
  unsigned long size;
  int mark_passed_items;
  int retval;

  copy_key (&key, &(ih->ih_key));

  if ((retval = are_file_items_correct (&key, &size, mark_passed_items = 0, 0, 0)) == 1)
    /* this file looks good (or there is no any items of it) */
    return 1;

  if (retval == -1) {
    /* there is an object with this key and it is directory */
    return -1;
  }

  /* rewrite file */


  /* look for all items of file, store them and delete */
  key.k_offset = 1;
  while (1) {
    usearch_by_key (&g_sb, &key, &path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys_3);
    if (PATH_LAST_POSITION (&path) == B_NR_ITEMS (PATH_PLAST_BUFFER (&path))) {
      rkey = uget_rkey (&path);
      if (rkey && comp_short_keys (&key, rkey) == KEYS_IDENTICAL) {
	/* file continues in the right neighbor */
	copy_key (&key, rkey);
	pathrelse (&path);
	continue;
      }
      /* there is no more items of file */
      pathrelse (&path);
      break;
    }
    path_ih = PATH_PITEM_HEAD (&path);
    if (comp_short_keys (&key, &(path_ih->ih_key)) != KEYS_IDENTICAL) {
      pathrelse (&path);
      break;
    }
    si = save_and_delete_file_item (si, &path);
  }

  /* put all items back into tree */
  put_saved_items_into_tree (si);

  add_event (REWRITTEN_FILES);

/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/
  copy_key (&key, &(ih->ih_key));
  size = 0;
  if (are_file_items_correct (&key, &size, mark_passed_items = 0, 0, 0) == 0) {
    die ("file still incorrect\n");
  }
/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/

  return 1;

}


/* this inserts __first__ indirect item (having k_offset == 1 and only
   one unfm pointer) into tree */
static int create_first_item_of_file (struct item_head * ih, char * item, struct path * path, int *pos_in_coming_item)
{
  unsigned long unfm_ptr;
  struct buffer_head * unbh;
  struct item_head indih;
  int retval;

  if (ih->ih_key.k_offset > g_sb.s_blocksize) {
    /* insert indirect item containing 0 unfm pointer */
    unfm_ptr = 0;
    indih.u.ih_free_space = 0;
    retval = 0;
  } else {
    if (I_IS_DIRECT_ITEM (ih)) {
      /* copy direct item to new unformatted node. Save information about it */
      
      unbh = reiserfsck_get_new_buffer (PATH_PLAST_BUFFER (path)->b_blocknr);
      unfm_ptr = unbh->b_blocknr;

/* this is for check only */
mark_block_unformatted (unfm_ptr);
      memcpy (unbh->b_data + ih->ih_key.k_offset - 1, item, ih->ih_item_len);

      save_unfm_overwriting (unfm_ptr, ih);

      indih.u.ih_free_space = g_sb.s_blocksize - ih->ih_item_len - (ih->ih_key.k_offset - 1);
      mark_buffer_dirty (unbh, 0);
      mark_buffer_uptodate (unbh, 0);
      brelse (unbh);
      retval = ih->ih_item_len;
    } else {
      /* take first unformatted pointer from an indirect item */
      unfm_ptr = *(unsigned long *)item;/*B_I_POS_UNFM_POINTER (bh, ih, 0);*/
      if (!is_block_used (unfm_ptr) && !is_block_uninsertable (unfm_ptr)) {
	mark_block_used (unfm_ptr);
/* this is for check only */
mark_block_unformatted (unfm_ptr);
      } else {
	unfm_ptr = 0;
      }
      indih.u.ih_free_space = (ih->ih_item_len == UNFM_P_SIZE) ? ih->u.ih_free_space : 0;
      retval = g_sb.s_blocksize - indih.u.ih_free_space;
      (*pos_in_coming_item) ++;
    }
  }
  copy_key (&(indih.ih_key), &(ih->ih_key));
  indih.ih_key.k_offset = 1;
  indih.ih_key.k_uniqueness = TYPE_INDIRECT;
  indih.ih_item_len = UNFM_P_SIZE;
  mark_item_unaccessed (&indih);
  reiserfsck_insert_item (path, &indih, (const char *)&unfm_ptr);
  return retval;
}


/* path points to first part of tail. Function copies file tail into unformatted node and returns
   its block number. If we are going to overwrite direct item then keep free space (keep_free_space
   == YES). Else (we will append file) set free space to 0 */
/* we convert direct item that is on the path to indirect. we need a number of free block for
   unformatted node. reiserfs_new_blocknrs will start from block number returned by this function */
static unsigned long block_to_start (struct path * path)
{
  struct buffer_head * bh;
  struct item_head * ih;

  bh = PATH_PLAST_BUFFER (path);
  ih = PATH_PITEM_HEAD (path);
  if (ih->ih_key.k_offset == 1 || PATH_LAST_POSITION (path) == 0)
    return bh->b_blocknr;

  ih --;
  return (B_I_POS_UNFM_POINTER (bh, ih, I_UNFM_NUM (ih) - 1)) ?: bh->b_blocknr;
}


static void direct2indirect (unsigned long unfm, struct path * path, int keep_free_space)
{
  struct item_head * ih;
  struct key key;
  struct buffer_head * unbh;
  struct unfm_nodeinfo ni;
  int copied = 0;
  
  copy_key (&key, &(PATH_PITEM_HEAD (path)->ih_key));

  if (key.k_offset % g_sb.s_blocksize != 1) {
    /* look for first part of tail */
    pathrelse (path);
    key.k_offset -= (key.k_offset % g_sb.s_blocksize - 1);
    if (usearch_by_key (&g_sb, &key, path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys) != ITEM_FOUND)
      die ("direct2indirect: can not find first part of tail");
  }

  unbh = reiserfsck_get_new_buffer (unfm ?: block_to_start (path));

  /* delete parts of tail coping their contents to new buffer */
  do {
    ih = PATH_PITEM_HEAD (path);
    memcpy (unbh->b_data + copied, B_I_PITEM (PATH_PLAST_BUFFER (path), ih), ih->ih_item_len);

    save_unfm_overwriting (unbh->b_blocknr, ih);

    copied += ih->ih_item_len;
    key.k_offset += ih->ih_item_len;
    reiserfsck_delete_item (path);
  } while (/*reiserfsck_*/usearch_by_key (&g_sb, &key, path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys) == ITEM_FOUND);

  pathrelse (path);

  /* paste or insert pointer to the unformatted node */
  key.k_offset -= copied;
  ni.unfm_nodenum = unbh->b_blocknr;
  ni.unfm_freespace = (keep_free_space == 1) ? (g_sb.s_blocksize - copied) : 0;

/* this is for check only */
mark_block_unformatted (ni.unfm_nodenum);

  if (usearch_by_position (&g_sb, &key, path) == FILE_NOT_FOUND) {
    struct item_head insih;

    copy_key (&(insih.ih_key), &key);
    insih.ih_key.k_uniqueness = TYPE_INDIRECT;
    insih.u.ih_free_space = ni.unfm_freespace;
    mark_item_unaccessed (&insih);
    insih.ih_item_len = UNFM_P_SIZE;
    reiserfsck_insert_item (path, &insih, (const char *)&(ni.unfm_nodenum));
  } else {
    ih = PATH_PITEM_HEAD (path);
    if (!I_IS_INDIRECT_ITEM (ih) || ih->ih_key.k_offset + I_BYTES_NUMBER (ih, g_sb.s_blocksize) != key.k_offset)
      die ("direct2indirect: incorrect item found");
    reiserfsck_paste_into_item (path, (const char *)&ni, UNFM_P_SIZE);
  }

  mark_buffer_dirty (unbh, 0);
  mark_buffer_uptodate (unbh, 0);
  brelse (unbh);

  if (usearch_by_position (&g_sb, &key, path) != BYTE_FOUND || !I_IS_INDIRECT_ITEM (PATH_PITEM_HEAD (path)))
    die ("direct2indirect: position not found");
  return;
}




static int append_to_unformatted_node (struct item_head * comingih, struct item_head * ih, char * item, struct path * path)
{
  struct buffer_head * bh, * unbh;
  int end_of_data = g_sb.s_blocksize - ih->u.ih_free_space;
  int offset = comingih->ih_key.k_offset % g_sb.s_blocksize - 1;
  int zero_number = offset - end_of_data;
  __u32 unfm_ptr;

  /* append to free space of the last unformatted node of indirect item ih */
  if (ih->u.ih_free_space < comingih->ih_item_len)
    die ("reiserfsck_append_file: there is no enough free space in unformatted node");

  bh = PATH_PLAST_BUFFER (path);

  unfm_ptr = B_I_POS_UNFM_POINTER (bh, ih, I_UNFM_NUM (ih) - 1);
  if (unfm_ptr == 0 || unfm_ptr >= SB_BLOCK_COUNT (&g_sb)) {
    unbh = reiserfsck_get_new_buffer (bh->b_blocknr);
    B_I_POS_UNFM_POINTER (bh, ih, I_UNFM_NUM (ih) - 1) = unbh->b_blocknr;
    mark_block_unformatted (unbh->b_blocknr);
    mark_buffer_dirty (bh, 0);
  } else {
    unbh = bread (g_sb.s_dev, unfm_ptr, g_sb.s_blocksize);
    if (!is_block_used (unfm_ptr))
      die ("append_to_unformatted_node:  unused block %d", unfm_ptr);

  }
  memset (unbh->b_data + end_of_data, 0, zero_number);
  memcpy (unbh->b_data + offset, item, comingih->ih_item_len);

  save_unfm_overwriting (unbh->b_blocknr, comingih);

  ih->u.ih_free_space -= (zero_number + comingih->ih_item_len);
  memset (unbh->b_data + offset + comingih->ih_item_len, 0, ih->u.ih_free_space);
  mark_buffer_uptodate (unbh, 0);
  mark_buffer_dirty (unbh, 0);
  brelse (unbh);
  pathrelse (path);
  return comingih->ih_item_len;
}


static void adjust_free_space (struct buffer_head * bh, struct item_head * ih, struct item_head * comingih)
{
  if (I_IS_INDIRECT_ITEM (comingih)) {
    ih->u.ih_free_space = 0;
  } else {
    if (comingih->ih_key.k_offset < ih->ih_key.k_offset + g_sb.s_blocksize * I_UNFM_NUM (ih))
      /* append to the last unformatted node */
      ih->u.ih_free_space = g_sb.s_blocksize - ih->ih_key.k_offset % g_sb.s_blocksize + 1;
    else
      ih->u.ih_free_space = 0;
  }

  mark_buffer_dirty (bh, 0);
}


/* this appends file with one unformatted node pointer (since balancing algorithm limitation). This
   pointer can be 0, or new allocated block or pointer from indirect item that is being inserted
   into tree */
int reiserfsck_append_file (struct item_head * comingih, char * item, int pos, struct path * path)
{
  struct unfm_nodeinfo ni;
  struct buffer_head * unbh;
  int retval;
/*  int keep_free_space;*/
  struct item_head * ih = PATH_PITEM_HEAD (path);

  if (!I_IS_INDIRECT_ITEM (ih))
    die ("reiserfsck_append_file: can not append to non-indirect item");

  if (ih->ih_key.k_offset + I_BYTES_NUMBER (ih, g_sb.s_blocksize) != comingih->ih_key.k_offset) {
    adjust_free_space (PATH_PLAST_BUFFER (path), ih, comingih);
  }

  if (I_IS_DIRECT_ITEM (comingih)) {
    if (comingih->ih_key.k_offset < ih->ih_key.k_offset + g_sb.s_blocksize * I_UNFM_NUM (ih)) {
      /* direct item fits to free space of indirect item */
      return append_to_unformatted_node (comingih, ih, item, path);
    }

    unbh = reiserfsck_get_new_buffer (PATH_PLAST_BUFFER (path)->b_blocknr);
    /* this is for check only */
    mark_block_unformatted (unbh->b_blocknr);
    memcpy (unbh->b_data + comingih->ih_key.k_offset % unbh->b_size - 1, item, comingih->ih_item_len);

    save_unfm_overwriting (unbh->b_blocknr, comingih);

    mark_buffer_dirty (unbh, 0);
    mark_buffer_uptodate (unbh, 0);

    ni.unfm_nodenum = unbh->b_blocknr;
    ni.unfm_freespace = g_sb.s_blocksize - comingih->ih_item_len - (comingih->ih_key.k_offset % unbh->b_size - 1);
    brelse (unbh);
    retval = comingih->ih_item_len;
  } else {
    /* coming item is indirect item */
    if (comingih->ih_key.k_offset + pos * g_sb.s_blocksize != ih->ih_key.k_offset + I_BYTES_NUMBER (ih, g_sb.s_blocksize))
      die ("reiserfsck_append_file: can not append indirect item (%lu) to position (%lu + %lu)",
	   comingih->ih_key.k_offset, ih->ih_key.k_offset, I_BYTES_NUMBER (ih, g_sb.s_blocksize));

    /* take unformatted pointer from an indirect item */
    ni.unfm_nodenum = *(unsigned long *)(item + pos * UNFM_P_SIZE);/*B_I_POS_UNFM_POINTER (bh, ih, pos);*/
    if (!is_block_used (ni.unfm_nodenum) && !is_block_uninsertable (ni.unfm_nodenum)) {
      mark_block_used (ni.unfm_nodenum);

      /* this is for check only */
      mark_block_unformatted (ni.unfm_nodenum);
    } else {
      ni.unfm_nodenum = 0;
    }
    ni.unfm_freespace = ((pos == (I_UNFM_NUM (comingih) - 1)) ? comingih->u.ih_free_space : 0);
    retval = g_sb.s_blocksize - ni.unfm_freespace;
  }

  reiserfsck_paste_into_item (path, (const char *)&ni, UNFM_P_SIZE);
  return retval;
}


int must_there_be_a_hole (struct item_head * comingih, struct path * path)
{
  struct item_head * ih = PATH_PITEM_HEAD (path);
  int keep_free_space;

  if (I_IS_DIRECT_ITEM (ih)) {
    direct2indirect (0, path, keep_free_space = 1);
    ih = PATH_PITEM_HEAD (path);
  }

  path->pos_in_item = I_UNFM_NUM (ih);
  if (ih->ih_key.k_offset + (I_UNFM_NUM (ih) + 1) * g_sb.s_blocksize <= comingih->ih_key.k_offset)
    return 1;

  return 0;
}


int reiserfs_append_zero_unfm_ptr (struct path * path)
{
  struct unfm_nodeinfo ni;
  int keep_free_space;

  ni.unfm_nodenum = 0;
  ni.unfm_freespace = 0;

  if (I_IS_DIRECT_ITEM (PATH_PITEM_HEAD (path)))
    /* convert direct item to indirect */
    direct2indirect (0, path, keep_free_space = 0);

  reiserfsck_paste_into_item (path, (const char *)&ni, UNFM_P_SIZE);
  return 0;
}


/* write direct item to unformatted node */
static int overwrite_by_direct_item (struct item_head * comingih, char * item, struct path * path)
{
  unsigned long unfm_ptr;
  struct buffer_head * unbh, * bh;
  struct item_head * ih;
  int offset;

  bh = PATH_PLAST_BUFFER (path);
  ih = PATH_PITEM_HEAD (path);
  unfm_ptr = B_I_POS_UNFM_POINTER (bh, ih, path->pos_in_item);
  if (unfm_ptr == 0 || unfm_ptr >= SB_BLOCK_COUNT (&g_sb)) {
    unbh = reiserfsck_get_new_buffer (PATH_PLAST_BUFFER (path)->b_blocknr);
    B_I_POS_UNFM_POINTER (bh, ih, path->pos_in_item) = unbh->b_blocknr;
/* this is for check only */
mark_block_unformatted (unbh->b_blocknr);
    mark_buffer_dirty (bh, 0);
  }
  else {
    unbh = bread (g_sb.s_dev, unfm_ptr, bh->b_size);
    if (!is_block_used (unfm_ptr))
      die ("overwrite_by_direct_item: unused block %d", unfm_ptr);
  }

  offset = comingih->ih_key.k_offset % bh->b_size - 1;
  if (offset + comingih->ih_item_len > MAX_DIRECT_ITEM_LEN (bh->b_size))
    die ("overwrite_by_direct_item: direct item too long (offset=%lu, length=%u)", comingih->ih_key.k_offset, comingih->ih_item_len);

  memcpy (unbh->b_data + offset, item, comingih->ih_item_len);

  save_unfm_overwriting (unbh->b_blocknr, comingih);

  if (path->pos_in_item == I_UNFM_NUM (ih) - 1 && (bh->b_size - ih->u.ih_free_space) < (offset + comingih->ih_item_len)) {
    ih->u.ih_free_space = bh->b_size - (offset + comingih->ih_item_len);
    mark_buffer_dirty (bh, 0);
  }
  mark_buffer_dirty (unbh, 0);
  mark_buffer_uptodate (unbh, 0);
  brelse (unbh);
  return comingih->ih_item_len;
}



void overwrite_unfm_by_unfm (unsigned long unfm_in_tree, unsigned long coming_unfm, int bytes_in_unfm)
{
  struct overwritten_unfm_segment * unfm_os_list;/* list of overwritten segments of the unformatted node */
  struct overwritten_unfm_segment unoverwritten_segment;
  struct buffer_head * bh_in_tree, * coming_bh;

  if (!test_bit (coming_unfm % (g_sb.s_blocksize * 8), SB_AP_BITMAP (&g_sb)[coming_unfm / (g_sb.s_blocksize * 8)]->b_data))
    /* block (pointed by indirect item) is free, we do not have to keep its contents */
    return;

  /* coming block is marked as used in disk bitmap. Put its contents to block in tree preserving
     everything, what has been overwritten there by direct items */
  unfm_os_list = find_overwritten_unfm (unfm_in_tree, bytes_in_unfm, &unoverwritten_segment);
  if (unfm_os_list) {
    add_event (UNFM_OVERWRITING_UNFM);
    bh_in_tree = bread (g_sb.s_dev, unfm_in_tree, g_sb.s_blocksize);
    coming_bh = bread (g_sb.s_dev, coming_unfm, g_sb.s_blocksize);
    
    while (get_unoverwritten_segment (unfm_os_list, &unoverwritten_segment)) {
      if (unoverwritten_segment.ous_begin < 0 || unoverwritten_segment.ous_end > bytes_in_unfm - 1 ||
	  unoverwritten_segment.ous_begin > unoverwritten_segment.ous_end)
	die ("overwrite_unfm_by_unfm: invalid segment found (%d %d)", unoverwritten_segment.ous_begin, unoverwritten_segment.ous_end);

      memcpy (bh_in_tree->b_data + unoverwritten_segment.ous_begin, coming_bh->b_data + unoverwritten_segment.ous_begin,
	      unoverwritten_segment.ous_end - unoverwritten_segment.ous_begin + 1);
      mark_buffer_dirty (bh_in_tree, 0);
    }

    brelse (bh_in_tree);
    brelse (coming_bh);
  }
}


/* put unformatted node pointers from incoming item over the in-tree ones */
static int overwrite_by_indirect_item (struct item_head * comingih, unsigned long * coming_item, struct path * path, int * pos_in_coming_item)
{
  struct buffer_head * bh = PATH_PLAST_BUFFER (path);
  struct item_head * ih = PATH_PITEM_HEAD (path);
  int written;
  unsigned long * item_in_tree;
  int src_unfm_ptrs, dest_unfm_ptrs, to_copy;
  int i;


  item_in_tree = (unsigned long *)B_I_PITEM (bh, ih) + path->pos_in_item;
  coming_item += *pos_in_coming_item;

  dest_unfm_ptrs = I_UNFM_NUM (ih) - path->pos_in_item;
  src_unfm_ptrs = I_UNFM_NUM (comingih) - *pos_in_coming_item;
  
  if (dest_unfm_ptrs >= src_unfm_ptrs) {
    /* whole coming item (comingih) fits into item in tree (ih) starting with path->pos_in_item */
    written = I_BYTES_NUMBER (comingih, g_sb.s_blocksize) - *pos_in_coming_item * g_sb.s_blocksize;
    *pos_in_coming_item = I_UNFM_NUM (comingih);
    to_copy = src_unfm_ptrs;
    if (dest_unfm_ptrs == src_unfm_ptrs)
      ih->u.ih_free_space = comingih->u.ih_free_space;/*??*/
  } else {
    /* only part of coming item overlaps item in the tree */
    *pos_in_coming_item += dest_unfm_ptrs;
    written = dest_unfm_ptrs * g_sb.s_blocksize;
    to_copy = dest_unfm_ptrs;
    ih->u.ih_free_space = 0;
  }
  
  for (i = 0; i < to_copy; i ++) {
    if (!is_block_used (coming_item[i]) && !is_block_uninsertable (coming_item[i])) {
      if (item_in_tree[i]) {
	/* do not overwrite unformatted pointer. We must save everything what is there already from
           direct items */
	overwrite_unfm_by_unfm (item_in_tree[i], coming_item[i], g_sb.s_blocksize);
      } else {
	item_in_tree[i] = coming_item[i];
	mark_block_used (coming_item[i]);
/* this is for check only */
mark_block_unformatted (coming_item[i]);
      }
    }
  }
  mark_buffer_dirty (bh, 0);
  return written;
}


int reiserfsck_overwrite_file (struct item_head * comingih, char * item, struct path * path, int * pos_in_coming_item)
{
  __u32 unfm_ptr;
  int written = 0;
  int keep_free_space;
  struct item_head * ih = PATH_PITEM_HEAD (path);

  if (comp_short_keys (ih, &(comingih->ih_key)) != KEYS_IDENTICAL)
    die ("reiserfsck_overwrite_file: found [%lu %lu], new item [%lu %lu]", ih->ih_key.k_dir_id, ih->ih_key.k_objectid,
	 comingih->ih_key.k_dir_id, comingih->ih_key.k_objectid);
  
  if (I_IS_DIRECT_ITEM (ih)) {
    unfm_ptr = 0;
    if (I_IS_INDIRECT_ITEM (comingih)) {
      if (ih->ih_key.k_offset % g_sb.s_blocksize != 1)
	die ("reiserfsck_overwrite_file: second part of tail can not be overwritten by indirect item");
      /* use pointer from coming indirect item */
      unfm_ptr = *(__u32 *)(item + *pos_in_coming_item * UNFM_P_SIZE);
      if (unfm_ptr >= SB_BLOCK_COUNT (&g_sb) || is_block_used (unfm_ptr) || 
	  !was_block_used (unfm_ptr) || is_block_uninsertable (unfm_ptr))
	unfm_ptr = 0;
    }
    /* */
    direct2indirect (unfm_ptr, path, keep_free_space = 1);
  }

  if (I_IS_DIRECT_ITEM (comingih)) {
    written = overwrite_by_direct_item (comingih, item, path);
  } else {
    written = overwrite_by_indirect_item (comingih, (unsigned long *)item, path, pos_in_coming_item);
  }

  return written;
}


/*
 */
int reiserfsck_file_write (struct item_head * ih, char * item)
{
  struct path path;
  struct item_head * path_ih;
  int count, pos_in_coming_item;
  int retval;
  struct key key;
  int written;

  if (make_file_writeable (ih) == -1)
    /* write was not completed. Skip that item. Maybe it should be
       saved to lost_found */
    return 0;

  count = I_BYTES_NUMBER (ih, g_sb.s_blocksize);
  pos_in_coming_item = 0;

  copy_key (&key, &(ih->ih_key));
  while (count) {
    retval = usearch_by_position (&g_sb, &key, &path);
    if (retval == DIRECTORY_FOUND) {
      pathrelse (&path);
      return 0;
    }
    if (retval == BYTE_FOUND) {
      written = reiserfsck_overwrite_file (ih, item, &path, &pos_in_coming_item);
      count -= written;
      key.k_offset += written;
    }
    if (retval == FILE_NOT_FOUND) {
      written = create_first_item_of_file (ih, item, &path, &pos_in_coming_item);
      count -= written;
      key.k_offset += written;
    }
    if (retval == BYTE_NOT_FOUND) {
      path_ih = PATH_PITEM_HEAD (&path);
      if (must_there_be_a_hole (ih, &path) == 1)
	reiserfs_append_zero_unfm_ptr (&path);
      else {
	count -= reiserfsck_append_file (ih, item, pos_in_coming_item, &path);
	key.k_offset += g_sb.s_blocksize;
	pos_in_coming_item ++;
      }
    }
    if (count < 0)
      die ("reiserfsck_file_write: count < 0 (%d)", count);
    pathrelse (&path);
  }
  
  return I_BYTES_NUMBER (ih, g_sb.s_blocksize);
}



