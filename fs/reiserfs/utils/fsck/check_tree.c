/*
 * Copyright 1999 Hans Reiser
 */

#include "fsck.h"
#include "reiserfs.h"


//
//
//  check S+ tree of the file system 
//
// check_fs_tree stops and recommends to run fsck --rebuild-tree when:
// 1. read fails
// 2. node of wrong level found in the tree
// 3. something in the tree points to wrong block number
//      out of filesystem boundary is pointed by tree
//      to block marked as free in bitmap
//      the same block is pointed from more than one place
//      not data blocks (journal area, super block, bitmaps)
// 4. bad formatted node found
// 5. delimiting keys are incorrect
//      



/* to make sure, that no blocks are pointed to from more than one
   place we use additional bitmap (control_bitmap). If we see pointer
   to a block we set corresponding bit to 1. If it is set already -
   run fsck with --rebuild-tree */
static char ** control_bitmap;
/* will compare with what does super_block say */
int used_blocks = 0;


/* 1 if block is not marked as used in the bitmap */
static int is_block_free (struct super_block * s, blocknr_t block)
{
    int i, j;
    char * bitmap;

    i = block / (s->s_blocksize * 8);
    j = block % (s->s_blocksize * 8);

    if (opt_fsck_mode == FSCK_DEFAULT)
	bitmap = SB_AP_BITMAP (s)[i]->b_data;
    else
	bitmap = g_new_bitmap[i];
    return !test_bit (j, bitmap);
    
}


/* we have seen this block in the tree, mark corresponding bit in the
   control bitmap */
static void we_met_it (struct super_block * s, blocknr_t block)
{
    int i, j;
    
    used_blocks ++;
    i = block / (s->s_blocksize * 8);
    j = block % (s->s_blocksize * 8);
    return set_bit (j, control_bitmap [i]);
}


/* have we seen this block somewhere in the tree before? */
static int did_we_meet_it (struct super_block * s, blocknr_t block)
{
    int i, j;
    
    i = block / (s->s_blocksize * 8);
    j = block % (s->s_blocksize * 8);
    return test_bit (j, control_bitmap [i]);
}


static void init_control_bitmap (struct super_block * s)
{
    int i;

    control_bitmap = getmem (sizeof (char *) * SB_BMAP_NR (s));
    for (i = 0; i < SB_BMAP_NR (s); i ++) {
	control_bitmap[i] = getmem (s->s_blocksize);
	memset (control_bitmap[i], 0, s->s_blocksize);
    }
    
    /* skipped and super block */
    for (i = 0; i <= SB_BUFFER_WITH_SB (s)->b_blocknr; i ++)
	we_met_it (s, i);
    
    /* bitmaps */
    for (i = 0; i < SB_BMAP_NR (s); i ++)
	we_met_it (s, SB_AP_BITMAP (s)[i]->b_blocknr);

    for (i = 0; i < get_journal_size (s) + 1; i ++)
	we_met_it (s, i + get_journal_start (s));


    /* unused space of last bitmap is filled by 1s */
    for (i = SB_BMAP_NR (s) * s->s_blocksize * 8; --i >= SB_BLOCK_COUNT (s); ) {
	we_met_it (s, i);
	used_blocks --;
    }
}


