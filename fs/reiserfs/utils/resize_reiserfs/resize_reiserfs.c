/* Copyright 1999 Hans Reiser, see README file for licensing details.
 * 
 * Written by Alexander Zarochentcev.
 * 
 * FS resize utility 
 *
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

struct buffer_head * g_sb_bh;
				/* use of a long is a 2.2 Linux VFS
                                   limitation, review this decision for
                                   2.3 and/or LFS patch. -Hans */
unsigned long g_block_count_new;
int g_bmap_nr_new;

int opt_force = 0;
int opt_verbose = 1;			/* now "verbose" option is default */
int opt_nowrite = 0;
int opt_safe = 0;

/* Given a file descriptor and an offset, check whether the offset is
   a valid offset for the file - return 0 if it isn't valid or 1 if it
   is */
int valid_offset( int fd, loff_t offset )
{
  char ch;

  if (reiserfs_llseek (fd, offset, 0) < 0)
    return 0;

  if (read (fd, &ch, 1) < 1)
    return 0;

  return 1;
}

				/* A bunch of these functions look like
                                   they could be shared with those in
                                   super.c or the utils, can they?
                                   If so, then do so.  -Hans */
static void read_superblock(int dev) {
	int bs;
	struct reiserfs_super_block * sb;
		
	g_sb_bh = bread(dev, (REISERFS_DISK_OFFSET_IN_BYTES / 1024), 1024);
	if (!g_sb_bh)
		die ("resize_reiserfs: can\'t read superblock\n");
	sb = (struct reiserfs_super_block *)g_sb_bh->b_data;

	if(strncmp(sb->s_magic, REISERFS_SUPER_MAGIC_STRING, sizeof(REISERFS_SUPER_MAGIC_STRING) - 1) ) 
        die ("resize_reiserfs: device doesn\'t contain valid reiserfs\n");
							
	bs = sb->s_blocksize;	
	brelse(g_sb_bh);
	
	g_sb_bh = bread(dev, REISERFS_DISK_OFFSET_IN_BYTES / bs, bs);
	if (!g_sb_bh)
		die ("resize_reiserfs: can\'t read superblock\n");
	if (g_sb_bh->b_blocknr >= sb->s_journal_block)
		die ("resize_reiserfs: can\'t read superblock\n");
}

/* calculate the new fs size (in blocks) from old fs size and the string
   representation of new size */
static unsigned long calc_new_fs_size(unsigned long count, 
								      int bs, char *bytes_str) {
	long long int bytes;
	unsigned long blocks;
	int c;
	
	bytes = atoll(bytes_str);
	c = bytes_str[strlen(bytes_str) - 1];

	switch (c) {
	case 'M':
	case 'm':
		bytes *= 1024;
	case 'K':
	case 'k':
		bytes *= 1024;
	}
	
	blocks = bytes / bs;

	if (bytes_str[0] == '+' || bytes_str[0] == '-')
		return (count + blocks);

	return blocks;
}

/* print some fs parameters */
static void sb_report(struct reiserfs_super_block * sb1,
       			      struct reiserfs_super_block * sb2){
	printf(
		"ReiserFS report:\n"
		"blocksize             %d\n"
		"block count           %d (%d)\n"
		"free blocks           %d (%d)\n"
		"bitmap block count    %d (%d)\n", 
		sb1->s_blocksize,
		sb1->s_block_count, sb2->s_block_count,
		sb1->s_free_blocks, sb2->s_free_blocks,
		sb1->s_bmap_nr, sb2->s_bmap_nr);
};

/* read i-th bitmap block */
static struct buffer_head * get_bm_blk (int dev, int ind, int bs) {
	if (ind == 0) 
		return bread(g_sb_bh->b_dev, REISERFS_DISK_OFFSET_IN_BYTES / bs + 1 ,bs);
	return bread(dev, ind * bs * 8, bs);
}

/* conditional bwrite */
static int bwrite_cond (struct buffer_head * bh) {
	if(!opt_nowrite) { 
		mark_buffer_uptodate(bh,0);
		mark_buffer_dirty(bh,0);
		return bwrite(bh);
	}
	return 0;
}


