/*
 * Copyright 1996-1999 Hans Reiser
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
#include <netinet/in.h>

#include "inode.h"
#include "sb.h"
#include "io.h"
#include "misc.h"
#include "reiserfs_fs.h"
#include "reiserfs.h"


int g_new_internals;


#define print_usage_and_exit() die ("Usage: %s [-b block-to-print][-idc] device\n\
-i Causes to print all items of a leaf\n\
-d                 content of directory items\n\
-c                 content of direct items\n\
-m                 bitmap blocks\n", argv[0]);



struct reiserfs_fsstat {
  int nr_internals;
  int nr_leaves;
  int nr_files;
  int nr_directories;
  int nr_unformatted;
} g_stat_info;
int g_comp_number = 0;

/*
 *  options
 */
int opt_print_regular_file_content = 0;/* -c */
int opt_print_directory_contents = 0;	/* -d */
int opt_print_leaf_items = 0;		/* -i */
int opt_print_objectid_map = 0;	/* -o */
int opt_print_block_map = 0;		/* -m */
/* when you want print one block specify -b # */
int opt_block_to_print = -1;
int opt_pack = 0;	/* -P will produce output that should be |gzip -c > whatever.gz */
			/* -p will calculate number of bytes needed to transfer the partition */
int opt_print_journal;
int opt_pack_all = 0;

struct super_block g_sb;



int print_mode (void)
{
    int mode = 0;

    if (opt_print_leaf_items == 1)
	mode |= PRINT_LEAF_ITEMS;
    if (opt_print_directory_contents == 1)
	mode |= (PRINT_LEAF_ITEMS | PRINT_DIRECTORY_ITEMS);
    if (opt_print_regular_file_content == 1)
	mode |= (PRINT_LEAF_ITEMS | PRINT_DIRECT_ITEMS);
    return mode;
}


void print_disk_tree (int block_nr)
{
    struct buffer_head * bh;

    bh = bread (g_sb.s_dev, block_nr, g_sb.s_blocksize);
    if (B_IS_KEYS_LEVEL (bh)) {
	int i;
	struct disk_child * dc;

	g_stat_info.nr_internals ++;
	print_block (bh, print_mode (), -1, -1);
      
	dc = B_N_CHILD (bh, 0);
	for (i = 0; i <= B_NR_ITEMS (bh); i ++, dc ++)
	    print_disk_tree (dc->dc_block_number);

    } else if (B_IS_ITEMS_LEVEL (bh)) {
	g_stat_info.nr_leaves ++;
	print_block (bh, print_mode (), -1, -1);
    } else {
	print_block (bh, print_mode (), -1, -1);
	die ("print_disk_tree: bad block type");
    }
    brelse (bh);
}



void print_one_block (int block)
{
    struct buffer_head * bh;
    
    if (test_bit (block % (g_sb.s_blocksize * 8), 
		  SB_AP_BITMAP (&g_sb)[block / (g_sb.s_blocksize * 8)]->b_data))
	printf ("%d is used in true bitmap\n", block);
    else
	printf ("%d is free in true bitmap\n", block);

    bh = bread (g_sb.s_dev, block, g_sb.s_blocksize);
    if (!not_formatted_node (bh->b_data, g_sb.s_blocksize))
	print_block (bh, PRINT_LEAF_ITEMS | PRINT_DIRECTORY_ITEMS | (opt_print_regular_file_content == 1 ? PRINT_DIRECT_ITEMS : 0), -1, -1);
    else
	printf ("Looks like unformatted\n");
    brelse (bh);
    return;
}


