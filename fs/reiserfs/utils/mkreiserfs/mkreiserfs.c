/*
 * Copyright 1996, 1997, 1998, 1999 Hans Reiser
 */

/* mkreiserfs is very simple. It supports only 4 and 8K blocks. It skip
   first REISERFS_DISK_OFFSET_IN_BYTES of device, and then writes the super
   block, the needed amount of bitmap blocks (this amount is calculated
   based on file system size), and root block. Bitmap policy is
   primitive: it assumes, that device does not have unreadable blocks,
   and it occupies first blocks for super, bitmap and root blocks.
   bitmap blocks are interleaved across the disk, mainly to make
   resizing faster. */

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

#define print_usage_and_exit() die ("Usage: %s [ -f ] device [block-count]\n\n", argv[0])

#define DEFAULT_BLOCKSIZE 4096
#define MIN_BLOCK_AMOUNT (100+JOURNAL_BLOCK_COUNT+RESERVED_FOR_PRESERVE_LIST)


struct buffer_head * g_sb_bh;
struct buffer_head * g_bitmap_bh;
struct buffer_head * g_rb_bh;
struct buffer_head * g_journal_bh ;


int g_block_size = DEFAULT_BLOCKSIZE;
unsigned long int g_block_number;

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



/* calculates number of blocks on device 
 */
unsigned long count_blocks (char * filename, int blocksize)
{
  loff_t high, low;
  int fd;

  fd = open (filename, O_RDONLY);
  if (fd < 0)
    die ("count_blocks: open failed (%s)", strerror (errno));

#ifdef BLKGETSIZE
  {
    long size;

    if (ioctl (fd, BLKGETSIZE, &size) >= 0) {
      close (fd);
      return  size / (blocksize / 512);
    }
  }
#endif

  low = 0;
  for( high = 1; valid_offset (fd, high); high *= 2 )
    low = high;
  while (low < high - 1) {
    const loff_t mid = ( low + high ) / 2;
      
    if (valid_offset (fd, mid))
      low = mid;
    else
      high = mid;
  }
  valid_offset (fd, 0);
  close (fd);
  
  return (low + 1) / (blocksize);
}



/* form super block */
void make_super_block (int dev)
{
  struct reiserfs_super_block * sb;
  unsigned long * oids;


  if (SB_SIZE > g_block_size)
    die ("mkreiserfs: blocksize (%d) too small", g_block_size);
    

  /* get buffer for super block */
  g_sb_bh = getblk (dev, REISERFS_DISK_OFFSET_IN_BYTES / g_block_size, g_block_size);

/*  sb = (struct reiserfs_super_block *)g_cp_super_block;*/
  sb = (struct reiserfs_super_block *)g_sb_bh->b_data;
  sb->s_blocksize = g_block_size;   		/* block size in bytes */
  sb->s_block_count = g_block_number;	/* how many block reiserfs must occupy */
  sb->s_state = REISERFS_VALID_FS;
  sb->s_tree_height = 2;
  sb->s_journal_dev = 0 ;
  sb->s_orig_journal_size = JOURNAL_BLOCK_COUNT ;
  sb->s_journal_trans_max = 0 ;
  sb->s_journal_block_count = 0 ;
  sb->s_journal_max_batch = 0 ;
  sb->s_journal_max_commit_age = 0 ;
  sb->s_journal_max_trans_age = 0 ;

  sb->s_bmap_nr = g_block_number / (g_block_size * 8) + ((g_block_number % (g_block_size * 8)) ? 1 : 0);
  memcpy (sb->s_magic, REISERFS_SUPER_MAGIC_STRING, sizeof (REISERFS_SUPER_MAGIC_STRING));
  

  /* initialize object map */
  oids = (unsigned long *)(sb + 1);
  oids[0] = 1;
  oids[1] = REISERFS_ROOT_OBJECTID + 1;	/* objectids > REISERFS_ROOT_OBJECTID are free */
  sb->s_oid_cursize = 2;

  /* max size must be even */
  sb->s_oid_maxsize = (g_block_size - SB_SIZE) / sizeof(unsigned long) / 2 * 2;


  mark_buffer_dirty (g_sb_bh, 0);
  mark_buffer_uptodate (g_sb_bh, 0);
  return;

}


