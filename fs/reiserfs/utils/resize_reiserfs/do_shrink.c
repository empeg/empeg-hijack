/*
 * Copyright 2000 Hans Reiser, see README file for licensing details.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <asm/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/vfs.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mount.h>

#include "inode.h"
#include "io.h"
#include "sb.h"
#include "misc.h"
#include "reiserfs_fs.h"
#include "resize.h"

static long int_node_cnt   = 0, int_moved_cnt   = 0;
static long	leaf_node_cnt  = 0, leaf_moved_cnt  = 0;
static long	unfm_node_cnt  = 0, unfm_moved_cnt  = 0;
static long	total_node_cnt = 0, total_moved_cnt = 0;

static unsigned long unused_block;
static unsigned long blocks_used;
static struct bitmap_head * bmp;

/* abnornal exit from block reallocation process */
static void quit_resizer()
{
	/* save changes to bitmap blocks */
	sync_bitmap (bmp);
	free_bitmap (bmp);
	/* leave fs in ERROR state */
	brelse(g_sb_bh);
	die ("resize_reiserfs: fs shrinking was not completed successfully, run reiserfsck.\n");
}

/* block moving */
static unsigned long move_generic_block(unsigned long block,
										unsigned long bnd, int h)
{
	struct buffer_head * bh, * bh2;

	/* primitive fsck */
	if (block > ((struct reiserfs_super_block *)(g_sb_bh->b_data))->s_block_count) {
		fprintf(stderr, "resize_reiserfs: invalid block number (%lu) found.\n", block);
		quit_resizer();
	}
	/* progress bar, 3D style :) */
	if (opt_verbose)	
		print_how_far((__u32 *)&total_node_cnt, blocks_used);
	else
		total_node_cnt ++;
	/* infinite loop check */
	if( total_node_cnt > blocks_used) {
		fputs("resize_reiserfs: block count exeeded\n",stderr);
		quit_resizer();
	}

	if (block < bnd) /* block will not be moved */
		return 0;
	
	/* move wrong block */ 
	bh = bread(g_sb_bh->b_dev, block, g_sb_bh->b_size);

	unused_block = find_1st_unused_block_right(bmp, unused_block);
	if (unused_block == 0 || unused_block >= bnd) {
		fputs ("resize_reiserfs: can\'t find free block\n", stderr);
		quit_resizer();
	}

	/* blocknr changing */
	bh2 = getblk(g_sb_bh->b_dev, unused_block, g_sb_bh->b_size);
	memcpy(bh2->b_data, bh->b_data, bh2->b_size);
	mark_block_free(bmp, block);
	mark_block_used(bmp, unused_block);

	brelse(bh);
	mark_buffer_dirty(bh2,0);
	mark_buffer_uptodate(bh2,0);
	bwrite(bh2);
	brelse(bh2);

	total_moved_cnt++;
	return unused_block;
}

static unsigned long move_unformatted_block(unsigned long block,
											unsigned long bnd, int h)
{
	unsigned long b;
	unfm_node_cnt++;
	b = move_generic_block(block, bnd, h);
	if (b)
		unfm_moved_cnt++;
	return b;		
}


/* recursive function processing all tree nodes */
static unsigned long move_formatted_block(unsigned long block,
										  unsigned long bnd, int h)
{
	struct buffer_head * bh;
	struct item_head *ih;
	unsigned long new_blocknr = 0;
	int dev;
	int i, j;
	
	dev = g_sb_bh -> b_dev;
	
	bh = bread(dev, block, g_sb_bh->b_size);

	if (B_IS_ITEMS_LEVEL(bh)) { /* leaf node*/

		leaf_node_cnt++;

		for (i=0; i < B_NR_ITEMS(bh); i++) {
			ih = B_N_PITEM_HEAD(bh, i);
			if (I_IS_INDIRECT_ITEM(ih)) {
				for (j = 0; j < I_UNFM_NUM(ih); j++) {
					unsigned long  unfm_block;
					/* unfm_block_ptr = (unsigned long *)() + j; */
					unfm_block = move_unformatted_block(
									B_I_POS_UNFM_POINTER(bh, ih, j),
									bnd, h + 1);
					if (unfm_block) {
						B_I_POS_UNFM_POINTER(bh,ih,j) = unfm_block;
						mark_buffer_dirty(bh,0);
					}
				}
			}	
		}
		mark_buffer_uptodate(bh,0);
		bwrite(bh);
		brelse(bh);
		new_blocknr = move_generic_block(block, bnd, h);
		if (new_blocknr)
			leaf_moved_cnt++;
	} else if (B_IS_KEYS_LEVEL(bh)) { /* internal node */

		int_node_cnt++;

		for (i=0; i <= B_NR_ITEMS(bh); i++) {
			unsigned long moved_block;
			moved_block = move_formatted_block(B_N_CHILD_NUM(bh, i), bnd, h+1);
			if (moved_block) {
				B_N_CHILD_NUM(bh, i) = moved_block;
				mark_buffer_dirty(bh,0);
			}
		}	
		mark_buffer_uptodate(bh,0);
		bwrite(bh);
		brelse(bh);	
		new_blocknr = move_generic_block(block, bnd, h);
		if (new_blocknr)
			int_moved_cnt++;
	} else {
		die ("resize_reiserfs: block (%lu) have invalid format\n", block);
	}

	return new_blocknr;
}

