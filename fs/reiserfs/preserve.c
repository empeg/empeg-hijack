/*
 * Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */

/* There are two schemes we can choose from in ensuring that shifted
objects are not obliterated on disk before they are written to their
new location.  

One is to track the shifting, and this was the approach taken by the
flush cells approach that was first tried.  It was complex, unfinished
for handling formatted/unformatted item conversions, and we decided to
do something simpler.  It may have been a valid approach, it was
probably more efficient for files under 10k, but it is abandoned for
now.  The preserve list has interesting possibilities for it to be
enhanced in the future in ways that are of interest.

The other approach is to write all formatted leaf nodes next to rather
than on top of themselves, and to restrict the freeing of these formatted
nodes so that it is only performed at times when there are no dirtied leaf
buffers.  It is not necessary to restrict the freeing of unformatted
nodes since objects are never shifted from one unformatted node to
another.  This approach pays a price in efficiency, in that when a
file system is heavily loaded it may never have a moment of having no
dirty buffers, and it is necesssary to force the existence of such a
moment by flushing all dirty buffers.  It reduces the precision of
layout optimization performed in reiserfs_new_block by causing blocks to
wander.  This is somewhat alleviated by the possibility that a block may
be placed closer to its optimal position if it was not optimally placed
to begin with.  It also wastes space, and requires writing error code
to handle lack of disk space caused by nodes being consumed by this.
It offers three benefits: 1) it potentially allows recovery from
partial node write which the flush cells approach inherently could
not, 2) it potentially enables a fast recovery mechanism that might
prevent people from suggesting that I add a log to this system, 3) it
is simpler.

The preserve list code involved modifications to:
preserve.c
reiserfs_fs.h (read the comments for the preserve list data structures.)
fs.h (especially notice mark_buffer_dirty() )
buffer.c
bitmap.c
fix_node.c
locks.h

plus you should tags-search for all places in linux that call the functions that are in preserve.c.

All operations that involve super blocks, internal nodes, bitmap
blocks, oids, or operations that alter but do not balance leaves, do
not need to use preserve_*() calls.  At some point in the future I
want to try preserving more and more, and measuring the performance
effect, for now a minimal approach is taken.  

We must restrict the freeing of those nodes which have had objects
shifted from them.  Nothing else needs to be duplicated to be
adequately recoverable, just shifting.  Note that tail conversion and
rename involve shifting.  

The set of preserved nodes is the set of nodes which have shifted
items out and which are not new nodes that have never been written to disk.

So what we do is keep a count of all blocks which are dirty suspected
recipients.  The set of BH_Suspected_Recipient flagged nodes is the
set of nodes which might have received those shifted items.  That is
to say, any dirty node whose node type is one which can receive
shifted items (for all such nodes, B_IS_COUNTED(bh) is true).
Actually, to simplify the recovery algorithm, we also flag bitmap
blocks as BH_Suspected_Recipient when they are dirtied.  When we set
this flag we increase the suspected_recipient_count in the superblock

We create a BH_PRESERVED buffer state flag to avoid preserving
multiple blocks for one buffer when doing multiple shifts from the
same buffer prior to it being written to disk.  BH_Preserved and
BH_Suspected_Recipient are cleared on buffer write completion or the
buffer being invalidated (Nodes are invalidated as a result of:
deleting their contents, when all of the nodes contents are shifted by
balancing to other nodes, and when an unformatted node is converted to
a direct item as a result of truncation.)  using the function
unpreserve().  However, we still don't try to track the shifting, and
we only free preserved nodes when no BH_Suspected_Recipient flagged
buffers exist.  A feasible improvement might be to track who has
received and only free when no recipients exist, but that requires
more code to support another list.

An unformatted node is a suspected recipient iff a direct item is
converted to the indirect item that points to this unformatted node.

A mod_time_count is a 32 bit timestamp that counts in seconds, plus a
32 bit count of item modifications and preserve list freeings since
the beginning of the second.  In the unlikely event that the count
reaches UL_MAX_INT, we spin until the second ends.

If we wanted to prevent indirect items from pointing to unformatted
nodes which never reached disk before a crash, we would preserve
indirect items and their unformatted nodes.  We would use a
mod_time_count on the indirect items and on the freeing of the
preserve list, and we would store this mod_time_count for the preserve
list in the super block.  We would then throw away all indirect items
subsequent to the last moment of consistency (well, actually, we would
put them in the lost+found directory).  We won't do this prior to
implementing a cleaner.

Unfortunately, getting a free blocknr risks schedule, and many of our
operations are not able to handle schedule.  This creates much pain,
in that we must get all the blocknrs at the beginning of the operation
(do_balance, rename, etc.), and it motivates the existence of a
free_and_near list in the tb structure for all of the sub-procedures
of do_balance.  We also create a free_and_near list in other
procedures as well.  To solve this problem, ready_preserve_list
reserves enough blocknrs for an operation, and ensures that there is
space on the preserve list.

To support transactions in a much later release we will need to use
both write_next_to and restrict the freeing of all written_next_to
nodes affected by a transaction at least until all of its modified
nodes have reached the disk and the transaction is complete in its
non-disk dependencies (and the transaction does not return completion
until then as well.)  We will need to pass an optional transaction id
with every fs system call, and create a complete_transaction() system
call as well.  Note that the design intent will be to optimize
parallel transaction throughput not serialized latency.

In unlock_buffer() we cannot wait on a lock, but we can choose to skip
freeing the preserve list.  unlock_buffer() can interrupt
add_to_preserve() but add_to_preserve() cannot interrupt either
unlock_buffer or itself.  I hope that unlock_buffer cannot interrupt
itself, I must check it.  In add_to_preserve() we cannot wait on a
lock but we must add to the preserve list.  In reiserfs_new_blocknrs
we must wait on freeing the preserve list while syncing, which means
it can be interrupted by add_to_preserve which must not wait.
reserve_space_preserve can experience schedule.

These conditions determine our preserve list locking strategy. 
add_to_preserve, reserve_space_preserve, and reiserfs_new_blocknrs 
shall lock the preserve list.  reserve_space_preserve, and 
reiserfs_new_blocknrs shall release the preserve list before nonatomic 
kmalloc or syncing.  After unlocking the preserve list they will check 
the free_it flag, and if set they will free the list on behalf of 
unlock_buffers.  reserve_space_preserve will keep one free page always 
present on the free list, and assume that there are not enough 
parallel writes in progress to need more than that. 

add_to_preserve() shall add to either the preserve list or the jam
list and shall lock without waiting the one it uses.

If reiserfs_new_blocknrs() and add_to_preserve() lock both lists then 
unlock_buffers skips freeing the list.  reiserfs_new_blocknrs must 
unlock before syncing or otherwise risking schedule.  If either list 
is locked reserve_space_preserve will first get the space it needs and 
only then perform its action.  Since unlock_buffers is not interrupted 
by schedule it cannot cause reiserfs_new_blocknrs() or 
add_to_preserve() to fail to find at least one list to use. 
unlock_buffers will free any unlocked list.  unlock_buffers && 
reiserfs_new_blocknrs will lock and then free any unlocked list. 
 
While implementing this code we faced the following problem: 
 
Our recovery algorithm works by going through every block marked as 
used in the bitmap, finding every leaf node in an allocated block, and 
adding those nodes to a tree that it constructs from scratch.  It does 
this because we don't sync internal nodes to disk.  We don't sync 
internal nodes to disk because it would require writing more code for 
us to do it efficiently, and this current code needs to ship. 

The problem arises when the bitmap block reaches disk before the 
crash, but the change to the allocated block does not reach disk 
before the crash.  This can cause us to examine a garbage node for 
possible insertion into the tree.  Since the preserve list creates 
lots of garbage former internal nodes, we need to worry about how we 
will avoid adding a former internal node into the tree and clobbering 
the current version of that node. 
 
I had come up with this overly complex method involving timestamps, 
and a bitmap of released but not yet overwritten former members of the 
preserve list. 
 
Vladimir came up with a much simpler approach: we have two copies of 
the bitmap.  One of them represents the state of the bitmap at the 
last freeing of the preserve list: the preserved_bitmap.  The other is 
the current_bitmap, and the version of it which is on disk might not 
be accurate.  We can think of no combination of operations which will 
cause a stale internal node to be present in the preserved_bitmap 
using this algorithm.  The file system that will be recovered may 
reflect some deletions more recent than the freeing of the bitmap. 
This is okay.  All newly allocated nodes must be flagged 
Suspected_Recipients, or, using a new name for this flag, 
Write_Before_Freeing_Preserve.  This means that the preserve list will 
be freed much less often, which is bad but tolerable.  It means we 
ought to someday write code to ensure that it frees at least every 6 
minutes, which is annoying but tolerable. 
  
    -Hans */ 