void zero_journal_blocks(int dev, int start, int len) {
  int i ;
  struct buffer_head *bh ;
  int done = 0;

  printf ("Initializing journal - "); fflush (stdout);

  for (i = 0 ; i < len ; i++) {
    print_how_far (&done, len);
    bh = getblk (dev, start + i, g_block_size) ;
    memset(bh->b_data, 0, g_block_size) ;
    mark_buffer_dirty(bh,0) ;
    mark_buffer_uptodate(bh,0) ;
    bwrite (bh);
    brelse(bh) ;
  }
  printf ("\n"); fflush (stdout);
}


/* this only sets few first bits in bitmap block. Fills not initialized
   fields of super block (root block and bitmap block numbers)
   */
void make_bitmap ()
{
  struct reiserfs_super_block * sb = (struct reiserfs_super_block *)g_sb_bh->b_data;
  int i, j;
  
  /* get buffer for bitmap block */
  g_bitmap_bh = getblk (g_sb_bh->b_dev, g_sb_bh->b_blocknr + 1, g_sb_bh->b_size);
  
  /* mark, that first 8K of device is busy */
  for (i = 0; i < REISERFS_DISK_OFFSET_IN_BYTES / g_block_size; i ++)
    set_bit (i, g_bitmap_bh->b_data);

  /* mark that super block is busy */
  set_bit (i++, g_bitmap_bh->b_data);

  /* mark first bitmap block as busy */
  set_bit (i ++, g_bitmap_bh->b_data);
  
  /* sb->s_journal_block = g_block_number - JOURNAL_BLOCK_COUNT ; */ /* journal goes at end of disk */
  sb->s_journal_block = i;

  /* mark journal blocks as busy  BUG! we need to check to make sure journal will fit in the first bitmap block */
  for (j = 0 ; j < (JOURNAL_BLOCK_COUNT + 1); j++) /* the descriptor block goes after the journal */
    set_bit (i ++, g_bitmap_bh->b_data);

  /* and tree root is busy */
  set_bit (i, g_bitmap_bh->b_data);
  sb->s_root_block = i;
  sb->s_free_blocks = sb->s_block_count - i - 1 ;

  /* count bitmap blocks not resides in first s_blocksize blocks */
  sb->s_free_blocks -= sb->s_bmap_nr - 1;

  mark_buffer_dirty (g_bitmap_bh, 0);
  mark_buffer_uptodate (g_bitmap_bh, 0);

  mark_buffer_dirty (g_sb_bh, 0);
  return;
}


/* form the root block of the tree (the block head, the item head, the
   root directory) */
void make_root_block ()
{
  struct reiserfs_super_block * sb = (struct reiserfs_super_block *)g_sb_bh->b_data;
  char * rb;
  struct block_head * blkh;
  struct item_head * ih;

  struct stat_data * sd;
  struct reiserfs_de_head * deh;
  struct key maxkey = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};

  /* get memory for root block */
/*  g_cp_root_block = getmem (g_block_size);*/
  /* no more cautious bitmap, kill the *2 */
  /* g_rb_bh = getblk (g_sb_bh->b_dev, g_sb_bh->b_blocknr + sb->s_bmap_nr * 2 + 1, g_sb_bh->b_size); */
  g_rb_bh = getblk (g_sb_bh->b_dev, sb->s_root_block, sb->s_blocksize);
  rb = g_rb_bh->b_data;

  /* block head */
  blkh = (struct block_head *)rb;
  blkh->blk_level = DISK_LEAF_NODE_LEVEL;
  blkh->blk_nr_item = 0;
  blkh->blk_free_space = sb->s_blocksize - BLKH_SIZE;
  memcpy (&blkh->blk_right_delim_key, &maxkey, KEY_SIZE);

  /* first item is stat data item of root directory */
  ih = (struct item_head *)(blkh + 1);
  ih->ih_key.k_dir_id = REISERFS_ROOT_PARENT_OBJECTID;
  ih->ih_key.k_objectid = REISERFS_ROOT_OBJECTID;
  ih->ih_key.k_offset = SD_OFFSET;
  ih->ih_key.k_uniqueness = TYPE_STAT_DATA;
  ih->ih_item_len = SD_SIZE;
  ih->ih_item_location = sb->s_blocksize - ih->ih_item_len;
  ih->u.ih_free_space = MAX_US_INT;
  ih->ih_reserved = 0;

  /* fill stat data */
  sd = (struct stat_data *)(rb + ih->ih_item_location);
  sd->sd_mode = S_IFDIR + 0755;
  sd->sd_nlink = 3;
  sd->sd_uid = 0;	/*??*/
  sd->sd_gid = 0;	/*??*/
  sd->sd_size = EMPTY_DIR_SIZE;
  sd->sd_atime = sd->sd_ctime = sd->sd_mtime = time (NULL);
  sd->u.sd_blocks = 0;	/*??*/
  sd->sd_first_direct_byte = MAX_UL_INT;	/*??*/


  blkh->blk_nr_item ++;
  blkh->blk_free_space -= (IH_SIZE + ih->ih_item_len);

  
  /* second item is root directory item, containing "." and ".." */
  ih ++;
  ih->ih_key.k_dir_id = REISERFS_ROOT_PARENT_OBJECTID;
  ih->ih_key.k_objectid = REISERFS_ROOT_OBJECTID;
  ih->ih_key.k_offset = DOT_OFFSET;
  ih->ih_key.k_uniqueness = DIRENTRY_UNIQUENESS/*DOT_UNIQUENESS*/;
