/*
 * Copyright 1996, 1997 Hans Reiser
 */

#include "fsck.h"
#include "reiserfs.h"


int check_file_system ()
{
  return 0;
}

/* this goes through buffers checking delimiting keys
 */

struct buffer_head * g_left = 0;
struct buffer_head * g_right = 0;
struct key * g_dkey = 0;


static void check_directory_item (struct item_head * ih, struct buffer_head * bh)
{
  int i;
  struct reiserfs_de_head * deh;

  for (i = 0, deh = B_I_DEH (bh, ih); i < I_ENTRY_COUNT (ih) - 1; i ++)
    if (deh[i].deh_offset > deh[i + 1].deh_offset)
      die ("check_directory_item: entries are not sorted properly");
}


static void check_items (struct buffer_head * bh)
{
  int i;
  struct item_head * ih;

  for (i = 0, ih = B_N_PITEM_HEAD (bh, i); i < B_NR_ITEMS (bh); i ++, ih) {
    if (I_IS_DIRECTORY_ITEM (ih))
      check_directory_item (ih, bh);
  }
}


static void compare_neighboring_leaves_in_pass1 (void)
{
  struct key * left = B_N_PKEY (g_left, B_NR_ITEMS (g_left) - 1);


  if (comp_keys (left, B_N_PKEY (g_right, 0)) != SECOND_GREATER)
    die ("compare_neighboring_leaves_in_pass1: left key is greater, that the right one");

  if (/*comp_keys (B_PRIGHT_DELIM_KEY (g_left), g_dkey) == FIRST_GREATER ||*/
      comp_keys (g_dkey, B_N_PKEY (g_right, 0)) != KEYS_IDENTICAL) {
    reiserfs_panic (0, "compare_neighboring_leaves_in_pass1: left's rdkey %k, dkey %k, first key in right %k",
		    B_PRIGHT_DELIM_KEY (g_left), g_dkey, B_N_PKEY (g_right, 0));
  }
  
  check_items (g_left);

/*&&&&&&&&&&&&&&&&&&&&&&&&&&
  for (i = 0, ih = B_N_PITEM_HEAD (g_left, i); i < B_NR_ITEMS (g_left); i ++, ih ++)
    if (is_item_accessed (ih) == YES)
      die ("compare_neighboring_leaves_in_pass1: item marked as accessed in g_left");
  for (i = 0, ih = B_N_PITEM_HEAD (g_right, i); i < B_NR_ITEMS (g_right); i ++, ih ++)
    if (is_item_accessed (ih) == YES)
      die ("compare_neighboring_leaves_in_pass1: item marked as accessed in g_right");
&&&&&&&&&&&&&&&&&&&&&&&&&&&*/
    
}


static void is_there_unaccessed_items (struct buffer_head * bh)
{
  int i;
  struct item_head * ih;

  ih = B_N_PITEM_HEAD (bh, 0);
  for (i = 0; i < B_NR_ITEMS (bh); i ++, ih ++) {
    if (is_objectid_used (ih->ih_key.k_objectid) == 0)
      die ("is_there_unaccessed_items: %lu is not marked as used", ih->ih_key.k_objectid);
      
    if (is_item_accessed (ih) == 0) {
      print_block (bh, 1, -1, -1);
      die ("is_there_unaccessed_items: unaccessed item found");
    }
  }
}


static void compare_neighboring_leaves_after_all (void)
{
  struct key * left = B_N_PKEY (g_left, B_NR_ITEMS (g_left) - 1);
  struct key * right = B_N_PKEY (g_right, 0);

  if (comp_keys (left, B_PRIGHT_DELIM_KEY (g_left)) != SECOND_GREATER)
    die ("compare_neighboring_leaves_after_all: invalid right delimiting key");

  if (comp_keys (left, B_N_PKEY (g_right, 0)) != SECOND_GREATER)
    die ("compare_neighboring_leaves_after_all: left key is greater, that the right one");

  if (comp_keys (B_PRIGHT_DELIM_KEY (g_left), g_dkey) != KEYS_IDENTICAL ||
      comp_keys (g_dkey, B_N_PKEY (g_right, 0)) != KEYS_IDENTICAL) {
    reiserfs_panic (0, "compare_neighboring_leaves_after all: invalid delimiting keys from left to right (%k %k %k)",
		    B_PRIGHT_DELIM_KEY (g_left), g_dkey, B_N_PKEY (g_right, 0));
  }

  if (comp_short_keys (left, right) == KEYS_IDENTICAL) {
    if (KEY_IS_DIRECT_KEY (left) || KEY_IS_INDIRECT_KEY (left))
      if (right->k_offset != left->k_offset + I_BYTES_NUMBER (B_N_PITEM_HEAD (g_left, B_NR_ITEMS (g_left) - 1), g_sb.s_blocksize))
	die ("compare_neighboring_leaves_after all: hole between items or items are overlapped");
  }

  is_there_unaccessed_items (g_left);
  
}


