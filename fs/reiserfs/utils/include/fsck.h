/*
 * Copyright 1996, 1997, 1998 Hans Reiser
 */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <asm/types.h>
#include <sys/vfs.h>
#include <errno.h>
#include <unistd.h>
#include <asm/byteorder.h>
#include <asm/types.h>

#include "inode.h"
#include "io.h"
#include "sb.h"
#include "misc.h"
#include "reiserfs_fs.h"


typedef __u32 blocknr_t;

/* searches.c */
#define KEY_FOUND 1
#define KEY_NOT_FOUND 0

#define DIRECTORY_NOT_FOUND -1

#define FILE_NOT_FOUND -1


#define reiserfsck_search_by_key(s,key,path,comp_func) search_by_key (s, key, path, 0, DISK_LEAF_NODE_LEVEL, READ_BLOCKS, comp_func)


/* main.c */
int main (int argc, char * argv []);


//
// options
//
extern int opt_verbose;
extern int opt_fsck;

#define FSCK_DEFAULT 0
#define FSCK_REBUILD 1
#define FSCK_FIND_ITEM 2
extern int opt_fsck_mode;

extern struct key key_to_find;

#define STOP_DEFAULT 0
#define STOP_AFTER_PASS1 1
#define STOP_AFTER_PASS2 2
#define STOP_AFTER_SEMANTIC 3
#define STOP_AFTER_REPLAY 4
extern int opt_stop_point;

#define SCAN_USED_PART 0
#define SCAN_WHOLE_PARTITION 1
extern int opt_what_to_scan;

#define NO_LOST_FOUND 0
#define DO_LOST_FOUND 1
extern int opt_lost_found;


extern struct super_block g_sb;
extern struct reiserfs_super_block * g_old_rs;
extern char ** g_disk_bitmap;
extern char ** g_new_bitmap;
extern char ** g_uninsertable_leaf_bitmap;
extern char ** g_formatted;
extern char ** g_unformatted;
extern int g_blocks_to_read;


/* pass1.c */
void build_the_tree (void);
extern int g_unaccessed_items;
int is_item_accessed (struct item_head * ih);
void mark_item_accessed (struct item_head * ih, struct buffer_head * bh);
void mark_item_unaccessed (struct item_head * ih);


/* file.c */
struct si {
  struct item_head si_ih;
  char * si_dnm_data;
  struct si * si_next;
};
void put_saved_items_into_tree (struct si *);
int reiserfsck_file_write (struct item_head * ih, char * item);
int are_file_items_correct (struct key * key, unsigned long * size, int mark_passed_items, struct path *, struct stat_data **);


/* pass2.c */
typedef	void (action_on_item_t)(struct si **, struct item_head *, char *);
action_on_item_t save_item;
action_on_item_t insert_item_separately;
void for_all_items_in_node (action_on_item_t action, struct si ** si, struct buffer_head * bh);
void take_bad_blocks_put_into_tree ();
void insert_each_item_separately (struct buffer_head *);


/* semantic.c */
extern struct key g_root_directory_key;
void semantic_pass (void);
int check_semantic_tree (struct key * key, struct key * parent, int is_dot_dot);



/* pass4.c */
int check_unaccessed_items (void);
void pass4 (struct super_block *);


/* check.c */
int check_file_system (void);
void reiserfsck_check_pass1 (void);
void reiserfsck_check_after_all (void);
int is_leaf_bad (struct buffer_head * bh);
int is_internal_bad (struct buffer_head * bh);

void check_fs_tree (struct super_block * s);



/* noname.c */
void get_max_buffer_key (struct buffer_head * bh, struct key * key);

/* ustree.c */
void init_tb_struct (struct tree_balance * tb, struct super_block  * s, struct path * path, int size);
void reiserfsck_paste_into_item (struct path * path, const char * body, int size);
void reiserfsck_insert_item (struct path * path, struct item_head * ih, const char * body);
void reiserfsck_delete_item (struct path * path);
void reiserfsck_cut_from_item (struct path * path, int cut_size);
typedef	int (comp_function_t)(void * key1, void * key2);
int usearch_by_key (struct super_block * s, struct key * key, struct path * path, int * repeat, int stop_level, int bread_par, 
		   comp_function_t comp_func);
