/*
 * Copyright 1996, 1997, 1998 Hans Reiser
 */
#include "fsck.h"


static void get_next_key (struct path * path, int i, struct key * key)
{
    struct buffer_head * bh = PATH_PLAST_BUFFER (path);
    struct key maxkey = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    struct key * rkey;

    if (i < B_NR_ITEMS (bh) - 1) {
	copy_key (key, B_N_PKEY (bh, i + 1));
	return;
    }

    rkey = uget_rkey (path);
    if (rkey) {
	copy_key (key, rkey);
	if (comp_keys (key, B_PRIGHT_DELIM_KEY (bh)) != KEYS_IDENTICAL) {
	    add_event (FIXED_RIGHT_DELIM_KEY);
	    copy_key (B_PRIGHT_DELIM_KEY (bh), key);
	    mark_buffer_dirty (bh, 0);
	}
    } else {
	if (comp_keys (&maxkey, B_PRIGHT_DELIM_KEY (bh)) != KEYS_IDENTICAL) {
	    /*printf ("get_next_key: Hmm, max key not found in the tree\n");*/
	    copy_key (B_PRIGHT_DELIM_KEY (bh), &maxkey);
	    mark_buffer_dirty (bh, 0);
	}
	copy_key (key, &maxkey);
    }
}

/* this is pass 4 */
int check_unaccessed_items (void)
{
    struct key key;
    struct path path;
    int i;
    struct buffer_head * bh;
    struct item_head * ih;
    __u32 passed = 0;

    if (opt_stop_point != STOP_DEFAULT)
	return 0;

    path.path_length = ILLEGAL_PATH_ELEMENT_OFFSET;
    copy_key (&key, &g_root_directory_key);

    if ( opt_fsck == 0 )
	fprintf (stderr, "Pass 4 - ");

    while (/*reiserfsck_*/usearch_by_key (&g_sb, &key, &path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys) == ITEM_FOUND) {
	bh = PATH_PLAST_BUFFER (&path);
	for (i = PATH_LAST_POSITION (&path), ih = PATH_PITEM_HEAD (&path); i < B_NR_ITEMS (bh); i ++, ih ++) {
	    if (is_item_accessed (ih) == 0) {

		get_next_key (&path, i, &key);

		add_event (UNACCESSED_ITEMS);
		if (I_IS_STAT_DATA_ITEM (ih))
		    g_fsck_info.fs_stat_data_items --;
	
		PATH_LAST_POSITION (&path) = i;
		reiserfsck_delete_item (&path);

		goto cont;
	    }
	    if ((I_IS_STAT_DATA_ITEM (ih)) && opt_fsck == 0) {
		print_how_far (&passed, get_event (STAT_DATA_ITEMS));
	    }
	}
	get_next_key (&path, i - 1, &key);
	pathrelse (&path);

/*fu_check ();*/

    cont:
    }
    if (key.k_dir_id != MAX_UL_INT || key.k_objectid != MAX_UL_INT ||
	key.k_offset != MAX_UL_INT || key.k_uniqueness != MAX_UL_INT) {
	reiserfs_panic (0, "check_unaccessed_items: invalid exit key %k", &key);
    }
    pathrelse (&path);

    if ( opt_fsck == 0 )
	printf ("\n");

    return 0;
}
