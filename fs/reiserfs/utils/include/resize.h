/* 
 * Copyright 1999 Hans Reiser, see README file for licensing details.
 */

#define print_usage_and_exit()\
 die ("Usage: %s  -s[+|-]#[M|K] [-fqv] device", argv[0])
 

/* reiserfs_resize.c */
extern struct buffer_head * g_sb_bh;

extern int opt_force;
extern int opt_verbose;
extern int opt_nowrite;
extern int opt_safe;

int expand_fs(void);

/* fe.c */
int resize_fs_online(char * devname, unsigned long blocks);

/* do_shrink.c */
int shrink_fs(unsigned long blocks);

/* bitmap.c */
struct bitmap_head {
	int bm_nr;
	int bm_blocksize;
	unsigned long bm_block_count; 
	char ** bm_bmap;
	struct buffer_head ** bm_bh_table;
};

struct bitmap_head * create_bitmap_from_sb (struct buffer_head * sb_bh);
struct bitmap_head * create_bitmap (unsigned long size, int blocksize);
void free_bitmap (struct bitmap_head * bmp);
int sync_bitmap (struct bitmap_head * bmp);
void truncate_bitmap (struct bitmap_head * bmp, unsigned long block);
int is_block_used (struct bitmap_head * bmp, unsigned long block);

#define is_block_free(bmp,block) (!is_block_used(bmp,block))			

void mark_block_free (struct bitmap_head * bmp, unsigned long block);
void mark_block_used (struct bitmap_head * bmp, unsigned long block);
unsigned long find_1st_unused_block_right (struct bitmap_head * bmp,
                                                  unsigned long start);
unsigned long find_1st_unused_block_left (struct bitmap_head * bmp,
                                                  unsigned long start);
			