/* the first one of the mainest functions */
int expand_fs(void) {
	struct reiserfs_super_block *  sb;
	struct buffer_head * bm_bh;
	int block_r, block_r_new;
	int i;
	
	sb = (struct reiserfs_super_block *) g_sb_bh->b_data;

	/* count used bits in last bitmap block */
	block_r = sb->s_block_count -
		((sb->s_bmap_nr - 1) * sb->s_blocksize * 8);
	
	/* count bitmap blocks in new fs */
	g_bmap_nr_new = g_block_count_new / (sb->s_blocksize * 8);
	block_r_new = g_block_count_new -
		g_bmap_nr_new * sb->s_blocksize	* 8;
	if(block_r_new)
		g_bmap_nr_new++;
	else 
		block_r_new = sb->s_blocksize * 8;

	/* clear bits in last bitmap block (old layout) */
	bm_bh = get_bm_blk(g_sb_bh->b_dev, sb->s_bmap_nr - 1, sb->s_blocksize);
	for (i = block_r; i < sb->s_blocksize * 8; i++)
		clear_bit(i, bm_bh->b_data);
	bwrite_cond(bm_bh);
	
	/* add new bitmap blocks */
	for (i = sb->s_bmap_nr; i < g_bmap_nr_new; i++) {
		memset(bm_bh->b_data, 0, bm_bh->b_size);
		set_bit(0, bm_bh->b_data);
		bm_bh->b_blocknr =  			/* It is not a first BM block */
			i * sb->s_blocksize * 8;	/* with special location */
		bwrite_cond(bm_bh);
	}
	
	/* set unused bits in last bitmap block (new layout) */
	for (i = block_r_new; i < sb->s_blocksize * 8; i++)
		set_bit(i, bm_bh->b_data);
	bwrite_cond(bm_bh);

	/* update super block buffer*/
	sb->s_free_blocks += g_block_count_new - sb->s_block_count
		- (g_bmap_nr_new - sb->s_bmap_nr);
	sb->s_block_count = g_block_count_new;
	sb->s_bmap_nr = g_bmap_nr_new;

	/* commit changes */
	bwrite_cond(g_sb_bh);

	brelse(g_sb_bh);
	brelse(bm_bh);
	
	return 0;
}

int main(int argc, char *argv[]) {
	char * bytes_count_str = NULL;
	char * devname;
	struct stat statbuf;
	int c;

	int dev;
	struct reiserfs_super_block *sb, *sb_old;
	
	while ((c = getopt(argc, argv, "fvcqs:")) != EOF) {
		switch (c) {
		case 's' :
			  if (!optarg) 
				  die("%s: Missing argument to -s option", argv[0]);		
			  bytes_count_str = optarg;
			  break;
		case 'f':
		    opt_force = 1;
		    break;		 
		case 'v':
			opt_verbose++; 
			break;
		case 'n':
			/* no nowrite option at this moment */
			/* opt_nowrite = 1; */
			break;
		case 'c':
			opt_safe = 1;
			break;
		case 'q':
			opt_verbose = 0;
			break;
		default:
			print_usage_and_exit ();
		}
	}

	if (optind == argc || (!bytes_count_str))
		print_usage_and_exit();
	devname = argv[optind];

	/* open_device will die if it could not open device */
	dev = open (devname, O_RDWR);
	if (dev == -1)
		die ("%s: can not open '%s': %s", argv[0], devname, strerror (errno));

	if (fstat (dev, &statbuf) < 0)
		die ("%s: unable to stat %s", argv[0], devname);
  
	if (!S_ISBLK (statbuf.st_mode) && opt_force )
		die ("%s: '%s (%o)' is not a block device", 
			 argv[0], devname, statbuf.st_mode);

	read_superblock(dev);
	
	sb = (struct reiserfs_super_block *) g_sb_bh->b_data;
	g_block_count_new = calc_new_fs_size(sb->s_block_count,
					     sb->s_blocksize, bytes_count_str);
	if (is_mounted (devname)) {
		close(dev);
		if (!opt_force) 
	    	die ("%s: '%s' contains a mounted file system,\n"
			     "\tspecify -f option to resize the fs online\n", 
				 argv[0], devname);
		resize_fs_online(devname, g_block_count_new);
		return 0;
	}	

	if (sb->s_state != REISERFS_VALID_FS) 
		die ("%s: the file system isn't in valid state\n", argv[0]);
		
	if(!valid_offset(dev, (loff_t) g_block_count_new * sb->s_blocksize - 1))
		die ("%s: %s too small", argv[0], devname);

	sb_old = 0;		/* Needed to keep idiot compiler from issuing false warning */
	/* save SB for reporting */
	if(opt_verbose) {
		sb_old = getmem(sizeof(struct reiserfs_super_block));
		memcpy(sb_old, sb, sizeof(struct reiserfs_super_block));
    }

	if (g_block_count_new == sb->s_block_count) 
		die ("%s: Calculated fs size is the same as the previous one.",
			 argv[0]);
	if (g_block_count_new > sb->s_block_count) 
		expand_fs();
	else
		shrink_fs(g_block_count_new);

	if(opt_verbose) {
		sb_report(sb, sb_old);
		freemem(sb_old);
	}

	check_and_free_mem ();		
	
	if (opt_verbose) {
		printf("\nSyncing..");
		fflush(stdout);
	}
	fsync (dev);
	if (opt_verbose)
		printf("done\n");
	

	close(dev);
	
	return 0;
}