static void sync_super_block()
{
	mark_buffer_dirty(g_sb_bh,0);
	mark_buffer_uptodate(g_sb_bh,0);
	bwrite(g_sb_bh);
}

int shrink_fs(unsigned long blocks)
{
	struct reiserfs_super_block *  sb;
	unsigned long n_root_block;
	int bmap_nr_new;

	/* warn about alpha version */
	{
		int c;

		printf(
			"You are running ALPHA version of reiserfs shrinker.\n"
			"This version is only for testing or VERY CAREFUL use.\n"
			"Backup of you data is recommended.\n\n"
			"Do you want to continue? [y/N]:"
			);
		c = getchar();
		if (c != 'y' && c != 'Y')
			exit(1);
	}
	sb = (struct reiserfs_super_block *) g_sb_bh->b_data;
	bmap_nr_new = (blocks - 1) / (8 * sb->s_blocksize) + 1;
	
	/* is shrinking possible ? */
	if (sb->s_block_count - blocks > 
		sb->s_free_blocks + sb->s_bmap_nr - bmap_nr_new)
		die ("resize_reiserfs: can\'t shrink fs; too many blocks already allocated\n"); 
	/* calculate number of data blocks */		
	blocks_used = 
		sb->s_block_count
		- sb->s_free_blocks
		- sb->s_bmap_nr 
		- sb->s_orig_journal_size
		- REISERFS_DISK_OFFSET_IN_BYTES / sb->s_blocksize
		- 2; /* superblock itself and 1 descriptor after the journal */

	bmp = create_bitmap_from_sb(g_sb_bh);
	if (!bmp) 
		die ("resize_reiserfs: read bitmap failed\n");
	unused_block = 1;

	/* change fs state before shrinking */
	sb->s_state = REISERFS_ERROR_FS;
	sync_super_block();

	if (opt_verbose) {
		printf("Processing the tree: ");
		fflush(stdout);
	}

	n_root_block = move_formatted_block(sb->s_root_block, blocks, 0);
	if (n_root_block) {
		sb->s_root_block = n_root_block;
	}

	if (opt_verbose)
		printf ("\n\nnodes processed (moved):\n"
				"int        %lu (%lu),\n"
				"leaves     %lu (%lu),\n" 
				"unfm       %lu (%lu),\n"
				"total      %lu (%lu).\n\n",
				int_node_cnt, int_moved_cnt,
				leaf_node_cnt, leaf_moved_cnt, 
				unfm_node_cnt, unfm_moved_cnt,
				total_node_cnt, total_moved_cnt);
	
#if 0
	printf("check for used blocks in truncated region\n");
	{
		long l;
		for (l = blocks; l < sb->s_block_count; l++)
			if (is_block_used(bmp,l))
				printf("<%lu>", l);
		printf("\n");
	}
#endif
		
	sb->s_free_blocks -= (sb->s_block_count - blocks) 
							- (sb->s_bmap_nr - bmap_nr_new);
	sb->s_block_count = blocks;
	sb->s_bmap_nr = bmap_nr_new;
	
	truncate_bitmap(bmp, blocks);
	sync_bitmap(bmp);
	free_bitmap(bmp);

	/* change fs state after shrinking */
	sb->s_state = REISERFS_VALID_FS;

	sync_super_block();

	brelse(g_sb_bh);
	return 0;
}