static char * parse_options (int argc, char * argv [])
{
    int c;
    char * tmp;
  
    while ((c = getopt (argc, argv, "b:icdmoMpPaAj")) != EOF) {
	switch (c) {
	case 'b':	/* print a single node */
	    opt_block_to_print = strtol (optarg, &tmp, 0);
	    if (*tmp)
		die ("parse_options: bad block size");
	    break;

	case 'p':	/* calculate number of bytes, that need to be transfered */
	    opt_pack = 'c'; break;
	case 'P':	/* dump a partition */
	    opt_pack = 'p'; break;
	case 'a':
	    opt_pack_all = 'c'; break;
	case 'A':
	    opt_pack_all = 'p'; break;

	case 'i':	/* print items of a leaf */
	    opt_print_leaf_items = 1; break;

	case 'd':	/* print directories */
	    opt_print_directory_contents = 1; break;

	case 'c':	/* print contents of a regular file */
	    opt_print_regular_file_content = 1; break;

	case 'o':	/* print a objectid map */
	    opt_print_objectid_map = 1; break;

	case 'm':	/* print a block map */
	    opt_print_block_map = 1;  break;
	case 'M':	/* print a block map with details */
	    opt_print_block_map = 2;  break;
	case 'j':
	    opt_print_journal = 1; break; /* print transactions */
	}
    }
    if (optind != argc - 1)
	/* only one non-option argument is permitted */
	print_usage_and_exit();
  
    return argv[optind];
}


/* journal has permanent location (currently) (after first bitmap
   block and constant size (JOURNAL_BLOCK_COUNT + 1) */
int journal_block (struct super_block * s, __u32 block)
{
/*
    if (block > SB_AP_BITMAP (s)[0]->b_blocknr &&
	block < SB_AP_BITMAP (s)[0]->b_blocknr + JOURNAL_BLOCK_COUNT + 1)
	return 1;
*/
    if (block >= SB_JOURNAL_BLOCK (s) &&
	block <= SB_JOURNAL_BLOCK (s) + s->u.reiserfs_sb.s_rs->s_orig_journal_size + 1)
	return 1;
    return 0;
}


int data_block (struct super_block * s, __u32 block)
{
    int i;

    if (block == REISERFS_DISK_OFFSET_IN_BYTES / s->s_blocksize)
      /* super block, not data block */
      return 0;

    for (i = 0; i < SB_BMAP_NR (s); i ++)
	if (block == SB_AP_BITMAP (s)[i]->b_blocknr)
	    /* bitmap block, not data block */
	    return 0;

    if (journal_block (s, block))
	return 0;

    return 1;
}


/* this dumps file sustem to stdout as a such way: 
   16 bit blocksize
   32 bit blocknumber
   16 bit - record length
   the record of given length
   ..

   to pack :   print_disk_layout -p /dev/xxx | gzip -c > xxx.gz
   to unpack : zcat xxx.gz | unpackreiserfs /dev/xxx
*/


static int get_total_block_number (void)
{
  int i, j;
  int retval = 0;
    
  retval = 0;
    
  if (opt_pack_all)
    retval = SB_BLOCK_COUNT (&g_sb);
  else {
    for (i = 0; i < SB_BMAP_NR (&g_sb); i ++) {
      for (j = 0; j < g_sb.s_blocksize * 8; j ++)
	if (i * g_sb.s_blocksize * 8 + j < SB_BLOCK_COUNT (&g_sb) &&
	    test_bit (j, SB_AP_BITMAP (&g_sb)[i]->b_data))
	  retval ++;
    }
  }
  return retval;
}


int direct_items = 0, direct_item_total_length = 0;
int items = 0;
int unreachable_items = 0;

/* fill direct items with 0s */
static void zero_direct_items (char * buf)
{
    int i;
    struct item_head * ih;

    if (((struct block_head *)buf)->blk_level != DISK_LEAF_NODE_LEVEL)
	return;

    /* leaf node found */
    ih = (struct item_head *)(buf + BLKH_SIZE);

    for (i = 0; i < ((struct block_head *)buf)->blk_nr_item; i ++, ih ++) {
	if (I_IS_DIRECT_ITEM (ih)) {
	    /* FIXME: do not zero symlinks */
	    direct_items ++;
	    direct_item_total_length += ih->ih_item_len;
	    memset (buf + ih->ih_item_location, 0, ih->ih_item_len);
	}
	items ++;
	if (ih->u.ih_free_space == 0xffff)
	    unreachable_items ++;
    }
}



