/*
 * Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */
#ifdef __KERNEL__

#include <linux/module.h>
#include <linux/sched.h>
#include <asm/uaccess.h>
#include <linux/reiserfs_fs.h>
#include <linux/locks.h>
#include <linux/init.h>

#else

#include "nokernel.h"
#include <stdlib.h> // for simple_strtoul

#endif

#define REISERFS_OLD_BLOCKSIZE 4096
#define REISERFS_SUPER_MAGIC_STRING_OFFSET_NJ 20

#if 0 /* journal victim */
void mark_suspected_recipients_dirty (struct reiserfs_transaction_handle *th, kdev_t dev)
{
  struct buffer_head * bh;
  int nlist;
  int dirtied = 0;
  struct super_block *s = reiserfs_get_super(dev) ;

repeat:
  for(nlist = 0; nlist < NR_LIST; nlist++) {
    bh = lru_list[nlist];
    if(!bh) continue;
    
    do {
      if (bh->b_dev == dev && test_bit (12, &(bh->b_state))) {
	if (!buffer_dirty (bh)) {
	  journal_mark_dirty(th, s, bh) ;
	  printk ("sr found: block %ld, state %lo\n", bh->b_blocknr, bh->b_state);
	  dirtied ++;
	  goto repeat;
	}
      }
      bh = bh->b_next_free;
    } while (bh != lru_list[nlist]);
  }
  if (dirtied)
    printk ("mark_suspected_recipients_dirty: found %d suspected recipients, dirtied\n", dirtied);
}
#endif /* journal victim */

/* like fs.h:/mark_buffer_dirty but refile_buffer */
inline void reiserfs_mark_buffer_dirty (struct buffer_head * bh, int flag)
{
  if (!test_and_set_bit(BH_Dirty, &bh->b_state))
    set_writetime(bh, flag);
}


/* like fs.h:/mark_buffer_clean but refile_buffer */
inline void reiserfs_mark_buffer_clean (struct buffer_head * bh)
{
  test_and_clear_bit(BH_Dirty, &bh->b_state);
}


void reiserfs_write_super (struct super_block * s)
{

  int dirty = 0 ;
  if (!(s->s_flags & MS_RDONLY)) {
#if 0 /* journal victim */
    rs = SB_DISK_SUPER_BLOCK (s);
    /*
     * if reiserfs was mounted with read-write permissions make file
     * system state not valid so that if we crash without doing a
     * clean umount we know that we must run file system
     * checker. umount will mark it valid if it does a clean umount
     */
    if (le16_to_cpu (rs->s_state) == REISERFS_VALID_FS) {
      rs->s_state = cpu_to_le16 (REISERFS_ERROR_FS);
      /* mark_buffer_dirty (SB_BUFFER_WITH_SB (s), 1); */
      journal_begin(&th, s, 1) ;
      journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB (s)) ;
      journal_end(&th, s, 1) ;
    }
#endif
    dirty = flush_old_commits(s, 1) ;
  }
  s->s_dirt = dirty;
}


void reiserfs_put_super (struct super_block * s)
{
  int i;
  kdev_t dev = s->s_dev;
  struct reiserfs_transaction_handle th ;

  journal_begin(&th, s, 10) ;
  if (s->u.reiserfs_sb.lock_preserve)
    reiserfs_panic (s, "vs-2000: reiserfs_put_super: lock_preserve == %d", s->u.reiserfs_sb.lock_preserve);

  /* change file system state to current state if it was mounted with read-write permissions */
  if (!(s->s_flags & MS_RDONLY)) {
    SB_REISERFS_STATE (s) = le16_to_cpu (s->u.reiserfs_sb.s_mount_state);
    /* mark_buffer_dirty (SB_BUFFER_WITH_SB (s), 1); */
    journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB (s));
  }

