/*
 * Copyright 2000 Hans Reiser
 */

#include "fsck.h"
#include <limits.h>
#include "reiserfs.h"


/* compares description block with commit block.  returns 1 if they differ, 0 if they are the same */
static int journal_compare_desc_commit(struct super_block *p_s_sb, struct reiserfs_journal_desc *desc, 
			               struct reiserfs_journal_commit *commit) {
  if (commit->j_trans_id != desc->j_trans_id || commit->j_len != desc->j_len || commit->j_len > JOURNAL_TRANS_MAX || 
      commit->j_len <= 0 
  ) {
    return 1 ;
  }
  return 0 ;
}


//
// set up start journal block and journal size
// make journal unreplayable by kernel replay routine
//
void reset_journal (struct super_block * s)
{
    int i ;
    struct buffer_head *bh ;
    int done = 0;
    int len;
    int start;

    /* first block of journal */
    s->u.reiserfs_sb.s_rs->s_journal_block = get_journal_start (s);
    start = s->u.reiserfs_sb.s_rs->s_journal_block;

    /* journal size */
    s->u.reiserfs_sb.s_rs->s_orig_journal_size = get_journal_size (s);
    len = s->u.reiserfs_sb.s_rs->s_orig_journal_size + 1;

    printf ("Resetting journal - "); fflush (stdout);

    for (i = 0 ; i < len ; i++) {
	print_how_far (&done, len);
	bh = getblk (s->s_dev, start + i, s->s_blocksize) ;
	memset(bh->b_data, 0, s->s_blocksize) ;
	mark_buffer_dirty(bh,0) ;
	mark_buffer_uptodate(bh,0) ;
	bwrite (bh);
	brelse(bh) ;
    }
    printf ("\n"); fflush (stdout);
    
#if 0 /* need better way to make journal unreplayable */


    /* have journal_read to replay nothing: look for first non-desc
       block and set j_first_unflushed_offset to it */
    {   
	int offset;
	struct buffer_head * bh, *jh_bh;
	struct reiserfs_journal_header * j_head;
	struct reiserfs_journal_desc * desc;


	jh_bh = bread (s->s_dev, s->u.reiserfs_sb.s_rs->s_journal_block + s->u.reiserfs_sb.s_rs->s_orig_journal_size,
		       s->s_blocksize);
	j_head = (struct reiserfs_journal_header *)(jh_bh->b_data);

	for (offset = 0; offset < s->u.reiserfs_sb.s_rs->s_orig_journal_size; offset ++) {
	    bh = bread (s->s_dev, s->u.reiserfs_sb.s_rs->s_journal_block + offset, s->s_blocksize);
	    desc = (struct reiserfs_journal_desc *)((bh)->b_data);
	    if (memcmp(desc->j_magic, JOURNAL_DESC_MAGIC, 8)) {
		/* not desc block found */
		j_head->j_first_unflushed_offset = offset;
		brelse (bh);
		break;
	    }
	    brelse (bh);
	}

	mark_buffer_uptodate (jh_bh, 1);
	mark_buffer_dirty (jh_bh, 1);
	bwrite (jh_bh);
	brelse (jh_bh);
    }
#endif
}

//
// end of stolen from ./fs/reiserfs/journal.c
//


#define bh_desc(bh) ((struct reiserfs_journal_desc *)((bh)->b_data))
#define bh_commit(bh) ((struct reiserfs_journal_commit *)((bh)->b_data))





static int desc_block (struct buffer_head * bh)
{
    struct reiserfs_journal_desc * desc = (struct reiserfs_journal_desc *)bh->b_data;
    if (!memcmp(desc->j_magic, JOURNAL_DESC_MAGIC, 8))
	return 1;
    return 0;
}

static int next_expected_desc (struct super_block * s, struct buffer_head * d_bh)
{
    int offset;
    struct reiserfs_journal_desc * desc;

    desc = (struct reiserfs_journal_desc *)d_bh->b_data;
    offset = d_bh->b_blocknr - get_journal_start (s);
    return get_journal_start (s) + ((offset + desc->j_len + 1 + 1) % JOURNAL_BLOCK_COUNT);
}


