/*
 * Copyright 1996-1999 Hans Reiser
 */
#include "fsck.h"
#include <time.h>




/* path is path to stat data */
static void check_regular_file (struct path * path, struct stat_data * sd)
{
    int mark_passed_items;
    struct key key;
    unsigned long size;
    struct buffer_head * bh = PATH_PLAST_BUFFER (path);/* contains stat data */
    struct item_head * ih = PATH_PITEM_HEAD (path);/* stat data item */

    if (sd->sd_nlink == 0) {

/*    print_how_far (&stat_datas, get_event (STAT_DATA_ITEMS));*/
	if ((sd->sd_mode & S_IFMT) == S_IFREG)
	    add_event (REGULAR_FILES);
	else if ((sd->sd_mode & S_IFMT) == S_IFLNK)
	    add_event (SYMLINKS);
	else
	    add_event (OTHERS);
	sd->sd_nlink ++;
	mark_item_accessed (ih, bh);
	mark_objectid_as_used (ih->ih_key.k_objectid);

	copy_key (&key, &(ih->ih_key));
	if (are_file_items_correct (&key, &size, mark_passed_items = 1, path, &sd) != 1) {
	    /* unpassed items will be deleted in pass 4 as they left unaccessed */
	    add_event (INCORRECT_REGULAR_FILES);
	}
	/* are_file_items_correct could perform indirect_to_direct, bh could be changed */
	bh = PATH_PLAST_BUFFER (path);
	/* set correct size */
	if (sd->sd_size != size) {
	    add_event (FIXED_SIZE_FILES);
	    sd->sd_size = size;
	    mark_buffer_dirty (bh, 0);
	}
	/* set first direct byte field of stat data (if it is set incorrect) */
	if (size == 0 || KEY_IS_INDIRECT_KEY(&key)) {
	    /* there are no direct items in file */
	    if (sd->sd_first_direct_byte != NO_BYTES_IN_DIRECT_ITEM) {
		sd->sd_first_direct_byte = NO_BYTES_IN_DIRECT_ITEM;
		mark_buffer_dirty (bh, 0);
	    }
	} else {
	    /* there is at least one direct item */
	    if (sd->sd_first_direct_byte != key.k_offset - (key.k_offset % g_sb.s_blocksize - 1)) {
		sd->sd_first_direct_byte = key.k_offset - (key.k_offset % g_sb.s_blocksize - 1);
		mark_buffer_dirty (bh, 0);
	    }
	}
    } else {
	if (is_item_accessed (ih) == 0)
	    die ("check_regular_file: stat data item must be accessed");
	sd->sd_nlink ++;
	mark_buffer_dirty (bh, 0);
    }
}


static int is_rootdir_key (struct key * key)
{
    if (comp_keys (key, &root_key))
	return 0;
    return 1;
}

static int is_rootdir_entry_key (struct key * key)
{
    if (comp_short_keys (key, &root_key))
	return 0;
    return 1;
}


/* when root direcotry can not be found */
static void create_root_directory (struct path * path)
{
    struct item_head ih;
    struct stat_data sd;

    /* insert stat data item */
    copy_key (&(ih.ih_key), &root_key);
    ih.ih_item_len = SD_SIZE;
    ih.u.ih_free_space = MAX_US_INT;
    mark_item_unaccessed (&ih);

    sd.sd_mode = S_IFDIR + 0755;
    sd.sd_nlink = 0;
    sd.sd_uid = 0;
    sd.sd_gid = 0;
    sd.sd_size = EMPTY_DIR_SIZE;
    sd.sd_atime = sd.sd_ctime = sd.sd_mtime = time (NULL);
    sd.u.sd_blocks = 0;
    sd.sd_first_direct_byte = MAX_UL_INT;
  
    reiserfsck_insert_item (path, &ih, (char *)(&sd));
}


static void paste_dot_and_dot_dot (struct path * path)
{
    char dir[EMPTY_DIR_SIZE];
    struct reiserfs_de_head * deh;
    struct key key;
  
    copy_key (&key, &root_key);

    deh = (struct reiserfs_de_head *)dir;
    deh[0].deh_offset = DOT_OFFSET;
    deh[0].deh_dir_id = root_key.k_dir_id;
    deh[0].deh_objectid = root_key.k_objectid;
    deh[0].deh_state = 0;
    set_bit (DEH_Visible, &(deh[0].deh_state));
    dir[DEH_SIZE] = '.';
    reiserfsck_paste_into_item (path, dir, DEH_SIZE + 1);

    key.k_offset = DOT_DOT_OFFSET;
    key.k_uniqueness = DIRENTRY_UNIQUENESS;
    if (usearch_by_entry_key (&g_sb, &key, path) == ENTRY_FOUND) {
	reiserfs_warning ("paste_dot_and_dot_dot: \"..\" found\n");
	pathrelse (path);
	return;
    }
    deh[0].deh_offset = DOT_DOT_OFFSET;
    deh[0].deh_dir_id = 0;
    deh[0].deh_objectid = root_key.k_dir_id;
    deh[0].deh_state = 0;
    set_bit (DEH_Visible, &(deh[0].deh_state));
    dir[DEH_SIZE] = '.';
    dir[DEH_SIZE + 1] = '.';

    reiserfsck_paste_into_item (path, dir, DEH_SIZE + 2);
}


