/*
 * Copyright 2000 Hans Reiser
 */
#include "fsck.h"
#include "reiserfs.h"


//
// this will probably eventually replace pass4 in fsck
//


//
// FIXME: there is no way to know what hash function is used to order
// names in directory
//
#define MAX_GEN_NUMBER  127
#define SET_GENERATION_NUMBER(offset,gen_number) (GET_HASH_VALUE(offset)|(gen_number))
static __u32 get_third_component (char * name, int len)
{
    unsigned long res;

    if (!len || (len == 1 && name[0] == '.'))
	return DOT_OFFSET;
    if (len == 2 && name[0] == '.' && name[1] == '.')
	return DOT_DOT_OFFSET;

    res = keyed_hash (name, len);
    res = GET_HASH_VALUE(res);
    if (res == 0)
	res = 128;
    return res + MAX_GEN_NUMBER;
    
}


//
// looks for name in the directory dir, return 1 if name found, 0
// otherwise
//
static objectid_t find_entry (struct key * dir, char * name)
{
    struct key entry_key;
    struct path path;
    int retval;
    int i;

    init_path (&path);

    entry_key.k_dir_id = dir->k_dir_id;
    entry_key.k_objectid = dir->k_objectid;
    entry_key.k_offset = get_third_component (name, strlen (name));
    entry_key.k_uniqueness = DIRENTRY_UNIQUENESS;
    
    while (1) {
	struct buffer_head * bh;
	struct item_head * ih;
	struct reiserfs_de_head * deh;

	retval = usearch_by_key (&g_sb, &entry_key, &path, 0, DISK_LEAF_NODE_LEVEL, 
				 READ_BLOCKS, comp_keys);
	if (retval == ITEM_NOT_FOUND)
	    PATH_LAST_POSITION (&path) --;
	
	bh = PATH_PLAST_BUFFER (&path);
	ih = PATH_PITEM_HEAD (&path);
	deh = B_I_DEH (bh, ih);
	for (i = ih->u.ih_entry_count - 1; i >= 0; i --) {
	    if (strlen (name) != I_DEH_N_ENTRY_LENGTH (ih, deh + i, i))
		continue;
	    if (!memcmp (B_I_E_NAME (i, bh, ih), name, strlen (name))) {
		pathrelse (&path);
		return deh[i].deh_objectid;;
	    }
	}
	pathrelse (&path);
	if (ih->ih_key.k_offset == DOT_OFFSET)
	    return 0;
	entry_key.k_offset --;
    }
    die ("find_entry: endless loop broken");
    return 0;
}
    

//
// add name pointing to 'key' to the directory 'dir'. FIXME: this will
// not work if there is a name in directory with the same value of
// hash function
//
static void add_entry (struct key * dir, char * name, struct key * key)
{
    struct path path;
    struct key entry_key;
    char * entry;
    struct reiserfs_de_head * deh;
    int retval;

    init_path (&path);
    
    entry_key.k_dir_id = dir->k_dir_id;
    entry_key.k_objectid = dir->k_objectid;
    entry_key.k_offset = SET_GENERATION_NUMBER (get_third_component (name, strlen (name)), 0);
    entry_key.k_uniqueness = DIRENTRY_UNIQUENESS;

    retval = usearch_by_entry_key (&g_sb, &entry_key, &path);
    if (retval == ENTRY_FOUND)
	die ("add_entry: can not add name %s", name);

    entry = getmem (DEH_SIZE + strlen (name));
    deh = (struct reiserfs_de_head *)entry;
    deh->deh_location = 0;
    deh->deh_offset = entry_key.k_offset;
    deh->deh_state = 0;
    mark_de_visible (deh);
    /* put key (ino analog) to de */
    deh->deh_dir_id = key->k_dir_id;
    deh->deh_objectid = key->k_objectid;
    memcpy ((char *)(deh + 1), name, strlen (name));
   
    reiserfsck_paste_into_item (&path, entry, DEH_SIZE + strlen (name));
}