/* Preserve List Code Overview (these comments should be merged into the ones above):

   We store preserved blocknrs on the preserve list (reiser_fs_sb.h ).

   preserve_shifted(), and preserve_invalidate
   place them on the list by calling add_to_preserve().

   We need to preserve buffers without incurring schedule during
   do_balance.  To allow preserve_shifted() to do that, before
   do_balance we insert a call to ready_preserve_list().
   ready_preserve_list() calls reserve_space_preserve() to grow the
   preserve list if it will be needed, and gets all of the new blocknrs that
   might be used by preserve_shifted during do_balance().

   free_all_preserve_members() frees the members of the preserve list
   when conditions indicate that it can be freed.

   maybe_free_preserve_list() checks if it can be freed, and calls
   free_all_preserve_members() if it can be.

   unpreserve() plus mark_buffer_dirty() accomplish the counting
   necessary to determine if the preserve list can be freed.

   */

#ifdef __KERNEL__

#include <linux/sched.h>
#include <linux/reiserfs_fs.h>
#include <linux/locks.h>

#else

#include "nokernel.h"

static void reiserfs_refile_buffer(struct buffer_head * bh) {}

#endif


#ifdef REISERFS_CHECK


#define SR_BITMAP(s) ((s)->u.reiserfs_sb.s_suspected_recipient_bitmap)
#define PB_BITMAP(s) ((s)->u.reiserfs_sb.s_preserved_block_bitmap)