static void insert_dot_and_dot_dot (struct path * path)
{
    struct item_head ih;
    char dir[EMPTY_DIR_SIZE];
    struct reiserfs_de_head * deh;
   
    copy_key (&(ih.ih_key), &root_key);
    ih.ih_key.k_offset = DOT_OFFSET;
    ih.ih_key.k_uniqueness = DIRENTRY_UNIQUENESS;
    ih.ih_item_len = EMPTY_DIR_SIZE;
    ih.u.ih_entry_count = 2;
    mark_item_unaccessed (&ih);

    deh = (struct reiserfs_de_head *)dir;
    deh[0].deh_offset = DOT_OFFSET;
    deh[0].deh_dir_id = root_key.k_dir_id;
    deh[0].deh_objectid = root_key.k_objectid;
    deh[0].deh_location = ih.ih_item_len - strlen (".");
    deh[0].deh_state = 0;
    set_bit (DEH_Visible, &(deh[0].deh_state));

    deh[1].deh_offset = DOT_DOT_OFFSET;
    deh[1].deh_dir_id = 0;
    deh[1].deh_objectid = root_key.k_dir_id;
    deh[1].deh_location = deh[0].deh_location - strlen ("..");
    deh[1].deh_state = 0;
    set_bit (DEH_Visible, &(deh[1].deh_state));
    dir[DEH_SIZE * 2] = '.';
    dir[DEH_SIZE * 2 + 1] = '.';
    dir[DEH_SIZE * 2 + 2] = '.';

    reiserfsck_insert_item (path, &ih, dir);
}


/* returns buffer, containing found directory item.*/
char * get_next_directory_item (struct path * path, struct key * key, struct key * parent, struct item_head * ih)
{
    char * dir_item;
    struct key * rdkey;
    struct buffer_head * bh;
    struct reiserfs_de_head * deh;
    int i;
    int retval;

    if ((retval = usearch_by_entry_key (&g_sb, key, path)) != ENTRY_FOUND) {
	if (key->k_offset != DOT_OFFSET)
	    die ("get_next_directory_item: entry not found");

	/* first directory item not found */
	if (is_rootdir_entry_key (key)) {
	    /* add "." and ".." to the root directory */
	    if (retval == ENTRY_NOT_FOUND)
		paste_dot_and_dot_dot (path);
	    else if (retval == DIRECTORY_NOT_FOUND)
		insert_dot_and_dot_dot (path);
	    else
		die ("get_next_directory_item: invalid return value");
	    usearch_by_entry_key (&g_sb, key, path);
	} else {
	    /* it is ok for directories but the root one that "." is not found */
	    pathrelse (path);
	    return 0;
	}
    }
    /* leaf containing directory item */
    bh = PATH_PLAST_BUFFER (path);

    memcpy (ih, PATH_PITEM_HEAD (path), IH_SIZE);

    /* make sure, that ".." exists as well */
    if (key->k_offset == DOT_OFFSET) {
	if (I_ENTRY_COUNT (ih) < 2) {
	    pathrelse (path);
	    return 0;
	}
	deh = B_I_DEH (bh, ih) + 1;
	if (I_DEH_N_ENTRY_FILE_NAME_LENGTH (ih, deh, 1) != strlen ("..") ||
	    memcmp ("..", B_I_E_NAME (1, bh, ih), 2)) {
	    printf ("******get_next_directory_item: \"..\" not found***********\n");
	    pathrelse (path);
	    return 0;
	}
    }

    deh = B_I_DEH (bh, ih);

    /* mark hidden entries as visible, reset ".." correctly */
    for (i = 0; i < I_ENTRY_COUNT (ih); i ++, deh ++) {
	if (de_hidden (deh)) {
	    if (opt_verbose)
		reiserfs_warning ("\nget_next_directory_item: hidden entry %d\n", i);

	    mark_de_visible (deh);
	    mark_buffer_dirty (bh, 0);
	}
	if (deh->deh_offset == DOT_DOT_OFFSET) {
	    /* set ".." so that it points to the correct parent directory */
	    if (comp_short_keys (&(deh->deh_dir_id), parent) && 
		deh->deh_objectid != REISERFS_ROOT_PARENT_OBJECTID) {
		if (opt_verbose)
		    reiserfs_warning ("\nget_next_directory_item: \"..\" fixed\n");
		deh->deh_dir_id = key->k_dir_id;
		deh->deh_objectid = key->k_objectid;
		mark_buffer_dirty (bh, 0);
	    }
	}
    }

    /* copy directory item to the temporary buffer */
    dir_item = getmem (ih->ih_item_len); 
    memcpy (dir_item, B_I_PITEM (bh, ih), ih->ih_item_len);

    /* next item key */
    if (PATH_LAST_POSITION (path) == (B_NR_ITEMS (PATH_PLAST_BUFFER (path)) - 1) &&
	(rdkey = uget_rkey (path)))
	copy_key (key, rdkey);
    else {
	key->k_dir_id = 0;
	key->k_objectid = 0;
    }

    mark_item_accessed (PATH_PITEM_HEAD (path), PATH_PLAST_BUFFER (path));
    return dir_item;
}


