/*
 * Copyright 1996, 1997, 1998, 1999  Hans Reiser
 */
#include "fsck.h"
#include <stdio.h>
#include <getopt.h>
#include <sys/mount.h>

#include "reiserfs.h"

//
// by default journal will be replayed via mount -o replay-only
// --replay-whole-journal
// --no-replay-journal - journal replaying will be skipped
// --scan-whole-partition - will scan whole parititon looking for the leaves
//


#define print_usage_and_exit() die ("Usage: %s [-aprvy] [--rebuild-tree]\n"\
"[--scan-whole-partition] [--no-journal-replay]\n"\
"[--replay-whole-journal] device\n"\
"\n"\
"Long options:\n"\
"\tModes:\n"\
"\t--check                  consistency checking (default)\n"\
"\t--rebuild-tree           force fsck to rebuild filesystem from scratch\n"\
"\t                         (takes a long time)\n"\
"\t--search-key             will search for key you specify\n"\
"\n"\
"\tArea to scan:\n"\
"\t--scan-used-part-only    scan what is marked used in bitmap (default)\n"\
"\t--scan-whole-partition   scan whole partition\n"\
"\n"\
"\tJournal replay options:\n"\
"\t--replay-by-mount        replay by calling mount -o replay-only (default)\n"\
"\t--no-journal-replay      skip journal replaying\n"\
"\t--replay-whole-journal   replay all valid transaction found in the journal\n"\
"\n"\
"\tStop point specifying\n"\
"\t--do-not-stop            (default)\n"\
"\t--stop-after-replay\n"\
"\t--stop-after-pass1\n"\
"\t--stop-after-pass2\n"\
"\t--stop-after-semantic-pass\n"\
"Short options:\n"\
"\t-v verbose mode\n"\
"\t-a supress progress information\n"\
"\t-y\n"\
"\t-p do nothing, exist for compatibility with fsck(8)\n"\
"\t-r\n", argv[0]);


int opt_verbose = 0;
int opt_fsck = 0; /* called with -a by fsck - the front-end for the
		     various file system checkers */


//
// fsck has three modes: default one - is check, other two are rebuild
// and find items
//
int opt_fsck_mode = FSCK_DEFAULT;

/* in mode FSCK_FIND_ITEM keu for search is stored here */
struct key key_to_find;

//
// replay journal modes
//
#define REPLAY_DEFAULT 0
#define REPLAY_ALL 1
#define NO_REPLAY 2
int opt_journal_replay = REPLAY_DEFAULT;


//
// fsck may stop after any of its phases: after journal replay or
// after any of passes. Default is do not stop
//
int opt_stop_point = STOP_DEFAULT;


//
// 
//
int opt_what_to_scan = SCAN_USED_PART;


//
//
//
int opt_lost_found = NO_LOST_FOUND;



/* fsck is called with one non-optional argument - file name of device
   containing reiserfs. This function parses other options, sets flags
   based on parsing and returns non-optional argument */