#ifdef REISERFS_ALIGNED
  ih->ih_item_len = DEH_SIZE * 2 + 4 + 4;
#else
  ih->ih_item_len = DEH_SIZE * 2 + strlen (".") + strlen ("..")/* + sizeof (unsigned long)*/;
#endif
  ih->ih_item_location = (ih-1)->ih_item_location - ih->ih_item_len;
  ih->u.ih_entry_count = 2;
  ih->ih_reserved = 0;
  

  deh = (struct reiserfs_de_head *)(rb + ih->ih_item_location);

  /* "." */
  deh[0].deh_offset = DOT_OFFSET;
  /*  deh[0].deh_uniqueness = DOT_UNIQUENESS;*/
  deh[0].deh_dir_id = ih->ih_key.k_dir_id;
  deh[0].deh_objectid = ih->ih_key.k_objectid;
#ifdef REISERFS_ALIGNED
  deh[0].deh_location = ih->ih_item_len - 4;
#else
  deh[0].deh_location = ih->ih_item_len - strlen (".");
#endif
  /*mark_de_without_sd (&(deh[0]));*/
  clear_bit (DEH_Statdata, &(deh[0].deh_state));

  /*mark_de_with_directory_id (&(deh[0]));*/
/*  clear_bit (DEH_AdditionalKeyComponent, &(deh[0].deh_state));*/

  /*mark_de_visible (&(deh[0]));*/
  set_bit (DEH_Visible, &(deh[0].deh_state));

  /* ".." */
  deh[1].deh_offset = DOT_DOT_OFFSET;
  /*  deh[1].deh_uniqueness = DOT_DOT_UNIQUENESS;*/
  deh[1].deh_dir_id = 0;
  deh[1].deh_objectid = REISERFS_ROOT_PARENT_OBJECTID;	/* as key of root directory is [REISERFS_ROOT_PARENT_OBJECTID, 
							                                REISERFS_ROOT_OBJECTID],
							   so objectid of root directory
							   parent direcotry is REISERFS_ROOT_PARENT_OBJECTID */
#ifdef REISERFS_ALIGNED
  deh[1].deh_location = deh[0].deh_location - 4;
#else
  deh[1].deh_location = deh[0].deh_location - strlen ("..");
#endif

  /*mark_de_without_sd (&(deh[1]));*/
  clear_bit (DEH_Statdata, &(deh[1].deh_state));
  
  /*mark_de_with_directory_id (&(deh[1]));*/
  /*set_bit (DEH_AdditionalKeyComponent, &(deh[1].deh_state));*/

  /*mark_de_visible (&(deh[1]));*/
  set_bit (DEH_Visible, &(deh[1].deh_state));

#ifdef REISERFS_ALIGNED
  strncpy(rb + ih->ih_item_location + deh[0].deh_location, ".", 4);
  strncpy(rb + ih->ih_item_location + deh[1].deh_location, "..", 4);
#else
  memcpy (rb + ih->ih_item_location + deh[0].deh_location, ".", strlen ("."));
  memcpy (rb + ih->ih_item_location + deh[1].deh_location, "..", strlen (".."));
#endif
  /* objectid of parent directory of object pointed by ".." */
  /**(unsigned long *)(rb + ih->ih_item_location + deh[1].deh_location + strlen ("..")) = 0;*/
  

  blkh->blk_nr_item ++;
  blkh->blk_free_space -= (IH_SIZE + ih->ih_item_len);
  
  mark_buffer_dirty (g_rb_bh, 0);
  mark_buffer_uptodate (g_rb_bh, 0);
  return;
}