static int is_valid_transaction (struct super_block * s, struct buffer_head * d_bh)
{
    struct buffer_head * c_bh;
    int offset;
    struct reiserfs_journal_desc *desc  = (struct reiserfs_journal_desc *)d_bh->b_data;
    struct reiserfs_journal_commit *commit ;


    offset = d_bh->b_blocknr - get_journal_start (s);
    
    /* ok, we have a journal description block, lets see if the transaction was valid */
    c_bh = bread (s->s_dev, next_expected_desc (s, d_bh) - 1,
		  s->s_blocksize) ;
    
    commit = (struct reiserfs_journal_commit *)c_bh->b_data ;
    if (journal_compare_desc_commit (s, desc, commit)) {
/*	printf ("desc and commit block do not match\n");*/
	brelse (c_bh) ;
	return 0;
    }
    brelse (c_bh);
    return 1;
}


int next_desc (struct super_block * s, int this)
{
    int j;
    struct buffer_head * bh;
    int retval;

    j = this + 1;
    do {
	bh = bread (s->s_dev, (j % JOURNAL_BLOCK_COUNT), s->s_blocksize);
	if (!desc_block (bh)) {
	    j ++;
	    brelse (bh);
	    continue;
	}
/*	printf ("desc block found %lu, trans_id %ld, len %ld\n",
		bh->b_blocknr, bh_desc(bh)->j_trans_id, bh_desc(bh)->j_len);*/
	retval = (j % JOURNAL_BLOCK_COUNT);
	brelse (bh);
	break;
    } while (1);

    return retval;
}


void replay_all (struct super_block * s)
{
    int first_journal_block = get_journal_start (s);
    int journal_size = get_journal_size (s);
    struct buffer_head * d_bh, * c_bh;
    struct reiserfs_journal_desc *desc ;
    struct reiserfs_journal_commit *commit ;
    int i;
    int the_most_old_transaction = INT_MAX;
    int the_most_young_transaction = 0;
    int valid_transactions = 0;
    int last_replayed;
    int start_replay = 0;


    /* look for oldest valid transaction */
    printf ("Looking for the oldest transaction to start with %4d", valid_transactions);
    for (i = first_journal_block; i < first_journal_block + journal_size; i ++) {
	d_bh = bread (s->s_dev, i, s->s_blocksize);
	if (desc_block (d_bh)) {
	    desc = (struct reiserfs_journal_desc *)d_bh->b_data;
	    /*printf ("block %ld is desc block of the transaction (trans_id %ld, len %ld, mount_id %ld) - ", 
		    d_bh->b_blocknr, desc->j_trans_id, desc->j_len, desc->j_mount_id);*/
	    if (!is_valid_transaction (s, d_bh)) {
		i += desc->j_len + 1;
		brelse (d_bh);
		continue;
	    }
	    valid_transactions ++;
	    printf ("\b\b\b\b    \b\b\b\b%4d", valid_transactions); fflush (stdout);
	    
	    /*printf ("good\n");*/
	    if (the_most_old_transaction > desc->j_trans_id) {
		the_most_old_transaction = desc->j_trans_id;
		start_replay = d_bh->b_blocknr;
	    }
	    if (the_most_young_transaction < desc->j_trans_id) {
		the_most_young_transaction = desc->j_trans_id;
		start_replay = d_bh->b_blocknr;
	    }
	    i += desc->j_len + 1;
	}
	brelse (d_bh);
	continue;
    }

    printf ("\b\b\b\b     \b\b\b\bok\n"
	    "%d valid trans found. Will replay from %d to %d\n", valid_transactions,
	    the_most_old_transaction, the_most_young_transaction);


    printf ("Replaying transaction..%4d left..\b\b\b\b\b\b\b", valid_transactions);

    /* replay all valid transaction */
    last_replayed = 0;

    while (1) {
	d_bh = bread (s->s_dev, start_replay, s->s_blocksize);
	if (!desc_block (d_bh)) {
/*	    printf ("No desc block found at the expected place %lu\n", d_bh->b_blocknr);*/
	    brelse (d_bh);
	    start_replay = next_desc (s, start_replay);
	    continue;
	}

	desc = bh_desc (d_bh);

	if (!is_valid_transaction (s, d_bh)) {
/*	    printf ("skip invalid transaction %ld (length %ld) starting from %lu\n", desc->j_trans_id, desc->j_len, d_bh->b_blocknr);*/
	    brelse (d_bh);
	    start_replay = next_desc (s, start_replay);
	    continue;
	}
	
	if (desc->j_trans_id < last_replayed) {
	    /* we found transaction that has been replayed already */
	    brelse (d_bh);
/*	    printf ("Found transaction %ld. last replayed %d\n", desc->j_trans_id, last_replayed);*/
	    break;
	}
/*	printf ("Replay transaction %ld (length %ld)-", desc->j_trans_id, desc->j_len);*/


	/* replay transaction */
	{
	    int trans_offset = d_bh->b_blocknr - get_journal_start (s);
	    struct buffer_head * log_bh, * in_place;


	    c_bh = bread (s->s_dev, get_journal_start (s) + ((trans_offset + desc->j_len + 1) % JOURNAL_BLOCK_COUNT), 
			  s->s_blocksize) ;
	
	    desc = bh_desc (d_bh);
	    commit = bh_commit (c_bh);
	    if (journal_compare_desc_commit(s, desc, commit))
		die ("read_journal: invalid transaction");

	    for (i = 0; i < desc->j_len; i ++) {
		/* read from log record */
		log_bh = bread (s->s_dev, get_journal_start (s) + (trans_offset + 1 + i) % JOURNAL_BLOCK_COUNT,
				s->s_blocksize);
		if (log_bh->b_blocknr == 8199)
		    printf ("block 8199 put in-placen\n");
		/* write in-place */
		if (i < JOURNAL_TRANS_HALF) {
		    in_place = getblk(s->s_dev, desc->j_realblock[i], s->s_blocksize) ;
		} else {
		    in_place = getblk(s->s_dev, commit->j_realblock[i - JOURNAL_TRANS_HALF], s->s_blocksize) ;
		}
		if (log_bh->b_blocknr == 8199) {
		    printf ("Put 8199 to %lu\n", in_place->b_blocknr);
		}
		memcpy (in_place->b_data, log_bh->b_data, s->s_blocksize);
		mark_buffer_dirty (in_place, 0);
		mark_buffer_uptodate (in_place, 1);
		bwrite (in_place);
		brelse (in_place);
		brelse (log_bh);
	    }
	    brelse (c_bh);
	}
	valid_transactions --;
	printf ("\b\b\b\b    \b\b\b\b%4d", valid_transactions); fflush (stdout);
	last_replayed = desc->j_trans_id;
	start_replay = next_expected_desc (s, d_bh);
	brelse (d_bh);
    }
    printf (" left .. ok\n");
}