#if 0
  if (maybe_free_preserve_list (s) == 0) {
    reiserfs_warning ("vs-2003: reiserfs_put_super: there are %ld buffers to write\n",
		      s->u.reiserfs_sb.s_suspected_recipient_count);
#ifdef REISERFS_CHECK
    preserve_trace_print_srs (s);
#endif

    /* mark_suspected_recipients_dirty (&th, dev); journal victim */
    fsync_dev (dev);
    s->u.reiserfs_sb.s_suspected_recipient_count = 0;
#ifdef REISERFS_CHECK
    preserve_trace_reset_suspected_recipients (s);
#endif
    maybe_free_preserve_list (s);
  }
#endif

  
#if 0 /* journal victim */
  for (i = 0; i < SB_BMAP_NR (s); i ++) {
    /* update cautious bitmap */
    if (memcmp (SB_AP_BITMAP (s)[i]->b_data, SB_AP_CAUTIOUS_BITMAP (s)[i], SB_AP_BITMAP (s)[i]->b_size)) {
      memcpy (SB_AP_CAUTIOUS_BITMAP (s)[i]->b_data, SB_AP_BITMAP (s)[i]->b_data, SB_AP_BITMAP (s)[i]->b_size);
      mark_buffer_dirty (SB_AP_CAUTIOUS_BITMAP (s)[i], 1);
      ll_rw_block (WRITE, 1, &SB_AP_CAUTIOUS_BITMAP (s)[i]);
    }
  }
#endif /* journal victim */
  journal_release(&th, s) ;
  /* reiserfs_sync_all_buffers(s->s_dev, 1) ; journal does not need this any more */


  /* wait on write completion */
  for (i = 0; i < SB_BMAP_NR (s); i ++) {
    /* wait_on_buffer (SB_AP_CAUTIOUS_BITMAP (s)[i]); */
    /* brelse (SB_AP_CAUTIOUS_BITMAP (s)[i]); */
    brelse (SB_AP_BITMAP (s)[i]);
  }

  reiserfs_kfree (SB_AP_BITMAP (s), sizeof (struct buffer_head *) * SB_BMAP_NR (s), s);
  /* reiserfs_kfree (SB_AP_CAUTIOUS_BITMAP (s), sizeof (struct buffer_head *) * SB_BMAP_NR (s), s); */


  brelse (SB_BUFFER_WITH_SB (s));

  print_statistics (s);

  if (s->u.reiserfs_sb.s_kmallocs != 0) {
    reiserfs_warning ("vs-2004: reiserfs_put_super: aloocated memory left %d\n", s->u.reiserfs_sb.s_kmallocs);
  }

  s->s_dev = 0;

  fixup_reiserfs_buffers (dev);

  MOD_DEC_USE_COUNT;
  return;
}


/* super block operations are */
static struct super_operations reiserfs_sops = 
{
  reiserfs_read_inode,
  reiserfs_write_inode,
  NULL,				/* put_inode*/
  reiserfs_delete_inode,
  reiserfs_notify_change,
  reiserfs_put_super,
  reiserfs_write_super,
  reiserfs_statfs,
  reiserfs_remount,
  NULL, 				/* clear_inode */
  NULL				/* umount_begin */
};

/* this was (ext2)parse_options */
static int parse_options (char * options, unsigned long * mount_options, unsigned long * blocks)
{
    char * this_char;
    char * value;
  
    *blocks = 0;
    set_bit (GENERICREAD, mount_options);
    if (!options)
	/* use default configuration: complex read, create tails, preserve on */
	return 1;
    for (this_char = strtok (options, ","); this_char != NULL; this_char = strtok (NULL, ",")) {
	if ((value = strchr (this_char, '=')) != NULL)
	    *value++ = 0;
	if (!strcmp (this_char, "notail")) {
	    set_bit (NOTAIL, mount_options);
	} else if (!strcmp (this_char, "replayonly")) {
	    set_bit (REPLAYONLY, mount_options);
	} else if (!strcmp (this_char, "resize")) {
	    if (!value || !*value){
	  	printk("reiserfs: resize option requires a value\n");
	    }
	    *blocks = simple_strtoul (value, &value, 0);
	} else {
	    printk ("reiserfs: Unrecognized mount option %s\n", this_char);
	    return 0;
	}
    }
    return 1;
}