/*
 *  write the super block, the bitmap blocks and the root of the tree
 */
void write_super_and_root_blocks ()
{
  struct reiserfs_super_block * sb = (struct reiserfs_super_block *)g_sb_bh->b_data;
  int i;

  zero_journal_blocks(g_sb_bh->b_dev, sb->s_journal_block, JOURNAL_BLOCK_COUNT + 1) ;

  /* super block */
  bwrite (g_sb_bh);

  /* bitmap blocks */
  for (i = 0; i < sb->s_bmap_nr; i ++) {
    if (i != 0) {
      g_bitmap_bh->b_blocknr = i * sb->s_blocksize * 8;
      memset (g_bitmap_bh->b_data, 0, g_bitmap_bh->b_size);
      set_bit (0,g_bitmap_bh->b_data);
    }
    if (i == sb->s_bmap_nr - 1) {
      int j;

      /* fill unused part of last bitmap block with 1s */
      if (sb->s_block_count % (sb->s_blocksize * 8))
	for (j = sb->s_block_count % (sb->s_blocksize * 8); j < sb->s_blocksize * 8; j ++) {
	  set_bit (j, g_bitmap_bh->b_data);
      }
    }
    /* write true bitmap */
    mark_buffer_dirty (g_bitmap_bh, 0);
    bwrite (g_bitmap_bh);

#if 0
    /* write cautious bitmap */
    g_bitmap_bh->b_blocknr += sb->s_bmap_nr;
    mark_buffer_dirty (g_bitmap_bh, 0);
    bwrite (g_bitmap_bh);
    g_bitmap_bh->b_blocknr -= sb->s_bmap_nr;    
#endif
  }

  /* root block */
  bwrite (g_rb_bh);
  brelse (g_rb_bh);
  brelse (g_bitmap_bh);
  brelse (g_sb_bh);
}


char buf[20];

#include <linux/kdev_t.h>

char * devname (int dev)
{
  struct stat st;

  if (fstat (dev, &st) != 0)
    die ("stat failed");
  sprintf (buf, "0x%x:0x%x", MAJOR((int)st.st_rdev), MINOR((int)st.st_rdev));
  return buf;
}


void report (void)
{
    struct reiserfs_super_block * sb = (struct reiserfs_super_block *)g_sb_bh->b_data;
    unsigned int i;

    printf ("Block size %d bytes\n", sb->s_blocksize);
    printf ("Block count %d\n", sb->s_block_count);
    printf ("First %ld blocks skipped\n", g_sb_bh->b_blocknr);
    printf ("Super block is in %ld\n", g_sb_bh->b_blocknr);
    printf ("Bitmap blocks are : \n\t%ld", g_bitmap_bh->b_blocknr);
    for (i = 1; i < sb->s_bmap_nr; i ++) {
	printf (", %d", i * sb->s_blocksize * 8);
    }
    printf ("\nJournal size %d (blocks %d-%d of device %s)\n",
	    JOURNAL_BLOCK_COUNT, sb->s_journal_block, 
	    sb->s_journal_block + JOURNAL_BLOCK_COUNT, devname (g_sb_bh->b_dev));
    printf ("Root block %u\n", sb->s_root_block);
    printf ("Used %d blocks\n", sb->s_block_count - sb->s_free_blocks);
    fflush (stdout);
}


/* discard 1st 2k block partition. This should be enough to make
   mount not see ext2 (and others) on mkreiserfs'd partition;
   NOW it clear the first 2k block to avoid wrong vfat mounting 
   (it search its "super block" in 1st 512 bytes) 

   We also clear the original old journaled superblock (8k offset).
   
*/
void invalidate_other_formats (int dev)
{
  struct buffer_head * bh;

  bh = getblk (dev, 0, 2048);
  mark_buffer_uptodate (bh, 1);
  mark_buffer_dirty (bh, 1);
  bwrite (bh);
  brelse (bh);
  bh = getblk(dev, REISERFS_OLD_DISK_OFFSET_IN_BYTES  / 1024, 1024) ;
  if (!bh) {
    printf("Unable to get block to clear the old reiserfs superblock\n") ;
    return ;
  }
  mark_buffer_uptodate (bh, 1);
  mark_buffer_dirty (bh, 1);
  bwrite (bh);
  brelse (bh);
}