//
// these duplicate the same from fsck/check_tree.c
//
static inline blocknr_t first_child (struct buffer_head * bh)
{
    return B_N_CHILD (bh, 0)->dc_block_number;
}


static inline blocknr_t last_child (struct buffer_head * bh)
{
    return B_N_CHILD (bh, B_NR_ITEMS (bh))->dc_block_number;
}


static inline blocknr_t next_child (struct buffer_head * child,
				    struct buffer_head * parent)
{
    int i;
    
    for (i = 0; i < B_NR_ITEMS (parent); i ++) {
	if (B_N_CHILD (parent, i)->dc_block_number == child->b_blocknr)
	    return B_N_CHILD (parent, i + 1)->dc_block_number;
    }
    die ("next_child: no child found: should not happen");
    return 0;
}


/* h == 0 for root level. block head's level == 1 for leaf level  */
static inline int h_to_level (struct super_block * s, int h)
{
    return SB_TREE_HEIGHT (s) - h - 1;
}


static inline int leaf_level (struct buffer_head * bh)
{
    return B_LEVEL(bh) == DISK_LEAF_NODE_LEVEL;
}


static void print (int cur, int total)
{
  printf ("/%3d (of %3d)", cur, total);fflush (stdout);
}


/* erase /XXX(of XXX) */
static void erase (void)
{
    printf ("\b\b\b\b\b\b\b\b\b\b\b\b\b");
    printf ("             ");
    printf ("\b\b\b\b\b\b\b\b\b\b\b\b\b");
    fflush (stdout);
}


/* the simplest scanning for free block., This should be rare */
__u32 alloc_block (void)
{
    int i, j;
    int bits = g_sb.s_blocksize * 8;
    int start = get_journal_start (&g_sb) + get_journal_size (&g_sb) + 1;

    for (i = 0; i < SB_BMAP_NR (&g_sb); i ++) {
	j = find_next_zero_bit (g_new_bitmap[i], bits, start);
	if (j < bits) {
	    mark_block_used (j + i * bits);
	    return j + i * bits;
	}
	start = 0;
    }
    die ("allocate_block: no free blocks");
    return 0;
	
}

struct buffer_head * copy_contents (struct buffer_head * from)
{
    struct buffer_head * bh;
    __u32 new;

    new = alloc_block ();
    bh = getblk (from->b_dev, new, from->b_size);
    memcpy (bh->b_data, from->b_data, bh->b_size);
    mark_buffer_uptodate (bh, 1);
    mark_buffer_dirty (bh, 1);
    bwrite (bh);
    return bh;
}