/* mkreiserfs should have created this */
static objectid_t make_lost_found_dir (void)
{
    int retval;
    struct path path;
    struct key key;
    struct stat_data sd;
    struct item_head ih;
    char empty [EMPTY_DIR_SIZE];
    objectid_t lost_found;

    lost_found = find_entry (&root_key, "lost+found");
    if (lost_found)
	return lost_found;

    key.k_dir_id = REISERFS_ROOT_OBJECTID;
    key.k_objectid = get_unused_objectid (&g_sb);
    key.k_offset = SD_OFFSET;
    key.k_uniqueness = SD_UNIQUENESS;

    /* stat data */
    make_dir_stat_data (&key, &ih, &sd);

    retval = usearch_by_key (&g_sb, &key, &path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys);
    if (retval != KEY_NOT_FOUND)
	die ("make_lost_found_dir: can not create stat data of \'lost+found\'");

    reiserfsck_insert_item (&path, &ih, (char *)&sd);

    
    /* empty dir item */
    ih.ih_key.k_offset = DOT_OFFSET;
    ih.ih_key.k_uniqueness = DIRENTRY_UNIQUENESS;
    ih.ih_item_len = EMPTY_DIR_SIZE;
    ih.u.ih_entry_count = 2;

    make_empty_dir_item (empty, key.k_dir_id, key.k_objectid, 
			 REISERFS_ROOT_PARENT_OBJECTID, REISERFS_ROOT_OBJECTID);

    retval = usearch_by_key (&g_sb, &(ih.ih_key), &path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys);
    if (retval != KEY_NOT_FOUND)
	die ("make_lost_found_dir: can not create empty dir body of \'lost+found\'");

    reiserfsck_insert_item (&path, &ih, empty);

    add_entry (&root_key, "lost+found", &key);

    {
	struct stat_data * root;

	/* update root directory */
	if (usearch_by_key (&g_sb, &root_key, &path, 0, DISK_LEAF_NODE_LEVEL,
			    READ_BLOCKS, comp_keys) != ITEM_FOUND)
	    die ("make_lost_found_dir: can not find root directory");
	root = B_I_STAT_DATA (PATH_PLAST_BUFFER (&path), PATH_PITEM_HEAD (&path));
	root->sd_size += DEH_SIZE + strlen ("lost+found");
	mark_buffer_dirty (PATH_PLAST_BUFFER (&path), 1);
	pathrelse (&path);
    }

    return key.k_objectid;
}



char lost_name[80];

static void link_lost (struct key * lost_found, struct buffer_head * bh)
{
    int i;
    struct item_head * ih;


    ih = B_N_PITEM_HEAD (bh, 0);
    for (i = 0; i < B_NR_ITEMS (bh); i ++, ih++) {
	if (I_IS_STAT_DATA_ITEM (ih) && ih->ih_reserved == 0xffff && S_ISDIR (B_I_STAT_DATA (bh, ih)->sd_mode)) {
	    struct key key;
	    
	    sprintf (lost_name, "%u_%u", ih->ih_key.k_dir_id, ih->ih_key.k_objectid);
	    /* entry in lost+found directory will point to this key */
	    key.k_dir_id = ih->ih_key.k_dir_id;
	    key.k_objectid = ih->ih_key.k_objectid;
	    add_entry (lost_found, lost_name, &key);
	}
    }
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


void pass4 (struct super_block * s)
{
    struct buffer_head * path[MAX_HEIGHT] = {0,};
    int total[MAX_HEIGHT] = {0,};
    int cur[MAX_HEIGHT] = {0,};
    int h = 0;
    blocknr_t block = SB_ROOT_BLOCK (s);
    struct key lost_found;


    if (opt_stop_point != STOP_DEFAULT || opt_lost_found == NO_LOST_FOUND)
	return ;


    /* create lost+found directory (if it is not there) */
    lost_found.k_dir_id = root_key.k_objectid;
    lost_found.k_objectid = make_lost_found_dir ();
    lost_found.k_offset = lost_found.k_uniqueness = 0;


    printf ("Looking for lost files..");

    while ( 1 ) {
	if (path[h])
	    die ("pass4: empty slot expected");
	
	if (h)
	    print (cur[h - 1], total[h - 1]);

	path[h] = bread (s->s_dev, block, s->s_blocksize);
	if (path[h] == 0)
	    die ("pass4: bread failed");

 	if (leaf_level (path[h])) {
	    link_lost (&lost_found, path[h]);

	    brelse (path[h]);
	    if (h)
	      erase ();

	    while (h && path[h]->b_blocknr == last_child (path[h - 1])) { 
		path[h] = 0;
		h --;
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

    /* we have passed whole tree once again. Something have been added
       to the lost+found directory. Do semantic pass for it */

    printf ("Checking lost+found directory.."); fflush (stdout);
    check_semantic_tree (&lost_found, &root_key, 0);
    printf ("ok\n");
    
}