void preserve_trace_init_bitmap (struct super_block * s)
{
  int i;

  SR_BITMAP(s) = reiserfs_kmalloc (sizeof (char *) * SB_BMAP_NR (s), GFP_KERNEL, s);
  PB_BITMAP(s) = reiserfs_kmalloc (sizeof (char *) * SB_BMAP_NR (s), GFP_KERNEL,s );

  if (SR_BITMAP(s) == 0 || PB_BITMAP(s) == 0)
    goto free_allocated;

  for (i = 0; i < SB_BMAP_NR (s); i ++) {
    SR_BITMAP(s)[i] = reiserfs_kmalloc (s->s_blocksize, GFP_KERNEL, s);
    PB_BITMAP(s)[i] = reiserfs_kmalloc (s->s_blocksize, GFP_KERNEL, s);

    if (SR_BITMAP(s)[i] == 0 || PB_BITMAP(s)[i] == 0)
      goto free_allocated;

    memset (SR_BITMAP(s)[i], 0, s->s_blocksize);
    memset (PB_BITMAP(s)[i], 0, s->s_blocksize);
  }
  return;

 free_allocated:
  if (SR_BITMAP(s)) {
    for (i = 0; i < SB_BMAP_NR (s); i ++)
      if (SR_BITMAP(s)[i])
	reiserfs_kfree (SR_BITMAP(s)[i], s->s_blocksize, s);
      else
	break;
  }
  if (PB_BITMAP(s)) {
    for (i = 0; i < SB_BMAP_NR (s); i ++)
      if (PB_BITMAP(s)[i])
	reiserfs_kfree (PB_BITMAP(s)[i], s->s_blocksize, s);
      else
	break;
  }
}


static void preserve_trace_print_bitmap (struct super_block * s, char ** bitmap, char * mes)
{
  int i, j;

  printk ("%s:", mes);
  for (i = 0; i < SB_BMAP_NR (s); i ++) {
    for (j = 0; j < s->s_blocksize * 8; j ++)
      if (test_bit (j, bitmap[i]))
	printk (" %lu", j + i * s->s_blocksize * 8);
  }
  printk ("\n");

}


void preserve_trace_release_bitmap (struct super_block * s)
{
  int i;

  if (s->u.reiserfs_sb.s_set_pb_bits || s->u.reiserfs_sb.s_set_sr_bits)
    reiserfs_warning ("vs-: preserve_trace_release_bitmap: preserved_blocks %d, suspected_recipients %d\n",
		      s->u.reiserfs_sb.s_set_pb_bits, s->u.reiserfs_sb.s_set_sr_bits);

  if (s->u.reiserfs_sb.s_set_sr_bits)
    preserve_trace_print_bitmap (s, SR_BITMAP (s), "suspected_recipients");

  if (s->u.reiserfs_sb.s_set_pb_bits)
    preserve_trace_print_bitmap (s, PB_BITMAP (s), "preserved_blocks");

  for (i = 0; i < SB_BMAP_NR (s); i ++) {
    reiserfs_kfree (SR_BITMAP (s)[i], s->s_blocksize, s);
    reiserfs_kfree (PB_BITMAP (s)[i], s->s_blocksize, s);
  }
  reiserfs_kfree (SR_BITMAP (s), sizeof (char **) * SB_BMAP_NR (s), s);
  reiserfs_kfree (PB_BITMAP (s), sizeof (char **) * SB_BMAP_NR (s), s);
}


static void preserve_trace_add_to_preserve (struct super_block * s, unsigned long block)
{
  int i, j;

  if (block >= SB_BLOCK_COUNT (s))
    reiserfs_panic (s, "vs-3004: preserve_trace_add_to_preserve: block %lu", block);

  i = block / (s->s_blocksize * 8);
  j = block % (s->s_blocksize * 8);
  if (test_bit (j, PB_BITMAP (s)[i]))
    reiserfs_panic (s, "vs-3005: preserve_trace_add_to_preserve: block %lu is preserved already", block);
  set_bit (j, PB_BITMAP (s)[i]);
  s->u.reiserfs_sb.s_set_pb_bits ++;
}


static void preserve_trace_mark_suspected_recipient (struct super_block * s, struct buffer_head * bh)
{
  int i, j;

  if (s->u.reiserfs_sb.s_suspected_recipient_count != s->u.reiserfs_sb.s_set_sr_bits)
    reiserfs_panic (s, "vs-3007: preserve_trace_mark_suspected_recipient: suspected recipients are %d or %d",
		    s->u.reiserfs_sb.s_suspected_recipient_count, s->u.reiserfs_sb.s_set_sr_bits);
  if (bh->b_blocknr >= SB_BLOCK_COUNT (s))
    reiserfs_panic (s, "vs-3008: preserve_trace_mark_suspected_recipient: block %lu", bh->b_blocknr);
  i = bh->b_blocknr / (s->s_blocksize * 8);
  j = bh->b_blocknr % (s->s_blocksize * 8);

  if (test_bit (j, SR_BITMAP (s)[i]))
    reiserfs_panic (s, "vs-3010: preserve_trace_mark_suspected_recipient: block %lu is suspected recipient", bh->b_blocknr);

  set_bit (j, SR_BITMAP (s)[i]);
  s->u.reiserfs_sb.s_set_sr_bits ++;
}


static void preserve_trace_unmark_suspected_recipient (struct super_block * s, struct buffer_head * bh)
{
  int i, j;

  if (s->u.reiserfs_sb.s_suspected_recipient_count != s->u.reiserfs_sb.s_set_sr_bits)
    reiserfs_panic (s, "vs-3012: preserve_trace_unmark_suspected_recipient: suspected recipients are %d or %d",
		    s->u.reiserfs_sb.s_suspected_recipient_count, s->u.reiserfs_sb.s_set_sr_bits);
  i = bh->b_blocknr / (s->s_blocksize * 8);
  j = bh->b_blocknr % (s->s_blocksize * 8);
  if (!test_bit (j, SR_BITMAP (s)[i]))
    reiserfs_panic (s, "vs-3015: preserve_trace_unmark_suspected_recipient: block %lu is not suspected recipient", bh->b_blocknr);
  clear_bit (j, SR_BITMAP (s)[i]);
  s->u.reiserfs_sb.s_set_sr_bits --;
  if (s->u.reiserfs_sb.s_set_sr_bits < 0)
    reiserfs_panic (s, "vs-3017: preserve_trace_unmark_suspected_recipient: suspected recipients counter < 0");
}