typedef	void (check_function_t)(void);

static void reiserfsck_check_tree (int dev, int block, int size, check_function_t comp_func)
{
  struct buffer_head * bh;

  bh = bread (dev, block, size);

  if (!B_IS_IN_TREE (bh)) {
    reiserfs_panic (0, "reiserfsck_check_tree: buffer (%b %z) not in tree", bh, bh);
  }

  if (not_formatted_node (bh->b_data, bh->b_size))
    die ("Not formatted node");
  if (!is_block_used (bh->b_blocknr))
    die ("Not marked as used");
  if (is_leaf_node (bh->b_data) && is_leaf_bad (bh))
    die ("Bad leaf");
  if (is_internal_node (bh->b_data) && is_internal_bad (bh))
    die ("bad internal");

#if 0
    || !is_block_used (bh->b_blocknr) ||
      (is_leaf_node (bh->b_data) && is_leaf_bad (bh)) ||
      (is_internal_node (bh->b_data) && is_internal_bad (bh)))
    die ("reiserfsck_check_tree: bad node in the tree");
#endif

  if (B_IS_KEYS_LEVEL (bh)) {
    int i;
    struct disk_child * dc;

    dc = B_N_CHILD (bh, 0);
    for (i = 0; i <= B_NR_ITEMS (bh); i ++, dc ++) {
      reiserfsck_check_tree (dev, dc->dc_block_number, size, comp_func);
      g_dkey = B_N_PDELIM_KEY (bh, i);
    }
  } else if (B_IS_ITEMS_LEVEL (bh)) {
    g_right = bh;
    if (g_left != 0 && g_dkey != 0) {
      comp_func ();
      brelse (g_left);
    }
    g_left = g_right;
    return;
  } else {
    print_block (bh, 0, -1, -1);
    reiserfs_panic (0, "reiserfsck_check_tree: bad block type");
  }
  brelse (bh);
}

static void reiserfsck_check_cached_tree (int dev, int block, int size)
{
  struct buffer_head * bh;

  bh = find_buffer (dev, block, size);
  if (bh == 0)
    return;
  if (!buffer_uptodate (bh)) {
    die ("reiserfsck_check_cached_tree: found notuptodate buffer");
  }
  bh->b_count ++;

  if (!B_IS_IN_TREE (bh)) {
    die ("reiserfsck_check_cached_tree: buffer (%b %z) not in tree", bh, bh);
  }

  if (not_formatted_node (bh->b_data, bh->b_size) || !is_block_used (bh->b_blocknr) ||
      (is_leaf_node (bh->b_data) && is_leaf_bad (bh)) ||
      (is_internal_node (bh->b_data) && is_internal_bad (bh)))
    die ("reiserfsck_check_cached_tree: bad node in the tree");
  if (B_IS_KEYS_LEVEL (bh)) {
    int i;
    struct disk_child * dc;

    dc = B_N_CHILD (bh, 0);
    for (i = 0; i <= B_NR_ITEMS (bh); i ++, dc ++) {
      reiserfsck_check_cached_tree (dev, dc->dc_block_number, size);
      g_dkey = B_N_PDELIM_KEY (bh, i);
    }
  } else if (B_IS_ITEMS_LEVEL (bh)) {
    /*    g_right = bh;
    if (g_left != 0 && g_dkey != 0) {
      comp_func ();
      brelse (g_left);
    }
    g_left = g_right;*/
    brelse (bh);
    return;
  } else {
    print_block (bh, 0, -1, -1);
    reiserfs_panic (0, "reiserfsck_check_cached_tree: bad block type");
  }
  brelse (bh);
}