static char * parse_options (int argc, char * argv [])
{
    int c;

    while (1) {
	static struct option options[] = {
	    // mode options
	    {"check", no_argument, &opt_fsck_mode, FSCK_DEFAULT},
	    {"rebuild-tree", no_argument, &opt_fsck_mode, FSCK_REBUILD},
	    {"search-key", no_argument, &opt_fsck_mode, FSCK_FIND_ITEM},

	    // journal replay options
	    {"replay-by-mount", no_argument, &opt_journal_replay, REPLAY_DEFAULT},
	    {"no-journal-replay", no_argument, &opt_journal_replay, NO_REPLAY},
	    {"replay-whole-journal", no_argument, &opt_journal_replay, REPLAY_ALL},

	    // stop point options
	    {"do-not-stop", no_argument, &opt_stop_point, STOP_DEFAULT},
	    {"stop-after-replay", no_argument, &opt_stop_point, STOP_AFTER_REPLAY},
	    {"stop-after-pass1", no_argument, &opt_stop_point, STOP_AFTER_PASS1},
	    {"stop-after-pass2", no_argument, &opt_stop_point, STOP_AFTER_PASS2},
	    {"stop-after-semantic-pass", no_argument, &opt_stop_point, STOP_AFTER_SEMANTIC},

	    // scanned area option
	    {"scan-used-part-only", no_argument, &opt_what_to_scan, SCAN_USED_PART},
	    {"scan-whole-partition", no_argument, &opt_what_to_scan, SCAN_WHOLE_PARTITION},

	    // lost+found
	    {"no-lost+found", no_argument, &opt_lost_found, NO_LOST_FOUND},
	    {"lost+found",  no_argument, &opt_lost_found, DO_LOST_FOUND},
	    {0, 0, 0, 0}
	};
	int option_index;
      
	c = getopt_long (argc, argv, "yapv", options, &option_index);
	if (c == -1)
	    break;

	switch (c) {
	case 0:
	    switch (option_index) {
	    case 0: /* check */
	    case 1: /* rebuild */
	    case 2: /* find */
		break;

	    case 3: /* replay by mount */
	    case 4: /* no journal replay */
	    case 5: /* replay whole journal */
		break;

	    case 6: /* do not stop */
	    case 7: /* stop after replay */
	    case 8: /* stop after pass 1 */
	    case 9: /* stop after pass 2 */
	    case 10: /* stop after semantic */
		break;
	    case 11: /* scan used part of partition */
	    case 12: /* scan whole partition */
		break;
		
	    }
	    break;

	case 'y':
	case 'p': /* these do nothing */
	case 'r':
	    break;

	case 'a':
	    opt_fsck = 1;
	    break;

	case 'v':
	    /* output fsck statistics to stdout on exit */
	    opt_verbose = 1;
	    break;

	default:
	    print_usage_and_exit();
	}
    }

  if (optind != argc - 1)
    /* only one non-option argument is permitted */
    print_usage_and_exit();
  
  return argv[optind];
}


struct super_block g_sb;
struct buffer_head * g_sbh;
struct reiserfs_super_block * g_old_rs;


static void reset_super_block (struct super_block * s)
{
    unsigned long * oids;

    g_old_rs = (struct reiserfs_super_block *)getmem (s->s_blocksize);
    memcpy (g_old_rs, SB_BUFFER_WITH_SB (s)->b_data, s->s_blocksize);

    /* reset few fields in */
    SB_FREE_BLOCKS (s) = SB_BLOCK_COUNT (s);
    SB_TREE_HEIGHT (s) = ~0;
    SB_ROOT_BLOCK (s) = ~0;
    s->u.reiserfs_sb.s_mount_state = REISERFS_ERROR_FS;
    s->u.reiserfs_sb.s_rs->s_oid_cursize = 2;
    oids = (unsigned long *)(s->u.reiserfs_sb.s_rs + 1);
    if (oids[0] != 1) {
	printf ("reset_super_block: invalid objectid map\n");
	oids[0] = 1;
    }
    oids[1] = 2;
    s->s_dirt = 1;

    mark_buffer_dirty (SB_BUFFER_WITH_SB (s), 0);
}

static void update_super_block (void)
{
    SB_REISERFS_STATE (&g_sb) = REISERFS_VALID_FS;
    
    reset_journal (&g_sb);

    mark_buffer_dirty (SB_BUFFER_WITH_SB (&g_sb), 0);
}


char ** g_disk_bitmap;
char ** g_new_bitmap;
char ** g_uninsertable_leaf_bitmap;
char ** g_formatted;
char ** g_unformatted;
int g_blocks_to_read;


/* read bitmaps (new or old format), create data blocks for new
   bitmap, mark non-data blocks in it (skipped, super block, journal
   area, bitmaps) used, create other auxiliary bitmaps */