static void print_bmap_block (int i, char * data, int silent)
{
    int j, k;
    int bits = g_sb.s_blocksize * 8;
    int zeros = 0, ones = 0;
  
    printf ("#%d: ", i);

    if (test_bit (0, data)) {
	/* first block addressed by this bitmap block is used */
	ones ++;
	if (!silent)
	    printf ("Busy (%d-", i * bits);
	for (j = 1; j < bits; j ++) {
	    while (test_bit (j, data)) {
		ones ++;
		if (j == bits - 1) {
		    if (!silent)
			printf ("%d)\n", j + i * bits);
		    goto end;
		}
		j++;
	    }
	    if (!silent)
		printf ("%d) Free(%d-", j - 1 + i * bits, j + i * bits);

	    while (!test_bit (j, data)) {
		zeros ++;
		if (j == bits - 1) {
		    if (!silent)
			printf ("%d)\n", j + i * bits);
		    goto end;
		}
		j++;
	    }
	    if (!silent)
		printf ("%d) Busy(%d-", j - 1 + i * bits, j + i * bits);

	    j --;
	end:
	}
    } else {
	/* first block addressed by this bitmap is free */
	zeros ++;
	if (!silent)
	    printf ("Free (%d-", i * bits);
	for (j = 1; j < bits; j ++) {
	    k = 0;
	    while (!test_bit (j, data)) {
		k ++;
		if (j == bits - 1) {
		    if (!silent)
			printf ("%d)\n", j + i * bits);
		    zeros += k;
		    goto end2;
		}
		j++;
	    }
	    zeros += k;
	    if (!silent)
		printf ("%d) Busy(%d-", j - 1 + i * bits, j + i * bits);
	    
	    k = 0;
	    while (test_bit (j, data)) {
		ones ++;
		if (j == bits - 1) {
		    if (!silent)
			printf ("%d)\n", j + i * bits);
		    ones += k;
		    goto end2;
		}
		j++;
	    }
	    ones += k;
	    if (!silent)
		printf ("%d) Busy(%d-", j - 1 + i * bits, j + i * bits);
	
	    j --;
	end2:
	}
    }

    printf ("used %d, free %d\n", ones, zeros);
}


static void show_diff (int n, char * disk, char * control, int bits)
{
    int i;
    int last_diff = 0;
    int from, num;
    
    for (i = 0; i < bits; i ++) {
	if (test_bit (i, disk) && !test_bit (i, control)) {
	    if (last_diff == 1) {
		num ++;
		continue;
	    } else if (last_diff == 2) {
		printf ("Block [%d-%d] free in disk bitmap, used in control\n", from, from + num - 1);
	    }
	    num = 1;
	    from = n * bits + i;
	    last_diff = 1;
	    continue;
	}
	if (!test_bit (i, disk) && test_bit (i, control)) {
	    if (last_diff == 2) {
		num ++;
		continue;
	    } else if (last_diff == 1) {
		printf ("Block [%d-%d] used in disk bitmap, free in control\n", from, from + num - 1);
	    }
	    num = 1;
	    from = n * bits + i;
	    last_diff = 2;
	    continue;
	}
	/* the same bits */
	if (last_diff == 1)
	    printf ("Block [%d-%d] used in disk bitmap, free in control\n", from, from + num - 1);
	if (last_diff == 2)
	    printf ("Block [%d-%d] free in disk bitmap, used in control\n", from, from + num - 1);
	    
	num = 0;
	from = 0;
	last_diff = 0;
	continue;
    }
}

static void compare_bitmaps (struct super_block * s)
{
    int i, wrong_bitmap = 0;
    char * bitmap;

    printf ("Comparing bitmaps..");

    if (SB_FREE_BLOCKS (s) != SB_BLOCK_COUNT (s) - used_blocks) {
	printf ("\nUsed blocks %d, super block version %d",
		used_blocks, SB_BLOCK_COUNT (s) - SB_FREE_BLOCKS (s));
	wrong_bitmap = 1;
    }

    for (i = 0; i < SB_BMAP_NR (s); i ++) {
	if (opt_fsck_mode == FSCK_DEFAULT)
	    /* we are read-only checking the partition, check this
               bitmap */
	    bitmap = SB_AP_BITMAP(s)[i]->b_data;
	else
	    /* we are re-building the tree, bitmap for check is here */
	    bitmap = g_new_bitmap [i];

	if (memcmp (bitmap, control_bitmap[i], s->s_blocksize)) {
	    printf ("\nbitmap %d does not match to the correct one", i);
	    if (opt_verbose) {
		printf ("\nSee diff");
		show_diff (i, bitmap, control_bitmap[i], s->s_blocksize * 8);
	    }
	    wrong_bitmap = 1;
	}
    }
    if (wrong_bitmap)
	reiserfs_panic (s, "\nRun reiserfsck with --rebuild-tree (or rewrite correct bitmap)\n");
    
    printf ("ok\n");
}