int reiserfs_is_super(struct super_block *s) {
   return (s->s_dev != 0 && s->s_op == &reiserfs_sops) ;
}
int reiserfs_remount (struct super_block * s, int * flags, char * data)
{
  struct reiserfs_super_block * rs;
  struct reiserfs_transaction_handle th ;
  unsigned long blocks;
  unsigned long mount_options;

  rs = SB_DISK_SUPER_BLOCK (s);

  if (!parse_options(data, &mount_options, &blocks))
  	return 0;

  if(blocks) 
  	reiserfs_resize(s, blocks);
	
  journal_begin(&th, s, 10) ;
  if ((unsigned long)(*flags & MS_RDONLY) == (s->s_flags & MS_RDONLY)) {
    /* there is nothing to do to remount read-only fs as read-only fs */
    journal_end(&th, s, 10) ;
    return 0;
  }
  if (*flags & MS_RDONLY) {
    /* try to remount file system with read-only permissions */
    if (le16_to_cpu (rs->s_state) == REISERFS_VALID_FS ||
	s->u.reiserfs_sb.s_mount_state != REISERFS_VALID_FS) {
      journal_end(&th, s, 10) ;
      return 0;
    }
    /* Mounting a rw partition read-only. */
    rs->s_state = cpu_to_le16 (s->u.reiserfs_sb.s_mount_state);
    /* mark_buffer_dirty (SB_BUFFER_WITH_SB (s), 1); journal victim */
    journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB (s));
    s->s_dirt = 0;
  } else {
    /* Mount a partition which is read-only, read-write */
    s->u.reiserfs_sb.s_mount_state = le16_to_cpu (rs->s_state);
    s->s_flags &= ~MS_RDONLY;
    rs->s_state = cpu_to_le16 (REISERFS_ERROR_FS);
    /* mark_buffer_dirty (SB_BUFFER_WITH_SB (s), 1); */
    journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB (s));
    s->s_dirt = 0;
    s->u.reiserfs_sb.s_mount_state = REISERFS_VALID_FS ;
    if (test_bit(NOTAIL, &mount_options)) {
      set_bit(NOTAIL, &(s->u.reiserfs_sb.s_mount_opt)) ;
    }
    
    /* check state, which file system had when remounting read-write */
#if 0 /* journal victim */    
    if (s->u.reiserfs_sb.s_mount_state != REISERFS_VALID_FS)
      printk ("REISERFS: remounting unchecked fs, "
	      "running reiserfsck is recommended\n");
#endif
  }
  /* this will force a full flush of all journal lists */
  SB_JOURNAL(s)->j_must_wait = 1 ;
  journal_end(&th, s, 10) ;
  return 0;
}


struct key root_key = {REISERFS_ROOT_PARENT_OBJECTID, REISERFS_ROOT_OBJECTID, 0, 0};

static int read_bitmaps (struct super_block * s)
{
  int i, repeat, bmp, dl ;
  struct reiserfs_super_block * rs = SB_DISK_SUPER_BLOCK(s);

  repeat = 0 ;
  /* read true bitmap block */
  SB_AP_BITMAP (s) = reiserfs_kmalloc (sizeof (struct buffer_head *) * le16_to_cpu (rs->s_bmap_nr), GFP_KERNEL, s);
  if (SB_AP_BITMAP (s) == 0)
    return 1;

  memset (SB_AP_BITMAP (s), 0, sizeof (struct buffer_head *) * le16_to_cpu (rs->s_bmap_nr));

  /* read bitmap blocks */
				/* reiserfs leaves the first 64k unused
                                   so that any partition labeling scheme
                                   currently used will have enough
                                   space. Then we need one block for the
                                   super.  -Hans */
  bmp = (REISERFS_DISK_OFFSET_IN_BYTES / s->s_blocksize) + 1;	/* first of bitmap blocks */
  SB_AP_BITMAP (s)[0] = reiserfs_bread (s->s_dev, bmp, s->s_blocksize, &repeat);
  if(!SB_AP_BITMAP(s)[0])
	  return 1;
  for (i = 1, bmp = dl = rs->s_blocksize * 8; i < le16_to_cpu (rs->s_bmap_nr); i ++) {
    SB_AP_BITMAP (s)[i] = reiserfs_bread (s->s_dev, bmp, s->s_blocksize, &repeat);
    if (!SB_AP_BITMAP (s)[i])
      return 1;
	bmp += dl;
  }

  return 0;
}