static void preserve_trace_free_preserved_block (struct super_block * s, unsigned long block)
{
  int i, j;

  i = block / (s->s_blocksize * 8);
  j = block % (s->s_blocksize * 8);
  if (!test_bit (j, PB_BITMAP (s)[i]))
    reiserfs_panic (s, "vs-3020: preserve_trace_free_preserved_block: block %lu is not preserved", block);
  clear_bit (j, PB_BITMAP (s)[i]);
  s->u.reiserfs_sb.s_set_pb_bits --;
  if (s->u.reiserfs_sb.s_set_pb_bits < 0)
    reiserfs_panic (s, "vs-3025: preserve_trace_free_preserved_block: preserved_blocks counter < 0");
}


static void preserve_trace_free_preserve_list (struct super_block * s)
{
  if (s->u.reiserfs_sb.s_set_sr_bits != 0) {
    preserve_trace_print_bitmap (s, SR_BITMAP (s), "suspected recipients");
    reiserfs_panic (0, "vs-3030: preserve_trace_free_preserve_list: preserved_blocks %d, suspected recipients %d, super block suspected recipient count %d",
		    s->u.reiserfs_sb.s_set_pb_bits, s->u.reiserfs_sb.s_set_sr_bits,
		    s->u.reiserfs_sb.s_suspected_recipient_count);
  }
}


void preserve_trace_reset_suspected_recipients (struct super_block * s)
{
  int i, j;

  for (i = 0; i < SB_BMAP_NR (s); i ++) {
    for (j = 0; j < s->s_blocksize * 8; j ++)
      if (test_bit (j, SR_BITMAP (s)[i])) {
	clear_bit (j, SR_BITMAP (s)[i]);
	s->u.reiserfs_sb.s_set_sr_bits --;
      }
  }
  if (s->u.reiserfs_sb.s_set_sr_bits)
    printk ("vs-3032: reset_suspected_recipients: %d buffers are marked for write\n",
	    s->u.reiserfs_sb.s_set_sr_bits);
}


void preserve_trace_print_srs (struct super_block * s)
{
  preserve_trace_print_bitmap (s, SR_BITMAP (s), "suspected_recipients");  
}


#endif /* REISERFS_CHECK */


static char * preserve_list_getmem (int size, int * repeat, struct super_block * s)
{
  char * buf;

  *repeat = SCHEDULE_OCCURRED;

  buf = reiserfs_kmalloc (size, GFP_KERNEL, s);
  if (buf == NULL)
    /* preserve list code is not ready to get 0 from kmalloc */
    reiserfs_panic (0, "vs-3035: preserve_list_getmem: kmalloc returned 0");
  return buf;
}


#define PL_PAGE_SIZE 4096
#define PL_NEXT_PAGE(p) ((void **)(p + PL_PAGE_SIZE - sizeof (void *)))

static int is_there_preserve_list (struct super_block * sb)
{
  return (SB_PL_FIRST_PAGE (sb) != 0);
}


/* no preserve list or no preserved blocks */
static int is_preserve_list_empty (struct super_block * sb)
{
  return (SB_PL_FIRST_PAGE (sb) == 0 || (unsigned long *)SB_PL_FIRST_PAGE (sb) == SB_PL_END (sb));
}


/* Ensure that the preserve list will have enough slots for a
   subsequent balancing operation.  It is possible for expanding the
   preserve list to cause schedule, and so this ensurance must be done
   before do_balance. Because I don't want to get a buffer for every
   do_balance and put it in tb on the small chance it might be used, I
   simply ensure that a full free buffer of unused slots exists.  The
   conditions under which this would not be enough are unlikely to
   exist for today's linux machines, and the more one ponders what it
   would take to consume a whole buffer before any process adds
   another buffer to the list the more distant the possibility of a
   problem seems.  Used in ready_preserve_list(). */
static int reserve_space_preserve (struct super_block * sb)
{
  int repeat = CARRY_ON, repeat2 = CARRY_ON;
  void * vp;

  /* Here we handle when the preserve list is empty, which only occurs once after mounting. */
  if (!is_there_preserve_list (sb)) {
#ifdef REISERFS_CHECK
    if (SB_PL_CURRENT_PAGE (sb))
      reiserfs_panic (sb, "reiser-3040: reserve_space_preserve: end but not start was nulled");
#endif /* REISERFS_CHECK */

    vp = preserve_list_getmem (PL_PAGE_SIZE, &repeat, sb);
    if (is_preserve_list_empty (sb)) {
      SB_PL_FIRST_PAGE (sb) = SB_PL_CURRENT_PAGE (sb) = vp;
      memset (SB_PL_FIRST_PAGE (sb), '\0', PL_PAGE_SIZE);
      /* set first entry */
      SB_PL_END (sb) = (unsigned long *)SB_PL_FIRST_PAGE (sb);
    } else
      /* somebody else has created preserve list already */
      reiserfs_kfree (vp, PL_PAGE_SIZE, sb);
  }

  /* if the current preserve list page is the last one in the preserve list, allocate a new one */
  if (*PL_NEXT_PAGE (SB_PL_CURRENT_PAGE (sb)) == 0) {
    /* preserve list always has empty memory block.*/
    vp = preserve_list_getmem (PL_PAGE_SIZE, &repeat2, sb);
    if (*PL_NEXT_PAGE (SB_PL_CURRENT_PAGE (sb)) == 0) {
      *PL_NEXT_PAGE (SB_PL_CURRENT_PAGE (sb)) = vp;
      memset (vp, '\0', PL_PAGE_SIZE);
    } else
      /* somebody else has expanded preserve list already */
      reiserfs_kfree (vp, PL_PAGE_SIZE, sb);
  }

  return (repeat | repeat2);
}