void pack_partition (struct super_block * s)
{
    int i, j, k;
    uint32_t blocknumber32;
    uint16_t reclen16, data16;
    __u32 done = 0;
    char * data;
    long long bytes_to_transfer = 0;
    struct buffer_head * bh;
    int total_block_number;


    total_block_number = get_total_block_number ();
    

    /* write filesystem's block size to stdout as 16 bit number */
    reclen16 = htons (s->s_blocksize);
    if (opt_pack == 'p' || opt_pack_all == 'p')
	write (1, &reclen16, sizeof (uint16_t));
    bytes_to_transfer = sizeof (uint16_t);

    /* go through blocks which are marked used in cautious bitmap */
    for (i = 0; i < SB_BMAP_NR (s); i ++) {
	for (j = 0; j < s->s_blocksize; j ++) {
	    /* make sure, that we are not out of the device */
	    if (i * s->s_blocksize * 8 + j * 8 == SB_BLOCK_COUNT (s))
		goto out_of_bitmap;

	    if (i * s->s_blocksize * 8 + j * 8 + 8 > SB_BLOCK_COUNT (s))
		die ("build_the_tree: Out of bitmap");

	    if (opt_pack_all == 0)
		if (SB_AP_BITMAP (s)[i]->b_data[j] == 0) {
		    /* skip busy block if 'a' not specified */
		    continue;
		}

	    /* read 8 blocks at once */
	    bh = bread (s->s_dev, i * s->s_blocksize + j, s->s_blocksize * 8);
	    for (k = 0; k < 8; k ++) {
		__u32 block;
		
		block = i * s->s_blocksize * 8 + j * 8 + k;
		
		if (opt_pack_all == 0 && (SB_AP_BITMAP (s)[i]->b_data[j] & (1 << k)) == 0)
		    continue;
#if 0
		if ((SB_AP_BITMAP (s)[i]->b_data[j] & (1 << k)) == 0  || /* k-th block is free */
		    block < SB_BUFFER_WITH_SB (s)->b_blocknr) /* is in skipped for drive manager area */
		    continue;
#endif
		
		print_how_far (&done, total_block_number);
		
		data = bh->b_data + k * s->s_blocksize;

		if (not_formatted_node (data, s->s_blocksize)) {
		    /* ok, could not find formatted node here. But
                       this can be commit block, or bitmap which has
                       to be transferred */
		    if (!not_data_block (s, block)) {
			/* this is usual unformatted node. Transfer
                           its number only to erase previously existed
                           formatted nodes on the partition we will
                           apply transferred metadata to */
	    
			/* size of following record in network byte order */
			reclen16 = htons (2);

			/* the record record */
			data16 = htons (MAX_HEIGHT + 1);/*?*/
			data = (char *)&data16;
		    } else {
			/* write super block and bitmap block must be transferred as are */
			/* size of record  */
			reclen16 = htons (s->s_blocksize);
	    
			/* the record itself */
			data = data;
		    }
		} else {
		    /* any kind of formatted nodes gets here (super
                       block, desc block of journal): FIXME: it would
                       be useful to be able to find commit blocks */
		    zero_direct_items (data);
		    /* FIXME: do other packing */
		    /* write size of following record */
		    reclen16 = htons (s->s_blocksize);
		    
		    /* the record itself */
		    data = data;

#if 0
		    if (blkh->blk_level > DISK_LEAF_NODE_LEVEL) {
			/* block must look like internal node on the target
			   partition. But (currently) fsck do not consider internal
			   nodes, therefore we do not have to transfer contents of
			   internal nodes */
	    
			/* size of following record in network byte order */
			reclen16 = htons (2);
	    
			/* the record itself */
			data16 = htons (DISK_LEAF_NODE_LEVEL + 1);
			data = (char *)&data16;	  
		    } else {
	    
			/* leaf node found */
			ih = (struct item_head *)(blkh + 1);
	    
			/* fill direct items with 0s */
			for (l = 0; l < blkh->blk_nr_item; l ++, ih ++)
			    if (I_IS_DIRECT_ITEM (ih)) {
				direct_items ++;
				direct_item_total_length += ih->ih_item_len;
				memset ((char *)blkh + ih->ih_item_location, 0, ih->ih_item_len);
			    }
	    
			/* write size of following record */
			reclen16 = htons (s->s_blocksize);
	    
			/* the record itself */
			data = (char *)blkh;
		    }
#endif
		}
	  
		/*fprintf (stderr, "block %d, reclen %d\n", block, ntohs (reclen16));*/
	
		/* write block number */
		blocknumber32 = htonl (block);
		bytes_to_transfer += sizeof (uint32_t) + sizeof (uint16_t) + ntohs (reclen16);
		if (opt_pack == 'p' || opt_pack_all == 'p') {
		    write (1, &blocknumber32, sizeof (uint32_t));
		    /* write record len */
		    write (1, &reclen16, sizeof (uint16_t));
		    /* write the record */
		    write (1, data, ntohs (reclen16));
		}
	    }
      
	    bforget (bh);
	}
    }
    
 out_of_bitmap:
    fprintf (stderr, "done\n");
    if (opt_pack == 'c' || opt_pack_all == 'c')
	fprintf (stderr, "Bytes to transfer %Ld, sequential 0s %d in %d sequeneces (%items (%d unreacable))\n",
		 bytes_to_transfer, direct_item_total_length, direct_items, items, unreachable_items);
    else
	fprintf (stderr, "Bytes dumped %Ld, sequential 0s %d in %d sequeneces\n",
		 bytes_to_transfer, direct_item_total_length, direct_items);
    
    
}