static int read_old_bitmaps (struct super_block * s)
{
  int i, repeat ;
  struct reiserfs_super_block * rs = SB_DISK_SUPER_BLOCK(s);
  int bmp1 = (REISERFS_OLD_DISK_OFFSET_IN_BYTES / s->s_blocksize) + 1;  /* first of bitmap blocks */

  repeat = 0 ;
  /* read true bitmap */
  SB_AP_BITMAP (s) = reiserfs_kmalloc (sizeof (struct buffer_head *) * le16_to_cpu (rs->s_bmap_nr), GFP_KERNEL, s);
  if (SB_AP_BITMAP (s) == 0)
    return 1;

  memset (SB_AP_BITMAP (s), 0, sizeof (struct buffer_head *) * le16_to_cpu (rs->s_bmap_nr));

  for (i = 0; i < le16_to_cpu (rs->s_bmap_nr); i ++) {
    SB_AP_BITMAP (s)[i] = reiserfs_bread (s->s_dev, bmp1 + i, s->s_blocksize, &repeat);
    if (!SB_AP_BITMAP (s)[i])
      return 1;
  }
	
  return 0;
}


void check_bitmap (struct super_block * s)
{
  int i = 0;
  int free = 0;
  char * buf;

  while (i < SB_BLOCK_COUNT (s)) {
    buf = SB_AP_BITMAP (s)[i / (s->s_blocksize * 8)]->b_data;
    if (!test_bit (i % (s->s_blocksize * 8), buf))
      free ++;
    i ++;
  }

  if (free != SB_FREE_BLOCKS (s))
    reiserfs_warning ("vs-4000: check_bitmap: %d free blocks, must be %d\n",
		      free, SB_FREE_BLOCKS (s));
}


/* support old disk layout */
static int read_old_super_block (struct super_block * s, int size)
{
  struct buffer_head * bh;
  struct reiserfs_super_block * rs;
  int repeat ;

  printk("reiserfs_read_super: try to find super block in old location\n");
  repeat = 0 ;
  /* there are only 4k-sized blocks in v3.5.10 */
  if (size != REISERFS_OLD_BLOCKSIZE)
	  set_blocksize(s->s_dev, REISERFS_OLD_BLOCKSIZE);
  bh = bread (s->s_dev, 
  			  REISERFS_OLD_DISK_OFFSET_IN_BYTES / REISERFS_OLD_BLOCKSIZE, 
 		      REISERFS_OLD_BLOCKSIZE);
  if (!bh) {
    printk("reiserfs_read_super: unable to read superblock on dev %s\n", kdevname(s->s_dev));
    return 1;
  }

  rs = (struct reiserfs_super_block *)bh->b_data;
  if (strncmp (rs->s_magic,  REISERFS_SUPER_MAGIC_STRING, strlen ( REISERFS_SUPER_MAGIC_STRING))) {
	  /* pre-journaling version check */
	  if(!strncmp((char*)rs + REISERFS_SUPER_MAGIC_STRING_OFFSET_NJ,
				  REISERFS_SUPER_MAGIC_STRING, strlen(REISERFS_SUPER_MAGIC_STRING))) {
		  printk("reiserfs_read_super: a pre-journaling reiserfs filesystem isn't suitable there.\n");
		  brelse(bh);
		  return 1;
	  }
	  
    brelse (bh);
    printk ("reiserfs_read_super: can't find a reiserfs filesystem on dev %s.\n", kdevname(s->s_dev));
    return 1;
  }

  if(REISERFS_OLD_BLOCKSIZE != le16_to_cpu (rs->s_blocksize)) {
  	printk("reiserfs_read_super: blocksize mismatch, super block corrupted\n");
	brelse(bh);
	return 1;
  }	

  s->s_blocksize = REISERFS_OLD_BLOCKSIZE;
  s->s_blocksize_bits = 0;
  while ((1 << s->s_blocksize_bits) != s->s_blocksize)
    s->s_blocksize_bits ++;

  SB_BUFFER_WITH_SB (s) = bh;
  SB_DISK_SUPER_BLOCK (s) = rs;
  s->s_op = &reiserfs_sops;
  return 0;
}


