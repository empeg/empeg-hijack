/* Copyright 1999 Hans Reiser, see README file for licensing details.
 *
 * Written by Alexander Zarochentcev.
 *
 * The kernel part of the (on-line) reiserfs resizer.
 */

#ifdef __KERNEL__

#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/locks.h>
#include <linux/string.h>
#include <linux/reiserfs_fs.h>
#include <linux/reiserfs_fs_sb.h>

#else

#include "nokernel.h"

#endif

int reiserfs_resize (struct super_block * s, unsigned long block_count_new)
{
	struct reiserfs_super_block * sb;
	struct buffer_head ** bitmap, * bh;
	struct reiserfs_transaction_handle th;
	unsigned int bmap_nr_new, bmap_nr;
	unsigned int block_r_new, block_r;
	
	struct reiserfs_list_bitmap * jb;
	char * jbitmap[JOURNAL_NUM_BITMAPS];
	
	unsigned long int block_count, free_blocks;
	int i,j; 

	sb = SB_DISK_SUPER_BLOCK(s);

	if (SB_BLOCK_COUNT(s) >= block_count_new) {
		printk("can\'t shrink filesystem on-line\n");
		return 1;
	}

	/* check the device size */
	bh = bread(s->s_dev, block_count_new - 1, s->s_blocksize);
	if (!bh) {
		printk("reiserfs_resize: can\'t read last block\n");
		return 1;
	}	
	brelse(bh);

	/* old disk layout detection; those partitions can be mounted, but
	 * cannot be resized */
	if (SB_BUFFER_WITH_SB(s)->b_blocknr *	SB_BUFFER_WITH_SB(s)->b_size 
		!= REISERFS_DISK_OFFSET_IN_BYTES ) {
		printk("reiserfs_resize: unable to resize a reiserfs without distributed bitmap (fs version < 3.5.12)\n");
		return 1;
	}

	
	/* count used bits in last bitmap block */
	block_r = le32_to_cpu(SB_BLOCK_COUNT(s)) -
	        (le16_to_cpu(SB_BMAP_NR(s)) - 1) * s->s_blocksize * 8;
	
	/* count bitmap blocks in new fs */
	bmap_nr_new = block_count_new / ( s->s_blocksize * 8 );
	block_r_new = block_count_new - bmap_nr_new * s->s_blocksize * 8;
	if (block_r_new) 
		bmap_nr_new++;
	else
		block_r_new = s->s_blocksize * 8;

	/* save old values */
	block_count = le32_to_cpu(SB_BLOCK_COUNT(s));
	bmap_nr     = le16_to_cpu(SB_BMAP_NR(s));
	free_blocks = le32_to_cpu(SB_FREE_BLOCKS(s));

	/* begin transaction */
	lock_super(s);
	journal_begin(&th, s, 10);

	/* reallocate journal bitmaps */
	for (i = 0 ; i < JOURNAL_NUM_BITMAPS ; i++) {
		jb = SB_JOURNAL(s)->j_list_bitmap + i;
		jbitmap[i] = vmalloc(block_count_new / 8);
		if (!jbitmap[i]) {
			printk("reiserfs_resize: unable to allocate memory for journal bitmaps\n");
			while (--i >= 0) 
				vfree(jbitmap[i]);
			return 1;
		}
		memset(jbitmap[i], 0, block_count_new / 8);
		memcpy(jbitmap[i], jb->bitmap, block_count / 8);
		for(j = bmap_nr; j < bmap_nr_new ; j++)
			set_bit(j * s->s_blocksize * 8, jbitmap[i]);
	}
	for (i = 0 ; i < JOURNAL_NUM_BITMAPS ; i++) {
		jb = SB_JOURNAL(s)->j_list_bitmap + i;
		vfree(jb->bitmap);
		jb->bitmap = jbitmap[i];
	}	
	
	/* allocate additional bitmap blocks, reallocate array of bitmap
	 * block pointers */
	if (bmap_nr_new > bmap_nr) {
		bitmap = reiserfs_kmalloc(sizeof(struct buffer_head *) * bmap_nr_new,
								   GFP_KERNEL, s);
		if (!bitmap) {
			printk("reiserfs_resize: unable to allocate memory.\n");
			journal_end(&th, s, 10);
			unlock_super(s);
			return 1;
		}
		for (i = 0; i < bmap_nr; i++)
			bitmap[i] = SB_AP_BITMAP(s)[i];
		for (i = bmap_nr; i < bmap_nr_new; i++) {
			bitmap[i] = getblk(s->s_dev, i * s->s_blocksize * 8, s->s_blocksize);
			if(!bitmap[i]) {
				printk("reiserfs_resize: getblk() failed");
				while (--i >= bmap_nr) 
					brelse(bitmap[i]);
				reiserfs_kfree(bitmap, 
					sizeof(struct buffer_head *) * bmap_nr_new, s);
				journal_end(&th, s, 10);
				unlock_super(s);
				return 1;
			}
			memset(bitmap[i]->b_data, 0, sb->s_blocksize);
			set_bit(0, bitmap[i]->b_data);
		}	
		/* free old bitmap blocks array */
		reiserfs_kfree(SB_AP_BITMAP(s), 
			sizeof(struct buffer_head *) * bmap_nr, s);
		SB_AP_BITMAP(s) = bitmap;
	}

	/* correct last bitmap blocks in old and new disk layout */
	for (i = block_r; i < s->s_blocksize * 8; i++)
		clear_bit(i, SB_AP_BITMAP(s)[bmap_nr - 1]->b_data);
	for (i = block_r_new; i < s->s_blocksize * 8; i++)
		set_bit(i, SB_AP_BITMAP(s)[bmap_nr_new - 1]->b_data);
	
	for (i = bmap_nr - 1; i < bmap_nr_new; i++)  
		journal_mark_dirty_nolog(&th, s, SB_AP_BITMAP(s)[i]);
/*	journal_mark_dirty(&th, s, SB_AP_BITMAP(s)[bmap_nr - 1]);
 */
	sb->s_free_blocks = cpu_to_le32(free_blocks + (block_count_new 
		- block_count - (bmap_nr_new - bmap_nr)));
	sb->s_block_count = cpu_to_le32(block_count_new);
	sb->s_bmap_nr = cpu_to_le16(bmap_nr_new);
	s->s_dirt = 1;

	journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB(s));
	
	journal_end(&th, s, 10);
	unlock_super(s);

	return 0;
}