static void init_bitmaps (struct super_block * s)
{
    int i, j;

    /* read disk bitmap */
    if (uread_bitmaps (s))
	die ("init_bitmap: unable to read bitmap");

    g_disk_bitmap = getmem (sizeof (char *) * SB_BMAP_NR (s));
    for (i = 0; i < SB_BMAP_NR (s); i ++) {
	g_disk_bitmap[i] = SB_AP_BITMAP (s)[i]->b_data;

	if (opt_what_to_scan == SCAN_WHOLE_PARTITION)
	    /* mark all blocks busy */
	    memset (g_disk_bitmap[i], 0xff, s->s_blocksize);
    }


    /* g_blocks_to_read is used to report progress */
    if (opt_what_to_scan == SCAN_WHOLE_PARTITION)
	/* all blocks will be scanned */
	g_blocks_to_read = SB_BLOCK_COUNT (s);
    else {
	/* blocks marked used in bitmap will be scanned */
	g_blocks_to_read = 0;
	for (i = 0; i < SB_BMAP_NR (s); i ++) {
	    for (j = 0; j < s->s_blocksize * 8; j ++)
		if (i * s->s_blocksize * 8 + j < SB_BLOCK_COUNT (s) &&
		    test_bit (j, SB_AP_BITMAP (s)[i]->b_data))
		    g_blocks_to_read ++;
	}
    }

    /* this bitmap will contain valid bitmap when fsck will have done */
    g_new_bitmap = getmem (sizeof (char *) * SB_BMAP_NR (s));
    for (i = 0; i < SB_BMAP_NR (s); i ++)
	g_new_bitmap[i] = getmem (s->s_blocksize);

    /* mark skipped blocks and super block used */
    for (i = 0; i <= SB_BUFFER_WITH_SB (s)->b_blocknr; i ++)
	mark_block_used (i);

    /* mark bitmap blocks as used */
    for (i = 0; i < SB_BMAP_NR (s); i ++)
	mark_block_used (SB_AP_BITMAP (s)[i]->b_blocknr);

    /* mark journal area as used */
    for (i = 0; i < JOURNAL_BLOCK_COUNT + 1; i ++)
	mark_block_used (i + get_journal_start (s));

    /* fill by 1s the unused part of last bitmap */
    if (SB_BLOCK_COUNT (s) % (s->s_blocksize * 8))
	for (j = SB_BLOCK_COUNT (s) % (s->s_blocksize * 8); j < s->s_blocksize * 8; j ++)
	    set_bit (j, g_new_bitmap[SB_BMAP_NR (s) - 1]);

    /* allocate space for bitmap of uninsertable leaves */
    g_uninsertable_leaf_bitmap = getmem (sizeof (char *) * SB_BMAP_NR (s));
    for (i = 0; i < SB_BMAP_NR (s); i ++) {
	g_uninsertable_leaf_bitmap[i] = getmem (s->s_blocksize);
	memset (g_uninsertable_leaf_bitmap[i], 0xff, s->s_blocksize);
    }

    /* bitmap of formatted nodes */
    g_formatted = getmem (sizeof (char *) * SB_BMAP_NR (s));
    for (i = 0; i < SB_BMAP_NR (s); i ++) {
	g_formatted[i] = getmem (s->s_blocksize);
	memset (g_formatted[i], 0, s->s_blocksize);
    }
    /* bitmap of unformatted nodes */
    g_unformatted = getmem (sizeof (char *) * SB_BMAP_NR (s));
    for (i = 0; i < SB_BMAP_NR (s); i ++) {
	g_unformatted[i] = getmem (s->s_blocksize);
	memset (g_unformatted[i], 0, s->s_blocksize);
    }
}


/* write bitmaps and brelse them */
static void update_bitmap (struct super_block * s)
{
    int i;

    /* journal area could be used, reset it */
    for (i = 0; i < get_journal_start (s) + get_journal_size (s) + 1; i ++)
	if (!is_block_used (i))
	    mark_block_used (i);

    for (i = 0; i < SB_BMAP_NR (s); i ++) {

	/* copy newly built bitmap to cautious bitmap */
	memcpy (SB_AP_BITMAP (s)[i]->b_data, g_new_bitmap[i], s->s_blocksize);
	mark_buffer_dirty (SB_AP_BITMAP (s)[i], 0);
	bwrite (SB_AP_BITMAP (s)[i]);
    

	freemem (g_new_bitmap[i]);
	/* g_disk_bitmap[i] points to corresponding cautious bitmap's b_data */
	freemem (g_uninsertable_leaf_bitmap[i]);
    }

    freemem (g_disk_bitmap);
    freemem (g_new_bitmap);
    freemem (g_uninsertable_leaf_bitmap);

}