/* print all valid transactions and found dec blocks */
static void print_journal (struct super_block * s)
{
    struct buffer_head * d_bh, * c_bh;
    struct reiserfs_journal_desc * desc ;
    struct reiserfs_journal_commit *commit ;
    int end_journal;
    int start_journal;
    int i, j;
    int first_desc_block = 0;
    int wrapped = 0;
    int valid_transactions = 0;

    start_journal = SB_JOURNAL_BLOCK (s);
    end_journal = start_journal + JOURNAL_BLOCK_COUNT;
    printf ("Start scanning from %d\n", start_journal);

    for (i = start_journal; i < end_journal; i ++) {
	d_bh = bread (s->s_dev, i, s->s_blocksize);
	if (is_desc_block (d_bh)) {
	    int commit_block;

	    if (first_desc_block == 0)
		/* store where first desc block found */
		first_desc_block = i;

	    print_block (d_bh); /* reiserfs_journal_desc structure will be printed */
	    desc = bh_desc (d_bh);

	    commit_block = d_bh->b_blocknr + desc->j_len + 1;
	    if (commit_block >= end_journal) {
		printf ("-- wrapped?");
		wrapped = 1;
		break;
	    }

	    c_bh = bread (s->s_dev, commit_block, s->s_blocksize);
	    commit = bh_commit (c_bh);
	    if (does_desc_match_commit (desc, commit)) {
		printf ("commit block %d (trans_id %ld, j_len %ld) does not match\n", commit_block,
			commit->j_trans_id, commit->j_len);
		brelse (c_bh) ;
		brelse (d_bh);
		continue;
	    }

	    valid_transactions ++;
	    printf ("(commit block %d) - logged blocks (", commit_block);
	    for (j = 0; j < desc->j_len; j ++) {
		if (j < JOURNAL_TRANS_HALF) {
		    printf (" %ld", desc->j_realblock[j]);
		} else {
		    printf (" %ld", commit->j_realblock[i - JOURNAL_TRANS_HALF]);
		}
	    }
	    printf ("\n");
	    i += desc->j_len + 1;
	    brelse (c_bh);
	}
	brelse (d_bh);
    }
    
    if (wrapped) {
	c_bh = bread (s->s_dev, first_desc_block - 1, s->s_blocksize);
	commit = bh_commit (c_bh);
	if (does_desc_match_commit (desc, commit)) {
	    printf ("No! commit block %d (trans_id %ld, j_len %ld) does not match\n", first_desc_block - 1,
		    commit->j_trans_id, commit->j_len);
	} else {
	    printf ("Yes! (commit block %d) - logged blocks (\n", first_desc_block - 1);
	    for (j = 0; j < desc->j_len; j ++) {
		if (j < JOURNAL_TRANS_HALF) {
		    printf (" %ld", desc->j_realblock[j]);
		} else {
		    printf (" %ld", commit->j_realblock[i - JOURNAL_TRANS_HALF]);
		}
	    }
	    printf ("\n");
	}
	brelse (c_bh) ;
	brelse (d_bh);
    }

    printf ("%d valid transactions found\n", valid_transactions);

    {
	struct buffer_head * bh;
	struct reiserfs_journal_header * j_head;

	bh = bread (s->s_dev, s->u.reiserfs_sb.s_rs->s_journal_block + s->u.reiserfs_sb.s_rs->s_orig_journal_size,
		    s->s_blocksize);
	j_head = (struct reiserfs_journal_header *)(bh->b_data);

	printf ("#######################\nJournal header:\n"
		"j_last_flush_trans_id %ld\n"
		"j_first_unflushed_offset %ld\n"
		"j_mount_id %ld\n", j_head->j_last_flush_trans_id, j_head->j_first_unflushed_offset,
		j_head->j_mount_id);
	brelse (bh);
    }
}

