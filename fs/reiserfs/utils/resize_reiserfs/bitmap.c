/*
 * Copyright 1999,2000 Hans Reiser, see README file for licensing details.
 * written by Alexander Zarochentcev <zam@programbank.ru>
 * 
 * This file contains small functons which operate with bitmaps:
 * create, delete, copy, bit operations, etc.
 * There are two bitmap types: general bitmap and bitmap with associated
 * disk blocks from a reiserfs.
 *
 */

#include <sys/vfs.h>
#include <sys/types.h>
#include <asm/types.h>
 
#include "inode.h"
#include "io.h"
#include "sb.h"
#include "misc.h"
#include "reiserfs_fs.h"

#include "resize.h"

/* sorry, I can't reuse code from fsck/ubitmap.c */

struct bitmap_head * create_bitmap (unsigned long size, int blocksize) 
{
	struct bitmap_head * bmp;
	int i, bmap_nr;
	bmp = getmem(sizeof(struct bitmap_head));
	bmp->bm_block_count = size;
	bmp->bm_blocksize = blocksize;
	bmp->bm_bh_table = NULL;
	bmp->bm_nr = (size - 1) / blocksize + 1;
	for (i = 0; i < bmp->bm_nr; i++)
		bmp->bm_bmap [i] = getmem(blocksize);
	return bmp;				
}

struct bitmap_head * create_bitmap_from_sb (struct buffer_head * sb_bh)
{
	struct bitmap_head * bmp;
	struct reiserfs_super_block * sb;
	int i;

	sb = (struct reiserfs_super_block *) sb_bh->b_data;
	
	bmp = getmem(sizeof(struct bitmap_head));
	bmp->bm_blocksize = sb->s_blocksize;
	bmp->bm_block_count = sb->s_block_count;
	bmp->bm_bh_table = NULL;
	bmp->bm_nr = 0;

	bmp->bm_bh_table = getmem(sizeof(struct buffer_head *) * sb->s_bmap_nr);
	bmp->bm_bmap = getmem(sizeof(char *) * sb->s_bmap_nr);
	
	/* read first bitmap block */
	bmp->bm_bh_table [0] = bread(sb_bh->b_dev,
	 		   REISERFS_DISK_OFFSET_IN_BYTES / sb->s_blocksize + 1,
			   sb->s_blocksize);
	if (!bmp->bm_bh_table [0]) {
		free_bitmap(bmp);
		return NULL;
	}
	bmp->bm_bmap [0] = bmp->bm_bh_table [0] -> b_data;
	bmp->bm_nr++;
		
	/* read others bitmap blocks */	
	for (i=1; i < sb->s_bmap_nr; i++) {
		bmp->bm_bh_table [i] = bread(sb_bh->b_dev, i * sb->s_blocksize * 8, sb->s_blocksize);
		if (!bmp->bm_bh_table [i]) {
			free_bitmap(bmp);
			return NULL;
		}
		bmp->bm_bmap [i] = bmp->bm_bh_table [i] -> b_data;
		bmp->bm_nr++;
	}	
	return bmp;	
}

void free_bitmap (struct bitmap_head * bmp)
{
	int i;
	
	if (bmp->bm_bh_table) {
		for (i = 0; i < bmp->bm_nr; i++ )
			brelse(bmp->bm_bh_table [i]);
		freemem(bmp->bm_bh_table);
	} else {
		if(bmp->bm_bmap)
			for (i = 0; i < bmp->bm_nr; i++)
				freemem(bmp->bm_bmap [i]);
	}
	if(bmp->bm_bmap)
		freemem(bmp->bm_bmap);
	freemem(bmp);
}

int sync_bitmap (struct bitmap_head * bmp)
{
	int i;
	if (bmp->bm_bh_table)
		for (i = 0; i < bmp->bm_nr; i++) {
			bwrite(bmp->bm_bh_table [i]); 
		}	
	return 0;
}

void truncate_bitmap (struct bitmap_head * bmp, unsigned long block)
{
	int i,j;

	i = (block - 1) / (8 * bmp->bm_blocksize);
	j = block - i * 8 * bmp->bm_blocksize;

	while (j < (8 * bmp->bm_blocksize))
		set_bit(j++, bmp->bm_bh_table [i] -> b_data);
	mark_buffer_dirty(bmp->bm_bh_table [i], 1);	
	
	for (j = i + 1; j < bmp->bm_nr; j++)
		brelse(bmp->bm_bh_table [j]);

	bmp->bm_nr = i + 1;
	bmp->bm_block_count = block;
}


int is_block_used (struct bitmap_head * bmp, unsigned long block)
{
	int i,j;

	i = block / (8 * bmp->bm_blocksize);
	j = block % (8 * bmp->bm_blocksize);

	return test_bit(j, bmp->bm_bmap [i]);
}

void mark_block_free (struct bitmap_head * bmp, unsigned long block)
{
	int i,j;

    i = block / (8 * bmp->bm_blocksize);
    j = block % (8 * bmp->bm_blocksize);
	
	clear_bit(j, bmp->bm_bmap [i]);
	if (bmp->bm_bh_table) 
		mark_buffer_dirty(bmp->bm_bh_table [i], 1);
}

void mark_block_used (struct bitmap_head * bmp, unsigned long block)
{
	int i,j;
	
	i = block / (8 * bmp->bm_blocksize);
	j = block % (8 * bmp->bm_blocksize);

	set_bit(j, bmp->bm_bmap [i]);
	if (bmp->bm_bh_table) 
		mark_buffer_dirty(bmp->bm_bh_table [i], 1);
}

unsigned long find_1st_unused_block_right (struct bitmap_head * bmp,
					 							  unsigned long start)
{
	for(;start < bmp->bm_block_count; start++)
		if (is_block_free(bmp, start))
			return start;
	return 0;		
}

unsigned long find_1st_unused_block_left (struct bitmap_head * bmp,
					 							  unsigned long start)
{
	while (--start)
		if (is_block_free(bmp, start))
			return start;
	return 0;		
}