static int read_super_block (struct super_block * s, int size)
{
  struct buffer_head * bh;
  struct reiserfs_super_block * rs;
  int repeat ;

  repeat = 0 ;
  bh = bread (s->s_dev, (REISERFS_DISK_OFFSET_IN_BYTES / size), size);
  if (!bh) {
    printk("reiserfs_read_super: unable to read superblock on dev %s\n", kdevname(s->s_dev));
    return 1;
  }

  rs = (struct reiserfs_super_block *)bh->b_data;
  if (strncmp (rs->s_magic,  REISERFS_SUPER_MAGIC_STRING, strlen ( REISERFS_SUPER_MAGIC_STRING))) {
    brelse (bh);
    printk ("reiserfs_read_super: can't find a reiserfs filesystem on dev %s.\n", kdevname(s->s_dev));
    return 1;
  }

  s->s_blocksize = le16_to_cpu (rs->s_blocksize);
  s->s_blocksize_bits = 0;
  while ((1 << s->s_blocksize_bits) != s->s_blocksize)
    s->s_blocksize_bits ++;

  if (size != rs->s_blocksize) {
    brelse (bh);
    set_blocksize (s->s_dev, s->s_blocksize);
    bh = reiserfs_bread (s->s_dev,  (REISERFS_DISK_OFFSET_IN_BYTES / s->s_blocksize), s->s_blocksize, &repeat);
    if (!bh) {
      printk("reiserfs_read_super: unable to read superblock on dev %s\n", kdevname(s->s_dev));
      return 1;
    }

    rs = (struct reiserfs_super_block *)bh->b_data;
    if (strncmp (rs->s_magic,  REISERFS_SUPER_MAGIC_STRING, strlen ( REISERFS_SUPER_MAGIC_STRING)) ||
	le16_to_cpu (rs->s_blocksize) != s->s_blocksize) {
      brelse (bh);
      printk ("reiserfs_read_super: can't find a reiserfs filesystem on dev %s.\n", kdevname(s->s_dev));
      return 1;
    }
  }
  /* must check to be sure we haven't pulled an old format super out of the
  ** old format's log.  This is a kludge of a check, but it will work.  
  ** If block we've just read in is inside the journal for that
  ** super, it can't be valid.
  */
  if (bh->b_blocknr >= rs->s_journal_block && 
      bh->b_blocknr < (rs->s_journal_block + JOURNAL_BLOCK_COUNT)) {
      brelse(bh) ;
      printk("super-459: reiserfs_read_super: super found at block %lu is within its own log.  It must not be of this format type.\n", bh->b_blocknr) ;
      return 1 ;
  }

  SB_BUFFER_WITH_SB (s) = bh;
  SB_DISK_SUPER_BLOCK (s) = rs;
  s->s_op = &reiserfs_sops;
  return 0;
}