static void update_pointer (struct buffer_head * parent, __u32 new, __u32 old)
{
    int i;
       
    for (i = 0; i <= B_NR_ITEMS (parent); i ++) {
	if (B_N_CHILD (parent, i)->dc_block_number == old) {
	    B_N_CHILD (parent, i)->dc_block_number = new;
	    mark_buffer_dirty (parent, 1);
	    return;
	}
    }
    die ("update_pointer: old pointer not found");
}


static int block_from_journal (struct super_block * s, __u32 block)
{
    if(block && block < get_journal_start (s)) {
	printf ("not data block (%d) got into tree. Should not appear, but fixable\n", block);
	return 0;
    }
    if (block >= get_journal_start (s) && block <= get_journal_start (s) + get_journal_size (s))
	/* <= must_journal_end due to journal header */
	return 1;
    return 0;
}


/* sometimes indirect items point to blocks from journal. Replace them
   with data blocks. I believe this is rare case */
static void correct_indirect_items (struct super_block * s, struct buffer_head * bh)
{
    int i, j;
    struct item_head * ih;
    __u32 * unfm;

    ih = B_N_PITEM_HEAD (bh, 0);
    for (i = 0; i < B_NR_ITEMS (bh); i ++, ih ++) {
	if (!I_IS_INDIRECT_ITEM (ih))
	    continue;
	unfm = (__u32 *)B_I_PITEM (bh, ih);
	for (j = 0; j < I_UNFM_NUM (ih); j++) {
	    if (block_from_journal (s, unfm[j])) {
		struct buffer_head * from, * to;

		from = bread (bh->b_dev, unfm[j], bh->b_size);
		to = copy_contents (from);
		unfm[j] = to->b_blocknr;
		mark_buffer_dirty (bh, 1);
		brelse (from);
		brelse (to);
	    }
	}
    }
}



/* sometimes, (hopefully very rare) we have to use journal blocks to
   complete tree building. In this case we have to find all those
   blocks and replace them with data blocks (Those must exist to this
   time. We have to look such blocks also when start of  */
void release_journal_blocks (struct super_block * s)
{
    struct buffer_head * path[MAX_HEIGHT] = {0,};
    int total[MAX_HEIGHT] = {0,};
    int cur[MAX_HEIGHT] = {0,};
    int h = 0;


    blocknr_t block = SB_ROOT_BLOCK (s);

    printf ("%d blocks from journal area [%d %d] has been used to perform repairing. Will release them. This may take a while\nScanning tree..",
	    from_journal, get_journal_start (s), 
	    get_journal_start (s) + get_journal_size (s));


    while ( 1 ) {
	if (path[h])
	    die ("release_journal_blocks: empty slot expected");
	
	if (h)
	    print (cur[h - 1], total[h - 1]);

	path[h] = bread (s->s_dev, block, s->s_blocksize);
	if (path[h] == 0)
	    die ("release_journal_blocks: bread failed");

	if (block_from_journal (s, path[h]->b_blocknr)) {
	    /* copy block to newly allocated, adjust pointer in the
               parent, replace on the path */
	    struct buffer_head * bh;
	    __u32 old = path[h]->b_blocknr;

	    bh = copy_contents (path[h]);
	    brelse (path[h]);
	    path[h] = bh;
	    if (h) {
		/* adjust corresponding dc_child_num in the parent*/
		update_pointer (path[h - 1], bh->b_blocknr, old);
	    } else {
		/* change pointer from super block */
		SB_ROOT_BLOCK (s) = bh->b_blocknr;
	    }
	}

 	if (leaf_level (path[h])) {
	    /* correct unformatted node pointers if they point to the
               journal area */
	    correct_indirect_items (s, path[h]);

	    brelse (path[h]);
	    if (h)
	      erase ();

	    while (h && path[h]->b_blocknr == last_child (path[h - 1])) { 
		path[h] = 0;
		h --;
		brelse (path[h]);
		if (h)
		  erase ();
	    }

	    if (h == 0) {
		path[h] = 0;
		break;
	    }

	    cur[h - 1] ++;
	    block = next_child (path[h], path[h-1]);
	    path[h] = 0;
	    continue; 
	}
	total[h] = B_NR_ITEMS (path[h]) + 1;
	cur[h] = 1;
	block = first_child (path[h]);
	h ++;
    }
    
    printf ("ok\n");
}