/* is this block legal to be pointed to by some place of the tree? */
static int bad_block_number (struct super_block * s, blocknr_t block)
{
    if (block >= SB_BLOCK_COUNT (s)) {
	reiserfs_warning ("block out of filesystem boundary found\n");
	return 1;
    }

    if (not_data_block (s, block)) {
	reiserfs_warning ("not data block is used in the tree\n");
	return 1;
    }

    if (is_block_free (s, block)) {
	reiserfs_warning ("block %lu is not marked as used in the disk bitmap\n",
			  block);
	return 1;
    }

    if (did_we_meet_it (s, block)) {
	reiserfs_warning ("block %lu is in tree already\n", block);
	return 1;
    }

    we_met_it (s, block);
    return 0;
}


/* 1 if some of fields in the block head of bh look bad */
static int bad_block_head (struct buffer_head * bh)
{
    struct block_head * blkh;

    blkh = B_BLK_HEAD (bh);
    if (__le16_to_cpu (blkh->blk_nr_item) > (bh->b_size - BLKH_SIZE) / IH_SIZE) {
	reiserfs_warning ("block %lu has wrong blk_nr_items (%z)\n", 
			  bh->b_blocknr, bh);
	return 1;
    }
    if (__le16_to_cpu (blkh->blk_free_space) > 
	bh->b_size - BLKH_SIZE - IH_SIZE * __le16_to_cpu (blkh->blk_nr_item)) {
	reiserfs_warning ("block %lu has wrong blk_free_space %z\n", 
			  bh->b_blocknr, bh);
	return 1;
    }
    return 0;
}


/* 1 if it does not look like reasonable stat data */
static int bad_stat_data (struct buffer_head * bh, struct item_head * ih)
{
    if (!is_objectid_used (ih->ih_key.k_objectid)) {
	// FIXME: this could be cured right here
	reiserfs_warning ("%lu is marked free, but used by an object");
	return 1;
    }
    return 0;
}


/* it looks like we can check item length only */
static int bad_direct_item (struct buffer_head * bh, struct item_head * ih)
{
    return 0;
}


/* each unformatted node pointer*/
static int bad_indirect_item (struct super_block * s, struct buffer_head * bh,
			      struct item_head * ih)
{
    int i;
    __u32 * ind = (__u32 *)B_I_PITEM (bh, ih);

    if (__le16_to_cpu (ih->ih_item_len) % 4)
	return 1;
    for (i = 0; i < I_UNFM_NUM (ih); i ++) {
	/* check unformatted node pointer and mark it used in the
           control bitmap */
	if (ind[i] && bad_block_number (s, __le32_to_cpu (ind[i])))
	    return 1;
    }
    /* delete this check for 3.6 */
    if (ih->u.ih_free_space > s->s_blocksize - 1)
	reiserfs_warning ("%h has wrong wong ih_free_space\n");
    return 0;
}


/* check entry count and locations of all names */
static int bad_directory_item (struct buffer_head * bh, struct item_head * ih)
{
    int i;
    struct reiserfs_de_head * deh;


    if (I_ENTRY_COUNT (ih) > __le16_to_cpu (ih->ih_item_len) / (DEH_SIZE + 1))
	return 1;

    deh = B_I_DEH (bh, ih);
    for (i = 0; i < I_ENTRY_COUNT (ih); i ++, deh ++) {
	if (__le16_to_cpu (deh->deh_location) >= __le16_to_cpu (ih->ih_item_len))
	    return 1;
	if (i && __le16_to_cpu (deh->deh_location) >= __le16_to_cpu ((deh-1)->deh_location))
	    return 1;
	if ((ih->ih_key.k_objectid != REISERFS_ROOT_OBJECTID && deh_dir_id (deh) == 0) ||
	    deh_offset (deh) == 0 || deh_objectid (deh) == 0 || 
	    deh_dir_id (deh) == deh_objectid (deh))
	    return 1;
    }
    return 0;
}