/* after journal replay, reread all bitmap and super blocks */
static int reread_meta_blocks(struct super_block *s) {
  int i ;
  ll_rw_block(READ, 1, &(SB_BUFFER_WITH_SB(s))) ;
  wait_on_buffer(SB_BUFFER_WITH_SB(s)) ;
  if (!buffer_uptodate(SB_BUFFER_WITH_SB(s))) {
    printk("reread_meta_blocks, error reading the super\n") ;
    return 1 ;
  }

  for (i = 0; i < SB_BMAP_NR(s) ; i++) {
    ll_rw_block(READ, 1, &(SB_AP_BITMAP(s)[i])) ;
    wait_on_buffer(SB_AP_BITMAP(s)[i]) ;
    if (!buffer_uptodate(SB_AP_BITMAP(s)[i])) {
      printk("reread_meta_blocks, error reading bitmap block number %d at %ld\n", i, SB_AP_BITMAP(s)[i]->b_blocknr) ;
      return 1 ;
    }
  }
  return 0 ;

}

static void print_credits(void) {
  /* avoid printing credits endlessly */
  static int did_this = 0;
  if (did_this) return;
  did_this = 1;
  printk("ReiserFS core development sponsored by SuSE Labs (suse.com)\n") ;
  printk("Journaling sponsored by MP3.com\n") ;
  printk("Item handlers sponsored by Ecila.com\n") ;
}

struct super_block * reiserfs_read_super (struct super_block * s, void * data, int silent)
{
  int size;
  struct inode *root_inode;
  kdev_t dev = s->s_dev;
  int j;
  extern int *blksize_size[];
  struct reiserfs_transaction_handle th ;
  int old_format = 0;
  unsigned long blocks;
  int jinit_done = 0 ;

  memset (&s->u.reiserfs_sb, 0, sizeof (struct reiserfs_sb_info));

  if (parse_options ((char *) data, &(s->u.reiserfs_sb.s_mount_opt), &blocks) == 0) {
    s->s_dev = 0;
    return NULL;
  }

  if (blocks) {
  	printk("reserfs: resize option for remount only\n");
	return NULL;
  }	

  MOD_INC_USE_COUNT;
  lock_super (s);

  if (blksize_size[MAJOR(dev)] && blksize_size[MAJOR(dev)][MINOR(dev)] != 0) {
    /* as blocksize is set for partition we use it */
    size = blksize_size[MAJOR(dev)][MINOR(dev)];
  } else {
    size = BLOCK_SIZE;
    set_blocksize (s->s_dev, BLOCK_SIZE);
  }

  /* read block, containing reiserfs super block (it is stored at REISERFS_FIRST_BLOCK-th 1K block) */
  if (read_super_block (s, size)) {
	  if(read_old_super_block(s,size)) 
		  goto error;
	  else
		  old_format = 1;
  }

  s->u.reiserfs_sb.s_mount_state = le16_to_cpu (SB_DISK_SUPER_BLOCK (s)->s_state); /* journal victim */
  s->u.reiserfs_sb.s_mount_state = REISERFS_VALID_FS ;

  /* reiserfs can not be mounted when it propably contains errors */
#if 0 /* journal victim */
  if (le16_to_cpu (SB_DISK_SUPER_BLOCK (s)->s_state) != REISERFS_VALID_FS) {
    printk ("reiserfs_read_super:  mounting unchecked fs, run reiserfsck first\n");
    goto error;
  }
#endif
  if (old_format ? read_old_bitmaps(s) : read_bitmaps(s)) { 
	  printk ("reiserfs_read_super: unable to read bitmap\n");
	  goto error;
  }

  if (journal_init(s)) {
    printk("reiserfs_read_super: unable to initialize journal space\n") ;
    goto error ;
  } else {
    jinit_done = 1 ; /* once this is set, journal_release must be called
                     ** if we error out of the mount 
		     */
  }
  if (reread_meta_blocks(s)) {
    printk("reiserfs_read_super: unable to reread meta blocks after journal init\n") ;
    goto error ;
  }

  if (replay_only (s))
    goto error;

  /*s->s_op = &reiserfs_sops;*/
   