static void release_bitmap (void)
{
    int i;

    for (i = 0; i < SB_BMAP_NR (&g_sb); i ++) {
	brelse (SB_AP_BITMAP (&g_sb)[i]);
    }
}

static void release_super_block (void)
{
    bwrite (SB_BUFFER_WITH_SB (&g_sb));
    freemem (SB_AP_BITMAP (&g_sb));
    brelse (SB_BUFFER_WITH_SB (&g_sb));

    freemem (g_old_rs);
}



static void mount_replay (char * devname1)
{
    int retval;
    char * tmpdir;

    printf ("Replaying journal.."); fflush (stdout);

    tmpdir = tmpnam (0);
    if (!tmpdir || mkdir (tmpdir, 0644))
	die ("replay_journal: tmpnam or mkdir failed: %s", strerror (errno));
 
    retval = mount (devname1, tmpdir, "reiserfs", MS_MGC_VAL, "replayonly");
    if (retval != -1 || errno != EINVAL) {
	printf ("\nMAKE SURE, THAT YOUR KERNEL IS ABLE TO MOUNT REISERFS\n");
	die ("replay_journal: mount returned unexpected value: %s", 
	     strerror (errno));
    }

    if (rmdir (tmpdir) == -1)
	die ("replay_journal: rmdir failed: %s", strerror (errno));

    printf ("ok\n"); fflush (stdout);
}


static inline int nothing_todo (struct super_block * s)
{
    if (opt_fsck)
	return 1;
    return 0;
}


void write_dirty_blocks (void)
{
  fsync_dev (0);
}


#define WARNING \
"Don't run this program unless something is broken.  You may want\n\
to backup first.  Some types of random FS damage can be recovered\n\
from by this program, which basically throws away the internal nodes\n\
of the tree and then reconstructs them.  This program is for use only\n\
by the desperate, and is of only beta quality.  Email\n\
reiserfs@devlinux.com with bug reports. \n"

/* 
   warning #2
   you seem to be running this automatically.  you are almost
   certainly doing it by mistake as a result of some script that
   doesn't know what it does.  doing nothing, rerun without -p if you
   really intend to do this.  */

void warn_what_will_be_done (void)
{
    char * answer = 0;
    size_t n = 0;

    /* warn about fsck mode */
    switch (opt_fsck_mode) {
    case FSCK_DEFAULT:
	printf ("Will read-only check consistency of the partition\n");
	break;

    case FSCK_REBUILD:
	printf (WARNING);
	break;

    case FSCK_FIND_ITEM:
	printf ("Will look for the item with key\n");
	break;
    }

    /* warn about replay */
    switch (opt_journal_replay) {
    case REPLAY_DEFAULT:
	printf ("Will replay just like mounting would\n");
	break;
	
    case REPLAY_ALL:
	printf ("Will replay all valid transactions\n"); break;

    case NO_REPLAY:
	printf ("Will not replay journal\n"); break;
    }

    /* warn about stop point */
    switch (opt_stop_point) {
    case STOP_AFTER_REPLAY:
	printf ("Will stop after journal replay\n"); break;
    case STOP_AFTER_PASS1:
	printf ("Will stop after pass 1\n"); break;

    case STOP_AFTER_PASS2:
	printf ("Will stop after pass 2\n"); break;

    case STOP_AFTER_SEMANTIC:
	printf ("Will stop after semantic pass\n"); break;
    }


    /* warn about scanned area */
    if (opt_what_to_scan == SCAN_WHOLE_PARTITION)
	printf ("Will scan whole partition\n");
    
    printf ("Do you want to run this "
	    "program?[N/Yes] (note need to type Yes):");
    if (getline (&answer, &n, stdin) != 4 || strcmp ("Yes\n", answer)) {
	exit (0);
    }

    if (opt_fsck_mode == FSCK_FIND_ITEM) {
	printf ("Specify key to search:");
	if (scanf ("%d %d %d %d", &(key_to_find.k_dir_id), &(key_to_find.k_objectid),
		   &(key_to_find.k_offset), &(key_to_find.k_uniqueness)) != 4)
	    die ("parse_options: specify a key through stdin");
    }
}