static void get_object_key (struct reiserfs_de_head * deh, struct key * key, struct key * entry_key, struct item_head * ih)
{
    key->k_dir_id = deh->deh_dir_id;
    key->k_objectid = deh->deh_objectid;
    key->k_offset = SD_OFFSET;
    key->k_uniqueness = SD_UNIQUENESS;

    entry_key->k_dir_id = ih->ih_key.k_dir_id;
    entry_key->k_objectid = ih->ih_key.k_objectid;
    entry_key->k_offset = deh->deh_offset;
    entry_key->k_uniqueness = DIRENTRY_UNIQUENESS;
}


static void reiserfsck_cut_entry (struct key * key)
{
    struct path path;

    if (usearch_by_entry_key (&g_sb, key, &path) != ENTRY_FOUND || key->k_offset == DOT_OFFSET)
	die ("reiserfsck_cut_entry: entry not found");

    if (I_ENTRY_COUNT (PATH_PITEM_HEAD (&path)) == 1)
	reiserfsck_delete_item (&path);
    else {
	struct reiserfs_de_head * deh = B_I_DEH (PATH_PLAST_BUFFER (&path), PATH_PITEM_HEAD (&path)) + path.pos_in_item;
	reiserfsck_cut_from_item (&path, -(DEH_SIZE + I_DEH_N_ENTRY_LENGTH (PATH_PITEM_HEAD (&path), deh, path.pos_in_item)));
    }
}



/* check recursively the semantic tree. Returns 0 if entry points to
   good object, and -1 or -2 if this entry must be deleted (stat data
   not found or directory does have any items).  Hard links are not
   allowed, but if directory rename has been interrupted by the system
   crash, it is possible, that fsck will find two entries (not "..") 
   pointing to the same directory. In this case fsck keeps only the
   first one. */
#define OK 0
#define STAT_DATA_NOT_FOUND -1
#define DIRECTORY_HAS_NO_ITEMS -2

static __u32 stat_datas = 0;