static int get_nodes_for_preserving (struct reiserfs_transaction_handle *th, struct tree_balance * tb, struct buffer_head * bh)
{
  struct buffer_head * new_bh;
  unsigned long	blocknrs[MAX_PRESERVE_NODES] = {0,};
  int i;
  int repeat1, repeat;

  if (tb->tb_nodes_for_preserving[0] != 0)
    return CARRY_ON;

  if ((repeat = reiserfs_new_blocknrs (th, tb->tb_sb, blocknrs, bh->b_blocknr, MAX_PRESERVE_NODES, 1/*for preserve list*/)) == NO_DISK_SPACE)
    return repeat; /* there is no free blocks */

  /* for each blocknumber we just got, get a buffer and stick it on tb array of nodes to be used in
     preserving */
  for (i = 0; i < MAX_PRESERVE_NODES; i ++) {

#ifdef REISERFS_CHECK
    if ( ! blocknrs[i] )
      reiserfs_panic (tb->tb_sb, "vs-3055: get_nodes_for_preserving: "
		      "reiserfs_new_blocknrs failed when got new blocks");
#endif

    repeat1 = CARRY_ON;
    new_bh = reiserfs_getblk (bh->b_dev, blocknrs[i], bh->b_size, &repeat1);
    repeat |= repeat1;
    if (new_bh->b_count > 1) {
      repeat |= SCHEDULE_OCCURRED;
      free_buffers_in_tb (tb);
      wait_buffer_until_released (new_bh);
    }
#ifdef REISERFS_CHECK
    if ( new_bh->b_count != 1 || buffer_dirty (new_bh)) {
      reiserfs_panic(tb->tb_sb,"vs-3060: get_nodes_for_preserving: not free or dirty buffer %b for the new block",
		     new_bh);
  }
#endif

    mark_buffer_journal_new(new_bh) ;
    /* Put empty buffers into the array. */
    tb->tb_nodes_for_preserving[i] = new_bh;
    
    /* we have atomically dirtied bitmap block, containing bit, that
       corresponds to p_s_new_bh->b_blocknr. Tree balance contains 1
       bit per each bitmap block. Set there bit corresponding to
       dirtied bitmap */
    set_bit (new_bh->b_blocknr / (new_bh->b_size * 8), DIRTY_BITMAP_MAP (tb));
  }

  return repeat;
}


/* expand preserve list if neccessary and get empty nodes to use in preserving */
int ready_preserve_list (struct tree_balance * tb, struct buffer_head * bh)
{
  int repeat, repeat1;

  if (dont_preserve (tb->tb_sb))
    return CARRY_ON;

  /* There is an operation (do_balance or reiserfs_rename) that needs to preserve block without
     risking schedule. Ensure that there is space on the preserve list for blocknrs of blocks that
     might be dirtied before the operation that must be protected from schedule ends. */
  repeat = reserve_space_preserve (tb->tb_sb);
  repeat1 = get_nodes_for_preserving (NULL, tb, bh);
  if (repeat1 == NO_DISK_SPACE)
    return NO_DISK_SPACE;
  return repeat | repeat1;
}


/* adds blocknr to preserve list so that shifted items are not overwritten before they are written.  assumes that
   pointer 'preserve_list_end' already points to the next available slot, and that there is another slot after that to
   increment to. */
void add_to_preserve (unsigned long blocknr, struct super_block * sb)
{
  if (dont_preserve (sb))
    return;
  
#ifdef REISERFS_CHECK
  if (SB_PL_CURRENT_PAGE (sb) == 0)
    reiserfs_panic (sb, "reiser-3065: add_to_preserve: current page is NULL");

  if ((char *)SB_PL_END (sb) > SB_PL_CURRENT_PAGE (sb) + PL_PAGE_SIZE - sizeof (void *) - sizeof (unsigned long))
    reiserfs_panic (sb, "reiser-3070: add_to_preserve: ran past the end of the current page somehow\n"); 

  if (*SB_PL_END (sb))
    reiserfs_panic (sb, "reiser-3075: add_to_preserve: "
		    "slot points to non-empty (does not contain 0) location, pointer run amuck\n");
  
  preserve_trace_add_to_preserve (sb, blocknr);

#endif /* REISERFS_CHECK */

  *SB_PL_END (sb) = blocknr;
  sb->u.reiserfs_sb.s_preserved ++;

  /* increment preserve_offset pointers appropriately */
  /* if not at end of page then increment to next slot else point offset to next page on jam.start */
  SB_PL_END (sb) ++;
  if (SB_PL_END (sb) + 1 > (unsigned long *)PL_NEXT_PAGE (SB_PL_CURRENT_PAGE (sb))) {
    SB_PL_CURRENT_PAGE (sb) = *PL_NEXT_PAGE (SB_PL_CURRENT_PAGE (sb));
    SB_PL_END (sb) = (unsigned long *)SB_PL_CURRENT_PAGE (sb);
  }
}


