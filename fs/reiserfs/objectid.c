/*
 * Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */
#ifdef __KERNEL__

#include <linux/string.h>
#include <linux/locks.h>
#include <linux/sched.h>
#include <linux/reiserfs_fs.h>

#else

#include "nokernel.h"

#endif


/* When we allocate objectids we allocate the first unused objectid.
   Each sequence of objectids in use (the odd sequences) is followed
   by a sequence of objectids not in use (the even sequences).  We
   only need to record the last objectid in each of these sequences
   (both the odd and even sequences) in order to fully define the
   boundaries of the sequences.  A consequence of allocating the first
   objectid not in use is that under most conditions this scheme is
   extremely compact.  The exception is immediately after a sequence
   of operations which deletes a large number of objects of
   non-sequential objectids, and even then it will become compact
   again as soon as more objects are created.  Note that many
   interesting optimizations of layout could result from complicating
   objectid assignment, but we have deferred making them for now. */


/* get unique object identifier */
unsigned long	reiserfs_get_unused_objectid (struct reiserfs_transaction_handle *th, struct super_block * s)
{
  unsigned long unused_objectid;
  struct reiserfs_super_block * disk_sb;
  unsigned long * objectid_map;


  disk_sb = SB_DISK_SUPER_BLOCK (s);
  objectid_map = (unsigned long *)(disk_sb + 1); /* The objectid map follows the superblock. */
  
                                /* comment needed -Hans */
  unused_objectid = objectid_map[1];
  if (unused_objectid == TYPE_INDIRECT) {
    printk ("REISERFS: get_objectid: no more object ids\n");
    return 0;
  }

  /* This incrementation allocates the first unused objectid. That is to say, the first entry on the
   objectid map is the first unused objectid, and by incrementing it we use it.  See below where we
   check to see if we eliminated a sequence of unused objectids.... */
  objectid_map[1] ++;

  /* Now we check to see if we eliminated the last remaining member of
     the first even sequence (and can eliminate the sequence by
     eliminating its last objectid from oids), and can collapse the
     first two odd sequences into one sequence.  If so, then the net
     result is to eliminate a pair of objectids from oids.  We do this
     by shifting the entire map to the left. */
  if (disk_sb->s_oid_cursize > 2 && objectid_map[1] == objectid_map[2]) {
    memmove (objectid_map + 1, objectid_map + 3, (disk_sb->s_oid_cursize - 3) * sizeof(unsigned long));
    disk_sb->s_oid_cursize -= 2;
  }

  /* super block has been changed. Non-atomic mark_buffer_dirty is allowed here */
  /* mark_buffer_dirty (SB_BUFFER_WITH_SB (s), 1); journal victim *//* no need to place buffer on preserve list */
  journal_mark_dirty(th, s, SB_BUFFER_WITH_SB (s));/* no need to place buffer on preserve list */
  s->s_dirt = 1;
  return unused_objectid;
}


/* makes object identifier unused */
void	reiserfs_release_objectid (struct reiserfs_transaction_handle *th, 
				   unsigned long objectid_to_release, struct super_block * s)
{
  struct reiserfs_super_block * disk_sb;
  unsigned long * objectid_map;
  int i = 0;


  /* return; */

  /* mark_buffer_dirty (SB_BUFFER_WITH_SB (s), 1);  journal victim */
  journal_mark_dirty(th, s, SB_BUFFER_WITH_SB (s)); 
  s->s_dirt = 1;

  /* let disk_sb serve as a convenient shorthand pointing to the Reiserfs
     Specific portion of the super_block */
  disk_sb = SB_DISK_SUPER_BLOCK (s);

  /* This means/assumes that the objectid map immediately follows
     the superblock in memory. */
  objectid_map = (unsigned long *)(disk_sb + 1);

  /* start at the beginning of the objectid map (i = 0) and go to the
     end of it (i = disk_sb->s_oid_cursize).  Linear search is what we use,
     though it is possible that binary search would be more efficient
     after performing lots of deletions (which is when oids is large.)
     We only check even i's. */
  while (i < disk_sb->s_oid_cursize) {
    if (objectid_to_release == objectid_map[i]) {
      if (i == 0)
	reiserfs_panic (s, "vs-15000: reiserfs_release_objectid: trying to free root object id (%lu)",
			    objectid_to_release);
      /* This incrementation unallocates the objectid. */
      objectid_map[i]++;
      /* Did we unallocate the last member of an odd sequence, and can shrink oids? */
      if (objectid_map[i] == objectid_map[i+1]) {
	/* shrink objectid map */
	memmove (objectid_map + i, objectid_map + i + 2, 
		 (disk_sb->s_oid_cursize - i - 2)*sizeof(unsigned long));
	disk_sb->s_oid_cursize -= 2;
#ifdef REISERFS_CHECK
	if (disk_sb->s_oid_cursize < 2 || disk_sb->s_oid_cursize > disk_sb->s_oid_maxsize)
	  reiserfs_panic (s, "vs-15005: reiserfs_release_objectid: "
			  "objectid map corrupted cur_size == %d (max == %d)",
			  disk_sb->s_oid_cursize, disk_sb->s_oid_maxsize);
#endif
      }
      return;
    }

    if (objectid_to_release > objectid_map[i] && objectid_to_release < objectid_map[i+1]) {
      /* size of objectid map is not changed */
      if (objectid_to_release + 1 == objectid_map[i+1]) {
	objectid_map[i+1]--;
	return;
      }

      if (disk_sb->s_oid_cursize == disk_sb->s_oid_maxsize)
	/* objectid map must be expanded, but there is no space */
	return;

      /* expand the objectid map*/
      memmove (objectid_map+i+3, objectid_map+i+1, (disk_sb->s_oid_cursize-i-1) * sizeof(unsigned long));
      objectid_map[i+1] = objectid_to_release;
      objectid_map[i+2] = objectid_to_release + 1;
      disk_sb->s_oid_cursize += 2;
      return;
    }
    i += 2;
  }

  reiserfs_panic (0, "vs-15010: reiserfs_release_objectid: trying to free free object id (%lu)", objectid_to_release);
}