int check_semantic_tree (struct key * key, struct key * parent, int is_dot_dot)
{
    struct path path;
    struct stat_data * sd;

    if (!KEY_IS_STAT_DATA_KEY (key))
	die ("check_semantic_tree: key must be key of a stat data");

    /* look for stat data of an object */
    if (usearch_by_key (&g_sb, key, &path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys) == ITEM_NOT_FOUND) {
	if (is_rootdir_key (key)) {
	    /* stat data of the root directory not found. Make it */
	    create_root_directory (&path);
	    usearch_by_key (&g_sb, key, &path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys);
	} else {
	    pathrelse (&path);
	    return STAT_DATA_NOT_FOUND;
	}
    }

    sd = B_N_STAT_DATA (PATH_PLAST_BUFFER (&path), PATH_LAST_POSITION (&path));
    if ((sd->sd_nlink == 0) && ( opt_fsck == 0 ))
	print_how_far (&stat_datas, get_event (STAT_DATA_ITEMS));

    if ((sd->sd_mode & S_IFMT) != S_IFDIR) {
	/* object is not a directory (regular, symlink, device file) */
	/*if ((sd->sd_mode & S_IFMT) == S_IFLNK)
	  printf ("Symlink found\n");*/

	check_regular_file (&path, sd);
	pathrelse (&path);
	return OK;
    }

    /* object is directory */
    sd->sd_nlink ++;
    mark_buffer_dirty (PATH_PLAST_BUFFER (&path), 0);
    if (sd->sd_nlink == 1) {
	char * dir_item;
	struct item_head ih;
	struct key item_key, entry_key, object_key;
	unsigned long dir_size = 0;

	/*print_how_far (&stat_datas, get_event (STAT_DATA_ITEMS));*/

	if (key->k_objectid == REISERFS_ROOT_OBJECTID)
	    sd->sd_nlink ++;

	add_event (DIRECTORIES);
	copy_key (&item_key, key);
	item_key.k_offset = DOT_OFFSET;
	item_key.k_uniqueness = DIRENTRY_UNIQUENESS;
	pathrelse (&path);
	while ((dir_item = get_next_directory_item (&path, &item_key, parent, &ih)) != 0) {
	    /* dir_item is copy of the item in separately allocated memory */
	    int i;
	    int retval;
	    struct reiserfs_de_head * deh = (struct reiserfs_de_head *)dir_item + path.pos_in_item;

/*&&&&&&&&&&&&&&&*/
	    if (dir_size == 0) {
		if (deh->deh_offset != DOT_OFFSET || (deh + 1)->deh_offset != DOT_DOT_OFFSET)
		    die ("check_semantic_tree: Directory without \".\" or \"..\"");
	    }
/*&&&&&&&&&&&&&&&*/

	    for (i = path.pos_in_item; i < I_ENTRY_COUNT (&ih); i ++, deh ++) {
		get_object_key (deh, &object_key, &entry_key, &ih);
		retval = check_semantic_tree (&object_key, key,
					      (deh->deh_offset == DOT_OFFSET ||deh->deh_offset == DOT_DOT_OFFSET) ? 1 : 0);
		if (retval != OK) {
		    if (entry_key.k_offset == DOT_DOT_OFFSET && object_key.k_objectid == REISERFS_ROOT_PARENT_OBJECTID) {
			/* ".." of root directory can not be found */
			if (retval != STAT_DATA_NOT_FOUND)
			    die ("check_semantic_tree: stat data of parent directory of root directory found");
			dir_size += DEH_SIZE + strlen ("..");
			continue;
		    }
		    add_event (DELETED_ENTRIES);
		    reiserfsck_cut_entry (&entry_key);
		} else {
		    /* OK */
		    dir_size += DEH_SIZE + I_DEH_N_ENTRY_LENGTH (&ih, deh, i);
		}
	    }

	    freemem (dir_item);

	    if (comp_short_keys (&item_key, key) != KEYS_IDENTICAL) {
		pathrelse (&path);
		break;
	    }
	    pathrelse (&path);
	}

	if (dir_size == 0)
	    return DIRECTORY_HAS_NO_ITEMS;

	if (usearch_by_key (&g_sb, key, &path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_keys) != ITEM_FOUND)
	    die ("check_semantic_tree: stat data not found");

	mark_objectid_as_used (PATH_PITEM_HEAD (&path)->ih_key.k_objectid);

	if (dir_size != (sd = B_N_STAT_DATA (PATH_PLAST_BUFFER (&path), PATH_LAST_POSITION (&path)))->sd_size) {
	    add_event (FIXED_SIZE_DIRECTORIES);
	    sd->sd_size = dir_size;
	}
	/* stat data of a directory is accessed */
	mark_item_accessed (PATH_PITEM_HEAD (&path), PATH_PLAST_BUFFER (&path));
    } else {
	/* we have accessed directory stat data not for the first time. we
	   can come here only from "." or "..". Other names must be removed
	   to avoid creation of hard links */
	if (!is_dot_dot) {
	    sd->sd_nlink --;
	    if (opt_verbose)
		reiserfs_warning ("\ncheck_semantic_tree: more than one name (neither \".\" nor \"..\") of a directory. Removed\n");
	    pathrelse (&path);
	    return STAT_DATA_NOT_FOUND;
	}
    }
    pathrelse (&path);


    return OK;
}


struct key g_root_directory_key = {REISERFS_ROOT_PARENT_OBJECTID, REISERFS_ROOT_OBJECTID, 0, 0};
struct key g_parent_root_directory_key = {0, REISERFS_ROOT_PARENT_OBJECTID, 0, 0};

void semantic_pass (void)
{
    if (opt_stop_point == STOP_AFTER_PASS1 || opt_stop_point == STOP_AFTER_PASS2)
	return;

    if ( opt_fsck == 0 )
	fprintf (stderr, "Pass 3 (semantic) - ");
    check_semantic_tree (&g_root_directory_key, &g_parent_root_directory_key, 0);
    if ( opt_fsck == 0 )
	printf ("\n");
}