  /* get root directory inode */
  store_key (s, &root_key);
  root_inode = iget (s, root_key.k_objectid);
  forget_key (s, &root_key);
  if (!root_inode) {
    printk ("reiserfs_read_super: get root inode failed\n");
    goto error;
  }

  s->s_root = d_alloc_root(root_inode, NULL);  
  if (!s->s_root) {
    iput(root_inode);
    goto error;
  }

  if (!(s->s_flags & MS_RDONLY)) {
    SB_DISK_SUPER_BLOCK (s)->s_state = cpu_to_le16 (REISERFS_ERROR_FS);
    /* mark_buffer_dirty (SB_BUFFER_WITH_SB (s), 1); */
    journal_begin(&th, s, 1) ;
    journal_mark_dirty(&th, s, SB_BUFFER_WITH_SB (s));
    journal_end(&th, s, 1) ;
    s->s_dirt = 0;
  }

  /*s->u.reiserfs_sb.unpreserve = dont_preserve (s) ? 0 : unpreserve;*/
  /* we have to do this to make journal writes work correctly */
  SB_BUFFER_WITH_SB(s)->b_end_io = reiserfs_end_buffer_io_sync ;

  unlock_super (s);
  print_credits() ;
  printk("%s\n", reiserfs_get_version_string()) ;
  return s;

 error:
  if (jinit_done) { /* kill the commit thread, free journal ram */
    journal_release_error(NULL, s) ;
  }
  if (SB_DISK_SUPER_BLOCK (s)) {
    for (j = 0; j < le16_to_cpu (SB_DISK_SUPER_BLOCK (s)->s_bmap_nr); j ++) {
      if (SB_AP_BITMAP (s))
	brelse (SB_AP_BITMAP (s)[j]);
      /* if (SB_AP_CAUTIOUS_BITMAP (s))
	brelse (SB_AP_CAUTIOUS_BITMAP (s)[j]); */
    }
    if (SB_AP_BITMAP (s))
      reiserfs_kfree (SB_AP_BITMAP (s), sizeof (struct buffer_head *) * SB_BMAP_NR (s), s);
    /* if (SB_AP_CAUTIOUS_BITMAP (s))
      reiserfs_kfree (SB_AP_CAUTIOUS_BITMAP (s), sizeof (struct buffer_head *) * SB_BMAP_NR (s), s); */
  }
  if (SB_BUFFER_WITH_SB (s))
    brelse(SB_BUFFER_WITH_SB (s));
  s->s_dev = 0;
  unlock_super(s);
  MOD_DEC_USE_COUNT;

  return NULL;
}


int reiserfs_statfs (struct super_block * s, struct statfs * buf, int bufsize)
{
  struct statfs tmp;
  struct reiserfs_super_block * rs = SB_DISK_SUPER_BLOCK (s);
  
				/* changed to accomodate gcc folks.*/
  tmp.f_type =  REISERFS_SUPER_MAGIC;
  tmp.f_bsize = le32_to_cpu (s->s_blocksize);
  tmp.f_blocks = le32_to_cpu (rs->s_block_count) - le16_to_cpu (rs->s_bmap_nr) - 1;
  tmp.f_bfree = le32_to_cpu (rs->s_free_blocks);
  tmp.f_bavail = tmp.f_bfree;
  tmp.f_files = 0;
  tmp.f_ffree = 0;
  tmp.f_namelen = (REISERFS_MAX_NAME_LEN (s->s_blocksize));
  return copy_to_user (buf, &tmp, bufsize) ? -EFAULT : 0;
}

#ifdef __KERNEL__

static struct file_system_type reiserfs_fs_type = {
  "reiserfs", FS_REQUIRES_DEV, reiserfs_read_super, NULL
};


__initfunc(int init_reiserfs_fs(void))
{
        return register_filesystem(&reiserfs_fs_type);
}

#endif

#ifdef MODULE
EXPORT_NO_SYMBOLS;

int init_module(void)
{
	return init_reiserfs_fs();
}

void cleanup_module(void)
{
        unregister_filesystem(&reiserfs_fs_type);
}

#endif
