int is_buffer_unwritten (struct buffer_head * bh)
{
  return test_bit (BH_Unwritten, &bh->b_state);
}


int is_buffer_preserved (struct buffer_head * bh)
{
  return test_bit (BH_Preserved, &bh->b_state);
}


int is_buffer_suspected_recipient (struct super_block * s, struct buffer_head * bh)
{
  if (s) {
#ifdef REISERFS_CHECK
    int i, j;
    
    i = bh->b_blocknr / (s->s_blocksize * 8);
    j = bh->b_blocknr  % (s->s_blocksize * 8);
    if ((test_bit (BH_Suspected_Recipient, &bh->b_state) && !test_bit (j, SR_BITMAP (s)[i])) ||
	(!test_bit (BH_Suspected_Recipient, &bh->b_state) && test_bit (j, SR_BITMAP (s)[i]))) {
      reiserfs_warning ("vs-3077: is_suspected_recipient: block %d, state %d, sr_bitmap %d\n",
			bh->b_blocknr, test_bit (BH_Suspected_Recipient, &bh->b_state),
			test_bit (j, SR_BITMAP (s)[i]));
    }
    ;
#endif
  }
  return test_bit (BH_Suspected_Recipient, &bh->b_state);
}





extern int g_balances_number;
extern struct tree_balance init_tb;
extern int init_item_pos;
extern int init_pos_in_item;
extern int init_mode;
extern struct tree_balance * cur_tb;



inline void mark_buffer_unwritten (struct buffer_head * bh)
{
  set_bit (BH_Unwritten, &bh->b_state);
}


static inline void mark_buffer_preserved (struct buffer_head * bh)
{
  set_bit (BH_Preserved, &bh->b_state);
}



/* This function should be called on every buffer into which anything
   is shifted.  It also is called on some buffers which are involved
   in direct_to_indirect conversions, indirect_to_direct, and marking
   of bitmap block, and rename. It should also be done for the
   objectid map perhaps, we are still discussing it.  Functionally, it
   is every buffer which must be written to disk before the preserve
   list can be freed.  */
inline void mark_suspected_recipient (struct super_block * s, struct buffer_head * bh)
{
  if (dont_preserve (s))
    return;

  if (bh && !is_buffer_suspected_recipient (s, bh)) {

#ifdef REISERFS_CHECK

    if (s->u.reiserfs_sb.s_suspected_recipient_count == MAX_UL_INT)
      reiserfs_panic (s, "reiser-3080: mark_suspected_recipient: suspected_recipient_count reached max, must be leaking");

    if (!buffer_dirty (bh))
      reiserfs_warning ("vs-3082: mark_suspected_recipient: buffer is clean (%lu, state %o)\n",
			bh->b_blocknr, bh->b_state);

    preserve_trace_mark_suspected_recipient (s, bh);

#endif /* REISERFS_CHECK */

    s->u.reiserfs_sb.s_suspected_recipient_count ++;

    set_bit (BH_Suspected_Recipient, &bh->b_state);
  }

}


inline void unmark_suspected_recipient (struct super_block * s, struct buffer_head * bh)
{
  if (dont_preserve (s))
    return;
  
  if (bh && is_buffer_suspected_recipient (s, bh)) {

#ifdef REISERFS_CHECK

    if (s->u.reiserfs_sb.s_suspected_recipient_count == 0)
      reiserfs_panic (s, "reiser-3085: unmark_suspected_recipient: suspected_recipient_count is 0");

    preserve_trace_unmark_suspected_recipient (s, bh);

#endif /* REISERFS_CHECK */
    
    s->u.reiserfs_sb.s_suspected_recipient_count --;
    clear_bit (BH_Suspected_Recipient, &bh->b_state);
  }
}



/* For greater consistency in the event of a power loss we avoid writting nodes that contain
   meta-data in-place.  (We also preserve tails via reiserfs_invalidate_unfm.)  This could be
   changed to avoiding writing any nodes in place if transactions are implemented at some future
   time.  in do_balance() the parameter count = MAX_DIRTIABLE

   bh is the buffer whose blocknr should not be overwritten until there are no dirty blocks.

   free_and_near is a list of count blocknrs that are free for use and are near the neighbors of the
   old block.

   count is the length of the list free_and_near.

   this function is used by do_balance`s subprocedures and reiserfs_rename().

   Schedule may not be allowed (and is not allowed) in this function (because of both do_balance and
   the locking mechanism of the preserve list).  */
static struct buffer_head * get_node_for_preserving (struct tree_balance * tb, struct buffer_head * being_preserved)
{
  int i;
  struct buffer_head * bh;

  for (i = 0; i < MAX_PRESERVE_NODES; i ++)
    if (tb->tb_nodes_for_preserving[i]) {
      bh = tb->tb_nodes_for_preserving[i];
      tb->tb_nodes_for_preserving[i] = 0;

      tb->preserved[i] = being_preserved;
      if (bh->b_count != 1)
	reiserfs_panic (tb->tb_sb, "vs-3090: get_node_for_preserving: not free buffer on the list of empty nodes");
      return bh;
    }
  reiserfs_panic (tb->tb_sb, "vs-3095: get_node_for_preserving: no nodes for preserving");
  return 0;
}