void end_fsck (char * file_name)
{
    update_super_block ();
    update_bitmap (&g_sb);
    release_bitmap ();
    release_super_block ();
    
    if (opt_verbose == 1)
	output_information ();

    printf ("Syncing.."); fflush (stdout);
    
    write_dirty_blocks ();
    sync ();
    
    printf ("done\n"); fflush (stdout);
    
    if (opt_verbose == 1)
	printf ("Checking mem..");
    
    free_overwritten_unfms ();
    check_and_free_buffer_mem ();

    if (opt_verbose == 1)
	printf ("done\n");

    if (opt_fsck == 1)
	printf("ReiserFS : done checking %s\n", file_name);
    else
	printf ("Ok\n");
    exit (0);
}


static void open_device (char * file_name, int flag)
{
    g_sb.s_dev = open (file_name, flag);
    if (g_sb.s_dev == -1)
	die ("reiserfsck: can not open '%s': %s", file_name, strerror (errno));
}  

static void reopen_read_only (char * file_name)
{
    close (g_sb.s_dev);
    open_device (file_name, O_RDONLY);
}

static void reopen_read_write (char * file_name)
{
    close (g_sb.s_dev);
    open_device (file_name, O_RDWR);
}


/* ubitmap.c: */extern int from_journal;


int main (int argc, char * argv [])
{
    char * file_name;
 
    if (opt_fsck == 0)
	printf ("\n\n<-----------REISERFSCK, 1999----------->\n\n");


    file_name = parse_options (argc, argv);
    if (is_mounted (file_name))
	/* do not work on mounted filesystem for now */
	die ("reiserfsck: '%s' contains a mounted file system\n", file_name);


    warn_what_will_be_done (); /* and ask confirmation Yes */


    if (opt_journal_replay == REPLAY_DEFAULT)
	mount_replay (file_name);

    open_device (file_name, O_RDONLY);
    
    if (uread_super_block (&g_sb))
	die ("reiserfsck: no reiserfs found");

    if (opt_journal_replay == REPLAY_ALL) {
	/* read-write permissions are needed */
	reopen_read_write (file_name);
	replay_all (&g_sb);
	reopen_read_only (file_name);
    }


    if (nothing_todo (&g_sb)) {
	/* this should work when fsck is called by fsck -a */
	printf ("%s: clean, %d/%d %ldk blocks\n", file_name,
		SB_BLOCK_COUNT (&g_sb) - SB_FREE_BLOCKS(&g_sb), SB_BLOCK_COUNT (&g_sb), g_sb.s_blocksize / 1024);
	brelse (SB_BUFFER_WITH_SB (&g_sb));
	return 0;
    }


    if (opt_fsck_mode == FSCK_DEFAULT) {
	check_fs_tree (&g_sb);
	release_bitmap ();
	release_super_block ();
	check_and_free_buffer_mem ();
	exit (0);
    }

    if (opt_stop_point == STOP_AFTER_REPLAY) {
	release_super_block ();
	check_and_free_buffer_mem ();
	exit (0);	
    }
	

    if (opt_fsck_mode == FSCK_REBUILD) {
	reopen_read_write (file_name);

	if (opt_fsck == 1)
	    printf ("ReiserFS : checking %s\n",file_name);
	else
	    printf ("Rebuilding..\n");

	reset_super_block (&g_sb);
	init_bitmaps (&g_sb);
	
	/* make file system invalid unless fsck done */
	SB_REISERFS_STATE (&g_sb) = REISERFS_ERROR_FS;
	bwrite (SB_BUFFER_WITH_SB (&g_sb));
	/* 1,2. building of the tree */
	build_the_tree ();

	/* 3. semantic pass */
	semantic_pass ();

	/* if --lost+found is set - link unaccessed directories to
           lost+found directory */
	pass4 (&g_sb);

	/* 4. look for unaccessed items in the leaves */
	check_unaccessed_items ();


	if (from_journal)
	    /* blocks from journal area could get into tree, fix that */
	    release_journal_blocks (&g_sb);

	end_fsck (file_name);
    }


    if (opt_fsck_mode == FSCK_FIND_ITEM) {
	init_bitmaps (&g_sb);
	build_the_tree ();
	release_bitmap ();
	release_super_block ();
	check_and_free_buffer_mem ();
	exit (0);
    }


    return 0;
}