void reiserfsck_tree_check (check_function_t how_to_compare_neighbors)
{
  g_left = 0;
  g_dkey = 0;
  reiserfsck_check_tree (g_sb.s_dev, SB_ROOT_BLOCK (&g_sb), g_sb.s_blocksize, how_to_compare_neighbors);
  brelse (g_right);
}


void reiserfsck_check_pass1 ()
{
/*  if (opt_check == 1)*/
    reiserfsck_tree_check (compare_neighboring_leaves_in_pass1);
}

void check_cached_tree ()
{
  reiserfsck_check_cached_tree (g_sb.s_dev, SB_ROOT_BLOCK (&g_sb), g_sb.s_blocksize);
}

void reiserfsck_check_after_all ()
{
  reiserfsck_tree_check (compare_neighboring_leaves_after_all);
}


#if 0
/* returns 1 if buf looks like a leaf node, 0 otherwise */
static int is_leaf (char * buf, int blocksize)
{
  struct block_head * blkh;
  struct item_head * ih;
  int used_space;
  int prev_location;
  int i;

  blkh = (struct block_head *)buf;
  ih = (struct item_head *)(buf + BLKH_SIZE) + blkh->blk_nr_item - 1;
  used_space = BLKH_SIZE + IH_SIZE * blkh->blk_nr_item + (blocksize - ih->ih_item_location);
  if (used_space != blocksize - blkh->blk_free_space)
    return 0;
  ih = (struct item_head *)(buf + BLKH_SIZE);
  prev_location = blocksize;
  for (i = 0; i < blkh->blk_nr_item; i ++, ih ++) {
    if (ih->ih_item_location >= blocksize || ih->ih_item_location < IH_SIZE * blkh->blk_nr_item)
      return 0;
    if (ih->ih_item_len < 1 || ih->ih_item_len > MAX_ITEM_LEN (blocksize))
      return 0;
    if (prev_location - ih->ih_item_location != ih->ih_item_len)
      return 0;
    prev_location = ih->ih_item_location;
  }

  return 1;
}


/* returns 1 if buf looks like an internal node, 0 otherwise */
static int is_internal (char * buf, int blocksize)
{
  struct block_head * blkh;
  int used_space;

  blkh = (struct block_head *)buf;
  used_space = BLKH_SIZE + KEY_SIZE * blkh->blk_nr_item + DC_SIZE * (blkh->blk_nr_item + 1);
  if (used_space != blocksize - blkh->blk_free_space)
    return 0;
  return 1;
}


/* sometimes unfomatted node looks like formatted, if we check only
   block_header. This is the reason, why it is so complicated. We
   believe only when free space and item locations are ok 
   */
int not_formatted_node (char * buf, int blocksize)
{
  struct block_head * blkh;

  blkh = (struct block_head *)buf;

  if (blkh->blk_level == FREE_LEVEL ||
      blkh->blk_level < DISK_LEAF_NODE_LEVEL || blkh->blk_level > MAX_HEIGHT)
    /* blk_level is out of range */
    return 1;

  if (blkh->blk_nr_item < 1 || blkh->blk_nr_item > (blocksize - BLKH_SIZE) / IH_SIZE)
    /* item number is out of range */
    return 1;

  if (blkh->blk_free_space > blocksize - BLKH_SIZE - IH_SIZE)
    /* free space is out of range */
    return 1;

  /* check format of nodes, such as we are not sure, that this is formatted node */
  if (blkh->blk_level == DISK_LEAF_NODE_LEVEL)
    return (is_leaf (buf, blocksize) == 1) ? 0 : 1;
  return (is_internal (buf, blocksize) == 1) ? 0 : 1;
}
#endif



int is_internal_node (char * buf)
{
  struct block_head * blkh;
  
  blkh = (struct block_head *)buf;
  if (blkh->blk_level != DISK_LEAF_NODE_LEVEL)
    return 1;
  return 0;
}

int is_leaf_node (char * buf)
{
  struct block_head * blkh;

  blkh = (struct block_head *)buf;
  if (blkh->blk_level == DISK_LEAF_NODE_LEVEL)
    return 1;
  return 0;
}