static int bad_item (struct super_block * s, struct buffer_head * bh, int i)
{
    struct item_head * ih;

    ih = B_N_PITEM_HEAD (bh, i);

    if (I_IS_STAT_DATA_ITEM (ih))
	return bad_stat_data (bh, ih);

    if (I_IS_DIRECT_ITEM (ih))
	return bad_direct_item (bh, ih);

    if (I_IS_INDIRECT_ITEM (ih))
	return bad_indirect_item (s, bh, ih);
    
    return bad_directory_item (bh, ih);
}


/* 1 if i-th and (i-1)-th items can not be neighbors */
static int bad_pair (struct super_block * s, struct buffer_head * bh, int i)
{
    struct item_head * ih;

    ih = B_N_PITEM_HEAD (bh, i);

    if (comp_keys (&((ih - 1)->ih_key), &ih->ih_key) != -1)
	return 1;

    if (I_IS_STAT_DATA_ITEM (ih))
	/* left item must be of another object */
	if (comp_short_keys (&((ih - 1)->ih_key), &ih->ih_key) != -1)
	    return 1;
    
    if (I_IS_DIRECT_ITEM (ih)) {
	/* left item must be indirect or stat data item of the same
	   file */
	if (comp_short_keys (&((ih - 1)->ih_key), &ih->ih_key) != 0)
	    return 1;
	if (!((I_IS_STAT_DATA_ITEM (ih - 1) && ih->ih_key.k_offset == 1) ||
	      (I_IS_INDIRECT_ITEM (ih - 1) && 
	       (ih - 1)->ih_key.k_offset + I_BYTES_NUMBER (ih - 1, s->s_blocksize) == 
	       ih->ih_key.k_offset)))
	    return 1;
    }

    if (I_IS_INDIRECT_ITEM (ih) || I_IS_DIRECTORY_ITEM (ih)) {
	/* left item must be stat data of the same object */
	if (comp_short_keys (&((ih - 1)->ih_key), &ih->ih_key) != 0)
	    return 1;
	if (!I_IS_STAT_DATA_ITEM (ih - 1))
	    return 1;
    }
    
    return 0;
}
 

/* 1 if block head or any of items is bad */
static int bad_leaf (struct super_block * s, struct buffer_head * bh)
{
    int i;

    if (bad_block_head (bh))
	return 1;
    
    for (i = 0; i < B_NR_ITEMS (bh); i ++) {
	if (bad_item (s, bh, i)) {
	    reiserfs_warning ("block %lu has invalid item %d: %h\n",
			      bh->b_blocknr, i, B_N_PITEM_HEAD (bh, i));
	    return 1;
	}
	
	if (i && bad_pair (s, bh, i)) {
	    reiserfs_warning ("block %lu has wrong order of items\n", 
			      bh->b_blocknr);
	    return 1;
	}
    }
    return 0;
}


/* 1 if bh does not look like internal node */
static int bad_internal (struct super_block * s, struct buffer_head * bh)
{
    return 0;
}


/* bh must be formatted node. blk_level must be tree_height - h + 1 */
static int bad_node (struct super_block * s, struct buffer_head * bh,
		     int level)
{
    if (B_LEVEL (bh) != level) {
	reiserfs_warning ("node with wrong level found in the tree\n");
	return 1;
    }

    if (bad_block_number (s, bh->b_blocknr))
	return 1;
    
    if (B_IS_ITEMS_LEVEL (bh))
	return bad_leaf (s, bh);

    return bad_internal (s, bh);
}


/* internal node bh must point to block */
static int get_pos (struct buffer_head * bh, blocknr_t block)
{
    int i;

    for (i = 0; i <= B_NR_ITEMS (bh); i ++) {
	if (B_N_CHILD (bh, i)->dc_block_number == block)
	    return i;
    }
    die ("get_pos: position for block %lu not found", block);
    return 0;
}


/* path[h] - leaf node */
static struct key * lkey (struct buffer_head ** path, int h)
{
    int pos;

    while (h > 0) {
	pos = get_pos (path[h - 1], path[h]->b_blocknr);
	if (pos)
	    return B_N_PDELIM_KEY(path[h - 1], pos - 1);
	h --;
    }
    return 0;
}


/* path[h] - leaf node */
static struct key * rkey (struct buffer_head ** path, int h)
{
    int pos;

