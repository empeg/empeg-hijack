/*
 * Copyright 2000 Hans Reiser
 */


//
// ./fs/reiserfs/utils/lib/reiserfs.c
//
int not_formatted_node (char * buf, int blocksize);
int not_data_block (struct super_block * s, b_blocknr_t block);
int uread_super_block (struct super_block * s);
int uread_bitmaps (struct super_block * s);


#define bh_desc(bh) ((struct reiserfs_journal_desc *)((bh)->b_data))
#define bh_commit(bh) ((struct reiserfs_journal_commit *)((bh)->b_data))
int get_journal_start (struct super_block * s);
int get_journal_size (struct super_block * s);
int is_desc_block (struct buffer_head * bh);
int does_desc_match_commit (struct reiserfs_journal_desc * desc, 
			    struct reiserfs_journal_commit * commit);

void make_dir_stat_data (struct key * dir_key, struct item_head * ih,
			 struct stat_data * sd);
void make_empty_dir_item (char * body, objectid_t dirid, objectid_t objid,
			  objectid_t par_dirid, objectid_t par_objid);