static int is_bad_sd (struct item_head * ih, char * item)
{
  struct stat_data * sd = (struct stat_data *)item;

  if (!S_ISDIR (sd->sd_mode) && !S_ISREG(sd->sd_mode) &&
      !S_ISCHR (sd->sd_mode) && !S_ISBLK(sd->sd_mode) &&
      !S_ISLNK (sd->sd_mode) && !S_ISFIFO(sd->sd_mode) &&
      !S_ISSOCK(sd->sd_mode)) {
    if (opt_verbose)
      reiserfs_warning ("file %k unexpected mode encountered 0%o\n", &ih->ih_key, sd->sd_mode);
  }
  return 0;
}



static int is_bad_directory (struct item_head * ih, char * item, int blocksize)
{
  int i;
  int namelen;
  struct reiserfs_de_head * deh = (struct reiserfs_de_head *)item;
  __u32 prev_offset = 0;
  __u16 prev_location = 0xffff;

  for (i = 0; i < I_ENTRY_COUNT (ih); i ++) {
    namelen = I_DEH_N_ENTRY_FILE_NAME_LENGTH (ih, deh + i, i);
    if (namelen > REISERFS_MAX_NAME_LEN (blocksize)) {
      return 1;
    }
    if (deh[i].deh_offset <= prev_offset) {
      return 1;
    }
    prev_offset = deh[i].deh_offset;

    if (deh[i].deh_location >= prev_location) {
      return 1;
    }
  }

  return 0;
}


#include <sys/ioctl.h>
#include <sys/mount.h>


int blocks_on_device (int dev, int blocksize)
{
int size;

  if (ioctl (dev, BLKGETSIZE, &size) >= 0) {
    return  size / (blocksize / 512);
  }
  if (ioctl (dev, BLKGETSIZE, &size) >= 0) {
    return  size / (blocksize / 512);
  } else {
    struct stat stat_buf;
    memset(&stat_buf, '\0', sizeof(struct stat));
    if(fstat(dev, &stat_buf) >= 0) {
      return stat_buf.st_size / (blocksize / 512);
    } else {
      die ("can not calculate device size\n");
    }
  }
  return 0;
}


/* change incorrect block adresses by 0. Do not consider such item as incorrect */
static int is_bad_indirect (struct item_head * ih, char * item, int dev, int blocksize)
{
  int i;
  int bad = 0;
  int blocks;

  if (ih->ih_item_len % UNFM_P_SIZE) {
    if (opt_verbose)
      reiserfs_warning ("indirect item of %h of invalid length");
    return 1;
  }
  blocks = blocks_on_device (dev, blocksize);
  
  for (i = 0; i < I_UNFM_NUM (ih); i ++) {
    __u32 * ind = (__u32 *)item;

    if (ind[i] >= blocks) {
      bad ++;
      ind[i] = 0;
      continue;
    }
  }
  return 0;
}


int is_bad_item (struct item_head * ih, char * item, int blocksize, int dev)
{
  if (I_IS_STAT_DATA_ITEM (ih))
    return is_bad_sd (ih, item);

  if (I_IS_DIRECTORY_ITEM (ih))
    return is_bad_directory (ih, item, blocksize);

  if (I_IS_INDIRECT_ITEM (ih))
    return is_bad_indirect (ih, item, dev, blocksize);

  return 0;
}


/* only directory item can be fatally bad */
int is_leaf_bad (struct buffer_head * bh)
{
  int i;
  struct item_head * ih;

  if (!is_leaf_node (bh->b_data))
    return 0;
  for (i = 0, ih = B_N_PITEM_HEAD (bh,  0); i < B_NR_ITEMS (bh); i ++, ih ++)
    if (is_bad_item (ih, B_I_PITEM (bh, ih), bh->b_size, bh->b_dev))
      return 1;
  return 0;
}

int is_internal_bad (struct buffer_head * bh)
{
  struct key * key;
  int i;
  
  if (!is_internal_node (bh->b_data))
    return 0;
  for (i = 0; i < B_NR_ITEMS (bh); i ++) {
    key = B_N_PDELIM_KEY (bh, i);
    if (//key->k_dir_id >= key->k_objectid ||
	(key->k_uniqueness != 500 && key->k_uniqueness != (__u32)-1 && key->k_uniqueness != (__u32)-2 &&
	 key->k_uniqueness != 0))
      return 1;
  }
  return 0;

}