void preserve_shifted (struct tree_balance * tb, struct buffer_head ** to_be_preserved, struct buffer_head * parent, int position,
		       struct buffer_head * dest)
{
  struct buffer_head * bh = *to_be_preserved;

  if (dont_preserve (tb->tb_sb))
    return;

  if (is_buffer_unwritten (bh) || is_buffer_preserved (bh)) {
    /* nothing to preserve: new node which has never been written on disk or buffer is preserved
       already */
    if (is_buffer_suspected_recipient (tb->tb_sb, bh) || is_buffer_preserved (bh)) {
      mark_suspected_recipient (tb->tb_sb, dest);
    }

    return;
  }

  *to_be_preserved = get_node_for_preserving (tb, bh);

  memcpy ((*to_be_preserved)->b_data, bh->b_data, bh->b_size);
  (*to_be_preserved)->b_state = bh->b_state;

  /* if bh was suspected recipient, then new buffer is suspected recipient also. Few line below
     reiserfs_invalidate_buffer will unmark_suspected_recipient (bh). To keep counter of suspected
     recipients in correct state we increase it */
  if (is_buffer_suspected_recipient (tb->tb_sb, bh)) {

#ifdef REISERFS_CHECK
    preserve_trace_mark_suspected_recipient (tb->tb_sb, (*to_be_preserved));
#endif
    tb->tb_sb->u.reiserfs_sb.s_suspected_recipient_count ++;
  }

  /* buffer (bh) is being shifted from for the first time since the last write */
  mark_buffer_preserved (*to_be_preserved);
  mark_suspected_recipient (tb->tb_sb, dest);

#ifdef REISERFS_CHECK
  /* make sure, that parent is correct */
  if (parent && B_N_CHILD_NUM (parent, position) != bh->b_blocknr) {
    print_block (parent, 0, position, position + 2);
    reiserfs_panic (tb->tb_sb, "reiser-3100: preserve_shifted: parent buffer (%b) "
		    "contains incorrect child pointer %y to %b in position %d",
		    parent, B_N_CHILD (parent, position), position, bh);
  }
#endif /* REISERFS_CHECK */

  add_to_preserve (bh->b_blocknr, tb->tb_sb);
  reiserfs_invalidate_buffer (NULL, tb, bh, 0/* do not free block */);
/*  brelse (bh);*/
  
  if (parent == 0) {
    if (bh->b_blocknr != SB_ROOT_BLOCK (tb->tb_sb))
      reiserfs_panic (tb->tb_sb, "reiser-3105: preserve_shifted: block must be root");
    SB_ROOT_BLOCK (tb->tb_sb) = (*to_be_preserved)->b_blocknr;
    
  } else
    B_N_CHILD_NUM (parent, position) = (*to_be_preserved)->b_blocknr;

}


/* When deleting files with a full disk I needed to preserve the
   buffer without asking for a free block.  This does that, unlike
   what preserve_shifted would do, and then it invalidates the buffer, thus
   avoiding the necessity of getting a new blocknr for the buffer. */
void preserve_invalidate (struct reiserfs_transaction_handle *th, 
                          struct tree_balance * tb, struct buffer_head * bh, struct buffer_head * dest)
{
  int do_free_block = 1;

  if (! dont_preserve (tb->tb_sb)) {
    if (is_buffer_preserved (bh)) {
      mark_suspected_recipient (tb->tb_sb, dest);
    } else if (is_buffer_unwritten (bh)) {
      if (is_buffer_suspected_recipient (tb->tb_sb, bh)) {
	mark_suspected_recipient (tb->tb_sb, dest);
      }
    } else {
      /* buffer is not preserved and not unwritten. put block number to the preserve list and,
         therefore, do not free block */
      mark_buffer_preserved (bh);
      add_to_preserve (bh->b_blocknr, tb->tb_sb);
      mark_suspected_recipient (tb->tb_sb, dest);
      do_free_block = 0;
    }
  }

  reiserfs_invalidate_buffer (th, tb,bh, do_free_block);
}


static void save_and_reset_preserve_list (struct super_block * sb, char ** first_page)
{
  *first_page = SB_PL_FIRST_PAGE (sb);
  SB_PL_FIRST_PAGE (sb) = 0;
  SB_PL_CURRENT_PAGE (sb) = 0;
  SB_PL_END (sb) = 0;
  /* now any processes that come along will see an empty preserve list that they will start
     appending to using add_to_preserve() without affecting us. -Hans */

}