int usearch_by_entry_key (struct super_block * s, struct key * key, struct path * path);
int usearch_by_position (struct super_block * s, struct key * key, struct path * path);
struct key * uget_lkey (struct path * path);
struct key * uget_rkey (struct path * path);
int comp_keys_3 (void * key1, void * key2);
int comp_dir_entries (void * key1, void * key2);


/* bitmap.c */
extern int from_journal;
int reiserfs_new_blocknrs (struct reiserfs_transaction_handle *th, struct super_block * s, unsigned long * free_blocknrs, unsigned long start, int amount_needed, int for_preserve_list);
void reiserfs_free_block (struct reiserfs_transaction_handle *th, struct super_block * s, unsigned long block);
void reiserfs_free_internal_block (struct super_block * s, unsigned long block);
struct buffer_head * reiserfsck_get_new_buffer (unsigned long start);
void force_freeing (void);
int is_block_used (unsigned long block);
int was_block_used (unsigned long block);
void mark_block_used (unsigned long block);
void mark_block_uninsertable (unsigned long block);
int is_block_uninsertable (unsigned long block);
void mark_block_unformatted (unsigned long block);
void mark_block_formatted (unsigned long block);
void unmark_block_unformatted (unsigned long block);
void unmark_block_formatted (unsigned long block);

/* objectid.c */
int is_objectid_used (unsigned long objectid);
void mark_objectid_as_used (unsigned long objectid);
void mark_objectid_as_free (unsigned long objectid);
objectid_t get_unused_objectid (struct super_block * s);



/* segments.c */
struct overwritten_unfm_segment {
  int ous_begin;
  int ous_end;
  struct overwritten_unfm_segment * ous_next;  
};
struct overwritten_unfm * look_for_overwritten_unfm (__u32);
struct overwritten_unfm_segment * find_overwritten_unfm (unsigned long unfm, int length, struct overwritten_unfm_segment * segment_to_init);
int get_unoverwritten_segment (struct overwritten_unfm_segment * list_head, struct overwritten_unfm_segment * unoverwritten_segment);
void save_unfm_overwriting (unsigned long unfm, struct item_head * direct_ih);
void free_overwritten_unfms (void);
void mark_formatted_pointed_by_indirect (__u32);
int is_formatted_pointed_by_indirect (__u32);


/* do_balan.c */
/* lbalance.c */
/* ibalance.c */	/* links to fs/reiser */
/* fix_node.c */
/* teahash3.c */


/* info.c */
struct fsck_stat {
  /* pass 1,2 */
  int fs_good_leaves;
  int fs_uninsertable_leaves;
  int fs_rewritten_files;
  int fs_leaves_used_by_indirect_items;
  int fs_unfm_overwriting_unfm;
  int fs_indirect_to_direct;
  /* pass 3 */
  int fs_incorrect_regular_files;
  int fs_fixed_size_directories;
  int fs_fixed_size_files;
  int fs_deleted_entries;
  /* pass 4 */
  int fs_unaccessed_items;
  int fs_fixed_right_delim_key;
  /* fs stat */
  int fs_stat_data_items;
  int fs_regular_files;
  int fs_directories;
  int fs_symlinks;
  int fs_others;
};
  

extern struct fsck_stat g_fsck_info;

/* pass 1,2 */
#define GOOD_LEAVES 0
#define UNINSERTABLE_LEAVES 1
#define REWRITTEN_FILES 2
#define LEAVES_USED_BY_INDIRECT_ITEMS 3
#define UNFM_OVERWRITING_UNFM 4		/* overwrite contents of unformatted node keeping what has been written there from direct items */

/* pass 3 (semantic) */
#define INCORRECT_REGULAR_FILES 5
#define FIXED_SIZE_DIRECTORIES 6
#define FIXED_SIZE_FILES 7
#define DELETED_ENTRIES 8
#define INDIRECT_TO_DIRECT 9

/* pass 4 */
#define UNACCESSED_ITEMS 10
#define FIXED_RIGHT_DELIM_KEY 11

/* fs stat */
#define STAT_DATA_ITEMS 12
#define REGULAR_FILES 13
#define SYMLINKS 14
#define OTHERS 15
#define DIRECTORIES 16

void add_event (int event);
int get_event (int event);
void output_information ();


/* journal.c */
void replay_all (struct super_block * s);
/*int get_journal_size (struct super_block * s);
int get_journal_start (struct super_block * s);*/
void release_journal_blocks (struct super_block * s);
void reset_journal (struct super_block * s);