int main (int argc, char **argv)
{
  char *tmp;
  int dev;
  int force = 0;
  struct stat statbuf;
  char * device_name;
  char c;

  printf ("\n\n<-----------MKREISERFS, 1999----------->\n%s\n", 
           reiserfs_get_version_string());
  

#if 1
  if (0) {
    /* ???? */
    getblk (0,0,0);
    iput (0);
  }
#endif

  if (argc < 2)
    print_usage_and_exit ();

/*  init_buffer_mem ();*/

  while ( ( c = getopt( argc, argv, "f" ) ) != EOF )
    switch( c )
      {
      case 'f' :                 /* force if file is not a block device */
	force = 1;
	break;
#if 0 /* -b is not supported with the journal code */
      case 'b' :                  /* -k n - where n is 1,2 or 4 */
	g_block_size = (int) strtol (optarg, &tmp, 0);
	if ( *tmp || ( g_block_size != 1 && g_block_size != 2 && g_block_size != 4 ))
	  die ("mkreiserfs: bad block size : %s\n", optarg);
	g_block_size *= 1024;
	break;
#endif /* -b */
      default :
	print_usage_and_exit ();
      }
  device_name = argv [optind];
  

  /* get block number for file system */
  if (optind == argc - 2) {
    g_block_number = strtol (argv[optind + 1], &tmp, 0);
    if (*tmp == 0) {    /* The string is integer */
      if (g_block_number > count_blocks (device_name, g_block_size))
	die ("mkreiserfs: specified block number (%d) is too high", g_block_number);
/*      else if (g_block_number < MIN_BLOCK_AMOUNT)
        die ("mkreiserfs: specified block number (%d) is too low", g_block_number); 
*/
    } else {
	    die ("mkreiserfs: bad block count : %s\n", argv[optind + 1]);
	}	
  } else 
    if (optind == argc - 1) {
      /* number of blocks is not specified */
      g_block_number = count_blocks (device_name, g_block_size);
      tmp = "";
    } else
      print_usage_and_exit ();


  g_block_number = g_block_number / 8 * 8;

/*  if (*tmp || g_block_number % 8 || (g_block_number == 0))
    / * block amount specified is not a valid integer * /
    die ("mkreiserfs: bad block count : %s\n", argv[optind + 1]);
*/	
  if (g_block_number < MIN_BLOCK_AMOUNT)
	die ("mkreiserfs: block number %d (truncated to n*8) is too low", 
		g_block_number);

  if (is_mounted (device_name))
    die ("mkreiserfs: '%s' contains a mounted file system\n", device_name);

  /* open_device will die if it could not open device */
  dev = open (device_name, O_RDWR);
  if (dev == -1)
    die ("mkreiserfs: can not open '%s': %s", device_name, strerror (errno));
  
  if (fstat (dev, &statbuf) < 0)
    die ("mkreiserfs: unable to stat %s", device_name);
  
  if (!S_ISBLK (statbuf.st_mode) && ( force == 1 ))
    die ("mkreiserfs: '%s (%o)' is not a block device", device_name, statbuf.st_mode);
  else        /* Ignore any 'full' fixed disk devices */
    if ( statbuf.st_rdev == 0x0300 || statbuf.st_rdev == 0x0340 
	 || statbuf.st_rdev == 0x0400 || statbuf.st_rdev == 0x0410
	 || statbuf.st_rdev == 0x0420 || statbuf.st_rdev == 0x0430
	 || statbuf.st_rdev == 0x0d00 || statbuf.st_rdev == 0x0d40 )
      /* ???? */
      die ("mkreiserfs: will not try to make filesystem on '%s'", device_name);

  
  /* these fill buffers (super block, first bitmap, root block) with
     reiserfs structures */
  make_super_block (dev);
  make_bitmap ();
  make_root_block ();
  
  report ();

  printf ("ATTENTION: ALL DATA WILL BE LOST ON '%s'! (y/n)", device_name);
  c = getchar ();
  if (c != 'y' && c != 'Y')
    die ("mkreiserfs: Disk was not formatted");

  invalidate_other_formats (dev);
  write_super_and_root_blocks ();

  check_and_free_mem ();

  printf ("Syncing.."); fflush (stdout);

  close(dev) ;
  sync ();
 
  printf ("\n\nReiserFS core development sponsored by SuSE Labs (suse.com)\n\nJournaling sponsored by MP3.com.\n\nItem handlers sponsored by Ecila.com\n\nTo learn about the programmers and ReiserFS, please go to\nhttp://www.devlinux.com/namesys\n\nHave fun.\n\n"); fflush (stdout);
  return 0;

}