static void free_preserve_list (struct reiserfs_transaction_handle *th, struct super_block * sb, char * freeing_page)
{
  char * tmp;
  unsigned long * freed_so_far;
  int freed = 0;

  sb->u.reiserfs_sb.s_preserve_list_freeings ++;

  /* inner loop moves from slot to slot freeing blocks, outer loop moves from page to page */
  /* this is one place where we could benefit from a reiserfs_free_block() that took a list as its
     arg... */
  for (;;) {
    freed_so_far = (unsigned long *)freeing_page;
    do {
      if (*freed_so_far) {
	reiserfs_free_block(th, sb, *freed_so_far);
	reiserfs_refile_buffer (SB_AP_BITMAP (sb)[*freed_so_far/(sb->s_blocksize * 8)]);
	/* reiserfs_refile_buffer (SB_AP_CAUTIOUS_BITMAP (sb)[*freed_so_far/(sb->s_blocksize * 8)]); journal victim */
#ifdef REISERFS_CHECK
	preserve_trace_free_preserved_block (sb, *freed_so_far);
#endif
	if (sb->u.reiserfs_sb.s_preserved == 0) {
	  printk ("vs-3107: free_preserve_list: preserved block counter is 0\n");
	  sb->u.reiserfs_sb.s_preserved = 1;
	}
	sb->u.reiserfs_sb.s_preserved --;
	freed ++;
      } else {
	/* Null slot, the first null slot should be the end of the preserve list. */
	if (*PL_NEXT_PAGE (freeing_page)) {
	  /* free next memory block if we have it */
	  reiserfs_kfree (*PL_NEXT_PAGE (freeing_page), PL_PAGE_SIZE, sb);
	}
	reiserfs_kfree (freeing_page, PL_PAGE_SIZE, sb);
	return;
      }
      freed_so_far ++;
      /* While not at end of page, and while testing condition
	 we increment the slot pointer. */
    } while ((freed_so_far + 1) <= (unsigned long *)PL_NEXT_PAGE (freeing_page));
	
    /* We reached the end of the page, so now we go to the next page, and free the current one. */
    tmp = freeing_page;
    freeing_page = *PL_NEXT_PAGE (freeing_page);

#ifdef REISERFS_CHECK
    if (!freeing_page)
      reiserfs_panic(sb, "reiser-3110: free_preserve_list: "
		     "preserve list contained unexpected null page prior to end of preserve list");
#endif
    reiserfs_kfree (tmp, PL_PAGE_SIZE, sb);
  }

  /* this is next operator after for (;;) */
  reiserfs_panic (sb, "reiser-3115: free_preserve_list: no next page on preserve list");
  return;
}


/* check to see if we can free the members of the preserve list.

   All sys calls which can dirty a block, before the block is dirtied, should check this:

   These calls are:
   reiserfs_object_write 
   create 
   link 
   unlink 
   symlink 
   mkdir 
   rmdir 
   mknod 
   rename

   It is also called when reiserfs_new_blocknr() runs out of disk space.

   */
/* if there are no dirty buffers that might contain shiftable objects && there is anything on the
   free list, and the list is not locked then free it.  Note that there is a presumption that if the
   list is locked it doesn't much matter, there will be plenty of other opportunities to free the
   list, go onwards without waiting for it to unlock.. */
int maybe_free_preserve_list (struct super_block * s)
{
  char * first_page;

  if (dont_preserve (s))
    return 1;

  if (s->u.reiserfs_sb.s_suspected_recipient_count != 0) {
    /* preserve list can not be freed */
    return 0;
  }

#ifdef REISERFS_CHECK
  preserve_trace_free_preserve_list (s);
#endif

  if (is_there_preserve_list (s)) {
    /* preserve list can contain 0 preserved blocks */
    save_and_reset_preserve_list (s, &first_page);
    free_preserve_list (NULL, s, first_page);
    return 1;
  }

  /* preserve list is empty */
  return 1;
}


/* We are out of disk space, so if there are blocks on the preserve list then free them by using
   sync, and return that schedule() occurred (1) so that fix_nodes can repeat, otherwise return that
   we are truly out of disk space (NO_DISK_SPACE) */
int get_space_from_preserve_list (struct super_block * s)
{
  static struct wait_queue * wait = NULL;
  static int lock = 0;
  char * first_page;

  if (dont_preserve (s))
    return NO_DISK_SPACE;

  if (lock) {
    sleep_on (&wait);
    return SCHEDULE_OCCURRED;
  }

  if (is_preserve_list_empty (s))
    return NO_DISK_SPACE;

  lock = 1;

  /* we are going to free all blocks saved on preserve list, disconnect preserve list from super
     block */
  save_and_reset_preserve_list (s, &first_page);

  if (s->u.reiserfs_sb.s_suspected_recipient_count != 0) {
    fsync_dev (s->s_dev);
    /* do not worry about suspected recipient count anymore, fsync_dev may not return until all
       buffer writes have completed. */
  }

  free_preserve_list (NULL, s, first_page);
  lock = 0;
  wake_up(&wait);
  
  return SCHEDULE_OCCURRED;
}


/* this is called on io completion */
void unpreserve (struct super_block * s, struct buffer_head * bh)
{
  int nr, offset;


  unmark_suspected_recipient (s, bh);

  /* This does no harm to non-reiserfs buffers. */
  clear_bit(BH_Preserved,&bh->b_state);	/* this is set when the buffer is preserved */
  clear_bit(BH_Unwritten,&bh->b_state);	/* this is set when the buffer is get into tree
					   via get_FEB */

  /* set bit in the cautious bitmap if it was not set */
  nr = bh->b_blocknr / (s->s_blocksize << 3);
  offset = bh->b_blocknr % (s->s_blocksize << 3);
#if 0 /* journal victim */
  if (!test_bit (offset, SB_AP_CAUTIOUS_BITMAP (s)[nr]->b_data)) {
    set_bit (offset, SB_AP_CAUTIOUS_BITMAP (s)[nr]->b_data);
    reiserfs_mark_buffer_dirty (SB_AP_CAUTIOUS_BITMAP (s)[nr], 1); /* journal victim */
    reiserfs_refile_buffer (SB_AP_CAUTIOUS_BITMAP (s)[nr]);

    mark_suspected_recipient (s, SB_AP_CAUTIOUS_BITMAP (s)[nr]);
  }
#endif

  if (!s->u.reiserfs_sb.lock_preserve)
    /* no tree updates are in progress */
    maybe_free_preserve_list (s);
}