    while (h > 0) {
	pos = get_pos (path[h - 1], path[h]->b_blocknr);
	if (pos != B_NR_ITEMS (path[h - 1]))
	    return B_N_PDELIM_KEY (path[h - 1], pos);
	h --;
    }
    return 0;
}


/* are all delimiting keys correct */
static int bad_path (struct buffer_head ** path)
{
    int h;
    struct key * dk;
    
    h = -1;
    while (path[h])
	h ++;    

    dk = lkey (path, h);
    if (dk && comp_keys (dk, B_N_PKEY (path[h], 0)))
	return 1;
    dk = rkey (path, h);
    if (dk && comp_keys (dk, B_PRIGHT_DELIM_KEY (path[h])))
	return 1;
   
    return 0;
}


static inline blocknr_t first_child (struct buffer_head * bh)
{
    return B_N_CHILD (bh, 0)->dc_block_number;
}


static inline blocknr_t last_child (struct buffer_head * bh)
{
    return B_N_CHILD (bh, B_NR_ITEMS (bh))->dc_block_number;
}


static inline blocknr_t next_child (struct buffer_head * child,
				    struct buffer_head * parent)
{
    int i;
    
    for (i = 0; i < B_NR_ITEMS (parent); i ++) {
	if (B_N_CHILD (parent, i)->dc_block_number == child->b_blocknr)
	    return B_N_CHILD (parent, i + 1)->dc_block_number;
    }
    die ("next_child: no child found: should not happen");
    return 0;
}


/* h == 0 for root level. block head's level == 1 for leaf level  */
static inline int h_to_level (struct super_block * s, int h)
{
    return SB_TREE_HEIGHT (s) - h - 1;
}


static inline int leaf_level (struct buffer_head * bh)
{
    return B_LEVEL(bh) == DISK_LEAF_NODE_LEVEL;
}


static void print (int cur, int total)
{
  printf ("/%3d (of %3d)", cur, total);fflush (stdout);
}


/* erase /XXX(of XXX) */
static void erase (void)
{
    printf ("\b\b\b\b\b\b\b\b\b\b\b\b\b");
    printf ("             ");
    printf ("\b\b\b\b\b\b\b\b\b\b\b\b\b");
    fflush (stdout);
}


/* pass the S+ tree of filesystem */
void check_fs_tree (struct super_block * s)
{
    struct buffer_head * path[MAX_HEIGHT] = {0,};
    int total[MAX_HEIGHT] = {0,};
    int cur[MAX_HEIGHT] = {0,};
    int h = 0;
    blocknr_t block = SB_ROOT_BLOCK (s);

    uread_bitmaps (s);

    init_control_bitmap (s);

    printf ("Checking S+tree..");

    while ( 1 ) {
	if (path[h])
	    die ("check_fs_tree: empty slot expected");
	
	if (h)
	    print (cur[h - 1], total[h - 1]);

	path[h] = bread (s->s_dev, block, s->s_blocksize);
	if (path[h] == 0 || bad_node (s, path[h], h_to_level (s, h)))
	    reiserfs_panic (s, "Run reiserfsck with --rebuild-tree\n");

 	if (leaf_level (path[h])) {
	    if (bad_path (path))
		reiserfs_panic (s, "Run reiserfsck with --rebuild-tree\n");

	    brelse (path[h]);
	    if (h)
	      erase ();

	    while (h && path[h]->b_blocknr == last_child (path[h - 1])) { 
		path[h] = 0;
		h --;
/*		check_internal (path[h]);*/
		brelse (path[h]);
		if (h)
		  erase ();
	    }

	    if (h == 0) {
		path[h] = 0;
		break;
	    }

	    cur[h - 1] ++;
	    block = next_child (path[h], path[h-1]);
	    path[h] = 0;
	    continue; 
	}
	total[h] = B_NR_ITEMS (path[h]) + 1;
	cur[h] = 1;
	block = first_child (path[h]);
	h ++;
    }

    /* S+ tree is correct (including all objects have correct
       sequences of items) */
    printf ("ok\n");
    
    /* compare created bitmap with the original */
    compare_bitmaps (s);

}