int main (int argc, char * argv[])
{
    char * file_name;
    int dev, i;

#if 1
    if (1) {
	/* ???? */
	schedule ();
	iput (0);
    }
#endif

    fprintf (stderr, "\n<-----------dumpreiserfs, version 0.99, 2000----------->\n"); 
    file_name = parse_options (argc, argv);


    dev = open (file_name, O_RDONLY);
    if (dev == -1)
	die ("dumpreiserfs: Can not open device %s: %s\n", file_name, strerror (errno));
    g_sb.s_dev = dev;

    if (uread_super_block (&g_sb))
	die ("dumpreiserfs: no reiserfs found on %s", file_name);
    if (uread_bitmaps (&g_sb))
	die ("dumpreiserfs: read_bitmap failed");

    if (opt_pack || opt_pack_all) {
	pack_partition (&g_sb);
    } else {
	/* dump file system to stdout */
	if (opt_block_to_print != -1) {
	    print_one_block (opt_block_to_print);
	    goto end;
	}

	print_block (SB_BUFFER_WITH_SB (&g_sb));

	if (opt_print_journal)
	    print_journal (&g_sb);
    
	if (opt_print_objectid_map == 1)
	    print_objectid_map (&g_sb);
    
	if (opt_print_block_map) {
	    print_bmap (&g_sb, opt_print_block_map == 1 ? 1 : 0);
	}

	if (opt_print_regular_file_content || opt_print_directory_contents ||
	    opt_print_leaf_items) {
	    print_disk_tree (SB_ROOT_BLOCK (&g_sb));

	    /* print the statistic */
	    printf ("File system uses %d internal + %d leaves + %d unformatted nodes = %d blocks\n", 
		    g_stat_info.nr_internals, g_stat_info.nr_leaves, g_stat_info.nr_unformatted, 
		    g_stat_info.nr_internals + g_stat_info.nr_leaves + g_stat_info.nr_unformatted);
	}
    }


 end:
    /* brelse bitmaps */
    if (SB_AP_BITMAP (&g_sb)) {
	for (i = 0; i < SB_BMAP_NR (&g_sb); i ++) {
	    brelse (SB_AP_BITMAP (&g_sb)[i]);
	}
	freemem (SB_AP_BITMAP (&g_sb));
    }

    /* brelse buffer containing super block */
    brelse (SB_BUFFER_WITH_SB (&g_sb));

    check_and_free_buffer_mem ();

    return 0;
}
/* end of main */



