/*
 * Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */
#ifndef _LINUX_REISER_FS_H
#define _LINUX_REISER_FS_H


#include <linux/types.h>
#ifdef __KERNEL__
#include <linux/malloc.h>
#include <linux/tqueue.h>
#endif

/*
 *  include/linux/reiser_fs.h
 *
 *  Reiser File System constants and structures
 *
 */

/* in reading the #defines, it may help to understand that they employ
 the following abbreviations:

B = Buffer
I = Item header
H = Height within the tree (should be changed to LEV)
N = Number of the item in the node
DNM = DyNaMic data
STAT = stat data
DEH = Directory Entry Header
EC = Entry Count
E = Entry number
UL = Unsigned Long
BLKH = BLocK Header
UNFM = UNForMatted node
DC = Disk Child
P = Path

These #defines are named by concatenating these abbreviations, where
first comes the arguments, and last comes the return value, of the
macro.

*/

#define REISERFS_CHECK

#ifdef __arm
/* ARM can't load ints or u32s from unaligned addresses (it silently gives the
 * wrong answer). So we must u32-align all structures in directory blocks :-(
 */
#define REISERFS_ALIGNED
#endif

/* NEW_GET_NEW_BUFFER will try to allocate new blocks better */
/*#define NEW_GET_NEW_BUFFER*/
#define OLD_GET_NEW_BUFFER

/* if this is undefined, all inode changes get into stat data immediately, if it can be found in RAM */
#define DIRTY_LATER


/* these are used by reiserfs_file_read when no genericread mount option specified */
#define REISERFS_NBUF 32

#define REISERFS_OBJECT_READ_AHEAD

#define PACKING_LOCALITY_READ_AHEAD

/* Should be used for single disk file systems. */
/* #define READ_LOCK_REISERFS */



/* obsolete defines */
/*#define REISERFS_CHECK_ONE_PROCESS*/
/*#define REISERFS_INFO*/





/*
 * Disk Data Structures
 */

/***************************************************************************/
/*                             SUPER BLOCK                                 */
/***************************************************************************/

/*
 * Structure of super block on disk, a version of which in RAM is often accessed as s->u.reiserfs_sb.s_rs
 * the version in RAM is part of a larger structure containing fields never written to disk.
 */

				/* used by gcc */
#define REISERFS_SUPER_MAGIC 0x52654973
				/* used by file system utilities that
                                   look at the superblock, etc. */
#define REISERFS_SUPER_MAGIC_STRING "ReIsErFs"

				/* ReiserFS leaves the first 64k unused,
                                   so that partition labels have enough
                                   space.  If someone wants to write a
                                   fancy bootloader that needs more than
                                   64k, let us know, and this will be
                                   increased in size.  This number must
                                   be larger than than the largest block
                                   size on any platform, or code will
                                   break.  -Hans */
#define REISERFS_DISK_OFFSET_IN_BYTES (64 * 1024)
#define REISERFS_FIRST_BLOCK unused_define

/* the spot for the super in versions 3.5 - 3.5.11 (inclusive) */
#define REISERFS_OLD_DISK_OFFSET_IN_BYTES (8 * 1024)

#define READ_BLOCKS  1
#define DONT_READ_BLOCKS 2

#define CARRY_ON          	0
#define SCHEDULE_OCCURRED  	1
#define PATH_INCORRECT    	2
#define IO_ERROR		3

#define NO_DISK_SPACE        (-1)
#define NO_BALANCING_NEEDED  (-2)


struct buffer_and_id {
  struct buffer_head  * bi_buf;
  unsigned long  bi_id;
};

typedef unsigned long b_blocknr_t;
typedef __u32 unp_t;

struct unfm_nodeinfo {
  unsigned long	 unfm_nodenum;
  unsigned short unfm_freespace;
};

/* when reiserfs_file_write is called with a byte count >= MIN_PACK_ON_CLOSE,
** it sets the inode to pack on close, and when extending the file, will only
** use unformatted nodes.
**
** This is a big speed up for the journal, which is badly hurt by direct->indirect
** conversions (they must be logged).
*/
#define MIN_PACK_ON_CLOSE		512

/* the defines below say, that if file size is >=
   DIRECT_TAIL_SUPPRESSION_SIZE * blocksize, then if tail is longer
   than MAX_BYTES_SUPPRESS_DIRECT_TAIL, it will be stored in
   unformatted node */
#define DIRECT_TAIL_SUPPRESSION_SIZE      1024
#define MAX_BYTES_SUPPRESS_DIRECT_TAIL    1024

/* Check whether byte is placed in a direct item. */
#define INODE_OFFSET_IN_DIRECT(p_s_inode, n_offset) \
( (n_offset) >= (p_s_inode)->u.reiserfs_i.i_first_direct_byte )

/* this is used for i_first_direct_byte field of inode */
#define NO_BYTES_IN_DIRECT_ITEM MAX_KEY_OFFSET

/* We store tail in unformatted node if it is too big to fit into a
   formatted node or if DIRECT_TAIL_SUPPRESSION_SIZE,
   MAX_BYTES_SUPPRESS_DIRECT_TAIL and file size say that. */
/* #define STORE_TAIL_IN_UNFM(n_file_size,n_tail_size,n_block_size) \ */
/* ( ((n_tail_size) > MAX_DIRECT_ITEM_LEN(n_block_size)) || \ */
/*   ( ( (n_file_size) >= (n_block_size) * DIRECT_TAIL_SUPPRESSION_SIZE ) && \ */
/*    ( (n_tail_size) >= MAX_BYTES_SUPPRESS_DIRECT_TAIL ) ) ) */

  /* This is an aggressive tail suppression policy, I am hoping it
     improves our benchmarks. The principle behind it is that
     percentage space saving is what matters, not absolute space
     saving.  This is non-intuitive, but it helps to understand it if
     you consider that the cost to access 4 blocks is not much more
     than the cost to access 1 block, if you have to do a seek and
     rotate.  A tail risks a non-linear disk access that is
     significant as a percentage of total time cost for a 4 block file
     and saves an amount of space that is less significant as a
     percentage of space, or so goes the hypothesis.  -Hans */
#define STORE_TAIL_IN_UNFM(n_file_size,n_tail_size,n_block_size) \
\
( ((n_tail_size) > MAX_DIRECT_ITEM_LEN(n_block_size)) || \
  ( (n_file_size) >= (n_block_size) * 4 ) || \
   ( ( (n_file_size) >= (n_block_size) * 3 ) && \
   ( (n_tail_size) >=   (MAX_DIRECT_ITEM_LEN(n_block_size))/4) ) || \
   ( ( (n_file_size) >= (n_block_size) * 2 ) && \
   ( (n_tail_size) >=   (MAX_DIRECT_ITEM_LEN(n_block_size))/2) ) || \
   ( ( (n_file_size) >= (n_block_size) ) && \
   ( (n_tail_size) >=   (MAX_DIRECT_ITEM_LEN(n_block_size) * 3)/4) ) )


/*
 * values for s_state field
 */
#define REISERFS_VALID_FS    1
#define REISERFS_ERROR_FS    2



/***************************************************************************/
/*                       KEY & ITEM HEAD                                   */
/***************************************************************************/
typedef __u32 objectid_t;

/* Key of the object drop determines its location in the S+tree, and is composed of 4 components */
struct key {
  __u32 k_dir_id;   	    /* packing locality: by default parent directory object id */
  __u32 k_objectid;          /* object identifier */
  __u32 k_offset;	    /* for regular files this is the offset to the first byte of the body, */
	                            /* contained in the object-item, */
       		                    /* as measured from the start of the entire body of the object. */

				       /* for directory entries, k_offset consists of hash derived from
					  hashing the name and using few bits (23 or more) of the resulting
					  hash, and generation number that allows distinguishing names with
					  hash collisions. If number of collisions overflows generation number, we return EEXIST. 
					  High order bit is 0 always */
  __u32 k_uniqueness;	       /* uniqueness field used for storing flags about the item's type and
					  mergeability.  Key size is performance critical.  These flags should
					  not be stored here in the key, and the key could be reduced to three
					  components by pushing the uniqueness field into the offset for
					  directories.  Grrr. -Hans */
};

 /* Our function for comparing keys can compare keys of different
    lengths.  It takes as a parameter the length of the keys it is to
    compare.  These defines are used in determining what is to be
    passed to it as that parameter. */
#define REISERFS_FULL_KEY_LEN     4

#define REISERFS_SHORT_KEY_LEN    2

/* The result of the key compare */
#define FIRST_GREATER 1
#define SECOND_GREATER -1
#define KEYS_IDENTICAL 0
#define KEY_FOUND 1
#define KEY_NOT_FOUND 0


#define KEY_SIZE (sizeof(struct key))
#define SHORT_KEY_SIZE (sizeof (unsigned long) + sizeof (unsigned long))

/* return values for search_by_key and clones */
#define ITEM_FOUND 1
#define ITEM_NOT_FOUND 0
#define ENTRY_FOUND 1
#define ENTRY_NOT_FOUND 0
#define DIRECTORY_NOT_FOUND -1
#define REGULAR_FILE_FOUND -2
#define DIRECTORY_FOUND -3
#define BYTE_FOUND 1
#define BYTE_NOT_FOUND 0
#define FILE_NOT_FOUND -1

#define POSITION_FOUND 1
#define POSITION_NOT_FOUND 0
#define GOTO_PREVIOUS_ITEM 2
#define POSITION_FOUND_INVISIBLE 3


/*  Everything in the filesystem is stored as a set of items.  The item head contains the key of the item, its
   free space (for indirect items) and specifies the location of the item itself within the block.  */

struct item_head
{
  struct key ih_key; 	/* Everything in the tree is found by searching for it based on its key.*/

  union {
    __u16 ih_free_space; /* The free space in the last unformatted node of an indirect item if this
				     is an indirect item.  This equals 0xFFFF iff this is a direct item or
				     stat data item. Note that the key, not this field, is used to determine
				     the item type, and thus which field this union contains. */
    __u16 ih_entry_count; /* Iff this is a directory item, this field equals the number of directory
				      entries in the directory item. */
  } u;
  __u16 ih_item_len;           /* total size of the item body                  */
  __u16 ih_item_location;      /* an offset to the item body within the block  */
  __u16 ih_reserved;		/* used by reiserfsck */
};
/* size of item header     */
#define IH_SIZE (sizeof(struct item_head))


#define I_K_KEY_IN_ITEM(p_s_ih, p_s_key, n_blocksize) \
    ( ! COMP_SHORT_KEYS(p_s_ih, p_s_key) && \
          I_OFF_BYTE_IN_ITEM(p_s_ih, (p_s_key)->k_offset, n_blocksize) )

/* maximal length of item */ 
#define MAX_ITEM_LEN(block_size) (block_size - BLKH_SIZE - IH_SIZE)
#define MIN_ITEM_LEN 1


/* object identifier for root dir */
#define REISERFS_ROOT_OBJECTID 2
#define REISERFS_ROOT_PARENT_OBJECTID 1
extern struct key root_key;




/* 
 * Picture represents a leaf of the S+tree
 *  ______________________________________________________
 * |      |  Array of     |                   |           |
 * |Block |  Object-Item  |      F r e e      |  Objects- |
 * | head |  Headers      |     S p a c e     |   Items   |
 * |______|_______________|___________________|___________|
 */

/* Header of a disk block.  More precisely, header of a formatted leaf
   or internal node, and not the header of an unformatted node. */
struct block_head {       
  __u16 blk_level;        /* Level of a block in the tree. */
  __u16 blk_nr_item;      /* Number of keys/items in a block. */
  __u16 blk_free_space;   /* Block free space in bytes. */
  struct key  blk_right_delim_key; /* Right delimiting key for this block (supported for leaf level nodes
				      only) */
};

#define BLKH_SIZE (sizeof(struct block_head))

/*
 * values for blk_type field
 */

#define FREE_LEVEL        0 /* Node of this level is out of the tree. */

#define DISK_LEAF_NODE_LEVEL  1 /* Leaf node level.                       */

/* Given the buffer head of a formatted node, resolve to the block head of that node. */
#define B_BLK_HEAD(p_s_bh)  ((struct block_head *)((p_s_bh)->b_data))
/* Number of items that are in buffer. */
#define B_NR_ITEMS(p_s_bh)	  	( B_BLK_HEAD(p_s_bh)->blk_nr_item )
#define B_LEVEL(bh)			( B_BLK_HEAD(bh)->blk_level )
#define B_FREE_SPACE(bh)		( B_BLK_HEAD(bh)->blk_free_space )
/* Get right delimiting key. */
#define B_PRIGHT_DELIM_KEY(p_s_bh)	( &(B_BLK_HEAD(p_s_bh)->blk_right_delim_key) )

/* Does the buffer contain a disk leaf. */
#define B_IS_ITEMS_LEVEL(p_s_bh)   	( B_BLK_HEAD(p_s_bh)->blk_level == DISK_LEAF_NODE_LEVEL )

/* Does the buffer contain a disk internal node */
#define B_IS_KEYS_LEVEL(p_s_bh) 	( B_BLK_HEAD(p_s_bh)->blk_level > DISK_LEAF_NODE_LEVEL &&\
					  B_BLK_HEAD(p_s_bh)->blk_level <= MAX_HEIGHT )




/***************************************************************************/
/*                             STAT DATA                                   */
/***************************************************************************/

/* Stat Data on disk (reiserfs version of UFS disk inode minus the address blocks) */

/*
  The sense of adding union to stat data is to keep a value of real number of blocks used by file.
  The necessity of adding such information is caused by existing of files with holes.
  Reiserfs should keep number of used blocks for file, but not calculate it from file size
  (that is not correct for holed files). Thus we have to add additional information to stat data.
  When we have a device special file, there is no need to get number of used blocks for them,
  and, accordingly, we doesn't need to keep major and minor numbers for regular files, which
  might have holes. So this field is being overloaded.
*/

struct stat_data {
  __u16 sd_mode;	/* file type, permissions */
  __u16 sd_nlink;	/* number of hard links */
  __u16 sd_uid;		/* owner */
  __u16 sd_gid;		/* group */
  __u32 sd_size;	/* file size */
  __u32 sd_atime;	/* time of last access */
  __u32 sd_mtime;	/* time file was last modified  */
  __u32 sd_ctime;	/* time inode (stat data) was last changed (except changes to sd_atime and sd_mtime) */
  union {
	 __u32 sd_rdev;
	 __u32 sd_blocks;	/* number of blocks file uses */
  } u;
  __u32 sd_first_direct_byte; /* first byte of file which is stored in a direct item: except that if it equals 1 it is a
     symlink and if it equals MAX_KEY_OFFSET there is no direct item.  The existence of this
     field really grates on me. Let's replace it with a macro based on sd_size and our tail
     suppression policy.  Someday.  -Hans */
};
#define SD_SIZE (sizeof(struct stat_data))


/***************************************************************************/
/*                      DIRECTORY STRUCTURE                                */
/***************************************************************************/
/* 
   Picture represents the structure of directory items
   ________________________________________________
   |  Array of     |   |     |        |       |   |
   | directory     |N-1| N-2 | ....   |   1st |0th|
   | entry headers |   |     |        |       |   |
   |_______________|___|_____|________|_______|___|
                    <----   directory entries         ------>

 First directory item has k_offset component 1. We store "." and ".."
 in one item, always, we never split "." and ".." into differing
 items.  This makes, among other things, the code for removing
 directories simpler. */
#define SD_OFFSET  0
#define SD_UNIQUENESS 0
#define DOT_OFFSET 1
#define DOT_DOT_OFFSET 2
#define DIRENTRY_UNIQUENESS 500

/* */
#define FIRST_ITEM_OFFSET 1

/*
   Q: How to get key of object pointed to by entry from entry?  

   A: Each directory entry has its header. This header has deh_dir_id and deh_objectid fields, those are key
      of object, entry points to */

/* NOT IMPLEMENTED:   
   Directory will someday contain stat data of object */



struct reiserfs_de_head
{
  __u32 deh_offset;  /* third component of the directory entry key */
  __u32 deh_dir_id;  /* objectid of the parent directory of the
			object, that is referenced by directory entry */
  __u32 deh_objectid;/* objectid of the object, that is referenced by
                        directory entry */
  __u16 deh_location;/* offset of name in the whole item */
  __u16 deh_state;   /* whether 1) entry contains stat data (for
			future), and 2) whether entry is hidden
			(unlinked) */
};
#define DEH_SIZE sizeof(struct reiserfs_de_head)

#define deh_offset(deh) (__le32_to_cpu ((deh)->deh_offset))
#define deh_dir_id(deh) (__le32_to_cpu ((deh)->deh_dir_id))
#define deh_objectid(deh) (__le32_to_cpu ((deh)->deh_objectid))

#define DEH_Statdata 0			/* not used now */
#define DEH_Visible 2

#define mark_de_with_sd(deh)        set_bit (DEH_Statdata, &((deh)->deh_state))
#define mark_de_without_sd(deh)     clear_bit (DEH_Statdata, &((deh)->deh_state))
#define mark_de_visible(deh)	    set_bit (DEH_Visible, &((deh)->deh_state))
#define mark_de_hidden(deh)	    clear_bit (DEH_Visible, &((deh)->deh_state))

#define de_with_sd(deh)		    test_bit (DEH_Statdata, &((deh)->deh_state))
#define de_visible(deh)	    	    test_bit (DEH_Visible, &((deh)->deh_state))
#define de_hidden(deh)	    	    !test_bit (DEH_Visible, &((deh)->deh_state))

/* length of the directory entry in directory item. This define calculates length of i-th directory entry
   using directory entry locations from dir entry head. When it calculates length of 0-th directory entry, it
   uses length of whole item in place of entry location of the non-existent following entry in the
   calculation.  See picture above.*/
#define I_DEH_N_ENTRY_LENGTH(ih,deh,i) \
((i) ? (((deh)-1)->deh_location - (deh)->deh_location) : ((ih)->ih_item_len) - (deh)->deh_location)

/* empty directory contains two entries "." and ".." and their headers */
#ifdef REISERFS_ALIGNED
#define EMPTY_DIR_SIZE  (2 * DEH_SIZE + 4 + 4)
#else
#define EMPTY_DIR_SIZE  (2 * DEH_SIZE + /*sizeof (unsigned long) +*/ 3)
#endif

/* number of entries in the directory item, depends on ENTRY_COUNT being at the start of directory dynamic data. */
#define I_ENTRY_COUNT(ih) ((ih)->u.ih_entry_count)

/* array of the entry headers */
#define B_I_DEH(bh,ih) ((struct reiserfs_de_head *)(B_I_PITEM(bh,ih)))

/* name by bh, ih and entry_num */
#define B_I_E_NAME(entry_num,bh,ih) ((char *)(bh->b_data + ih->ih_item_location + (B_I_DEH(bh,ih)+(entry_num))->deh_location))

#define REISERFS_MAX_NAME_LEN(block_size) (block_size - BLKH_SIZE - IH_SIZE - DEH_SIZE)	/* -SD_SIZE when entry will contain stat data */

/* this structure is used for operations on directory entries. It is not a disk structure. */
/* When reiserfs_find_entry or search_by_entry_key find directory entry, they return filled reiserfs_dir_entry structure */
struct reiserfs_dir_entry
{
  struct buffer_head * de_bh;
  int de_item_num;
  struct item_head * de_ih;
  int de_entry_num;
  struct reiserfs_de_head * de_deh;
  int de_entrylen;
  int de_namelen;
  char * de_name;
  char * de_gen_number_bit_string;

  __u32 de_dir_id;
  __u32 de_objectid;

  struct key de_entry_key;
};
   
/* these defines are useful when a particular member of a reiserfs_dir_entry is needed */

/* pointer to file name, stored in entry */
#define B_I_DEH_ENTRY_FILE_NAME(bh,ih,deh) (B_I_PITEM (bh, ih) + (deh)->deh_location)

/* length of name */
#define I_DEH_N_ENTRY_FILE_NAME_LENGTH(ih,deh,entry_num) \
(I_DEH_N_ENTRY_LENGTH (ih, deh, entry_num) - (de_with_sd (deh) ? SD_SIZE : 0))

#define DEH_OBJECTID(deh) ((deh)->deh_objectid)

/* hash value occupies 24 bits starting from 7 up to 30 */
#define GET_HASH_VALUE(offset) ((offset) & 0x7fffff80)
/* generation number occupies 7 bits starting from 0 up to 6 */
#define GET_GENERATION_NUMBER(offset) ((offset) & 0x0000007f)


/*
 * Picture represents an internal node of the reiserfs tree
 *  ______________________________________________________
 * |      |  Array of     |  Array of         |  Free     |
 * |block |    keys       |  pointers         | space     |
 * | head |      N        |      N+1          |           |
 * |______|_______________|___________________|___________|
 */

/***************************************************************************/
/*                      DISK CHILD                                         */
/***************************************************************************/
/* Disk child pointer: The pointer from an internal node of the tree
   to a node that is on disk. */
struct disk_child {
  unsigned long       dc_block_number;              /* Disk child's block number. */
  unsigned short      dc_size;		            /* Disk child's used space.   */
};

#define DC_SIZE (sizeof(struct disk_child))

/* Get disk child by buffer header and position in the tree node. */
#define B_N_CHILD(p_s_bh,n_pos)  ((struct disk_child *)\
((p_s_bh)->b_data+BLKH_SIZE+B_NR_ITEMS(p_s_bh)*KEY_SIZE+DC_SIZE*(n_pos)))

/* Get disk child number by buffer header and position in the tree node. */
#define B_N_CHILD_NUM(p_s_bh,n_pos) (B_N_CHILD(p_s_bh,n_pos)->dc_block_number)

 /* maximal value of field child_size in structure disk_child */ 
 /* child size is the combined size of all items and their headers */
#define MAX_CHILD_SIZE(bh) ((int)( (bh)->b_size - BLKH_SIZE ))

/* amount of used space in buffer (not including block head) */
#define B_CHILD_SIZE(cur) (MAX_CHILD_SIZE(cur)-(B_FREE_SPACE(cur)))

/* max and min number of keys in internal node */
#define MAX_NR_KEY(bh) ( (MAX_CHILD_SIZE(bh)-DC_SIZE)/(KEY_SIZE+DC_SIZE) )
#define MIN_NR_KEY(bh)    (MAX_NR_KEY(bh)/2)

/***************************************************************************/
/*                      PATH STRUCTURES AND DEFINES                        */
/***************************************************************************/


/* Search_by_key fills up the path from the root to the leaf as it descends the tree looking for the
   key.  It uses reiserfs_bread to try to find buffers in the cache given their block number.  If it
   does not find them in the cache it reads them from disk.  For each node search_by_key finds using
   reiserfs_bread it then uses bin_search to look through that node.  bin_search will find the
   position of the block_number of the next node if it is looking through an internal node.  If it
   is looking through a leaf node bin_search will find the position of the item which has key either
   equal to given key, or which is the maximal key less than the given key. */

struct  path_element  {
  struct buffer_head *	pe_buffer;    /* Pointer to the buffer at the path in the tree. */
  int         		pe_position;  /* Position in the tree node which is placed in the */
                                      /* buffer above.                                  */
};

#define MAX_HEIGHT 5 /* maximal height of a tree. don't change this without changing JOURNAL_PER_BALANCE_CNT */
#define EXTENDED_MAX_HEIGHT         7 /* Must be equals MAX_HEIGHT + FIRST_PATH_ELEMENT_OFFSET */
#define FIRST_PATH_ELEMENT_OFFSET   2 /* Must be equal to at least 2. */

#define ILLEGAL_PATH_ELEMENT_OFFSET 1 /* Must be equal to FIRST_PATH_ELEMENT_OFFSET - 1 */
#define MAX_FEB_SIZE 6   /* this MUST be MAX_HEIGHT + 1. See about FEB below */



/* We need to keep track of who the ancestors of nodes are.  When we
   perform a search we record which nodes were visited while
   descending the tree looking for the node we searched for. This list
   of nodes is called the path.  This information is used while
   performing balancing.  Note that this path information may become
   invalid, and this means we must check it when using it to see if it
   is still valid. You'll need to read search_by_key and the comments
   in it, especially about decrement_counters_in_path(), to understand
   this structure. */
struct  path {
  struct  path_element  path_elements[EXTENDED_MAX_HEIGHT];	/* Array of the path elements.  */
  int                   path_length;                      	/* Length of the array above.   */
  int			pos_in_item;
};

/* Get path element by path and path position. */
#define PATH_OFFSET_PELEMENT(p_s_path,n_offset)  ((p_s_path)->path_elements +(n_offset))

/* Get buffer header at the path by path and path position. */
#define PATH_OFFSET_PBUFFER(p_s_path,n_offset)   (PATH_OFFSET_PELEMENT(p_s_path,n_offset)->pe_buffer)

/* Get position in the element at the path by path and path position. */
#define PATH_OFFSET_POSITION(p_s_path,n_offset) (PATH_OFFSET_PELEMENT(p_s_path,n_offset)->pe_position)


#define PATH_PLAST_BUFFER(p_s_path) (PATH_OFFSET_PBUFFER((p_s_path), (p_s_path)->path_length))
#define PATH_LAST_POSITION(p_s_path) (PATH_OFFSET_POSITION((p_s_path), (p_s_path)->path_length))


#define PATH_PITEM_HEAD(p_s_path)    B_N_PITEM_HEAD(PATH_PLAST_BUFFER(p_s_path),PATH_LAST_POSITION(p_s_path))

/* in do_balance leaf has h == 0 in contrast with path structure,
   where root has level == 0. That is why we need these defines */
#define PATH_H_PBUFFER(p_s_path, h) PATH_OFFSET_PBUFFER (p_s_path, p_s_path->path_length - (h))	/* tb->S[h] */
#define PATH_H_PPARENT(path, h) PATH_H_PBUFFER (path, (h) + 1)			/* tb->F[h] or tb->S[0]->b_parent */
#define PATH_H_POSITION(path, h) PATH_OFFSET_POSITION (path, path->path_length - (h))	
#define PATH_H_B_ITEM_ORDER(path, h) PATH_H_POSITION(path, h + 1)		/* tb->S[h]->b_item_order */

#define PATH_H_PATH_OFFSET(p_s_path, n_h) ((p_s_path)->path_length - (n_h))


/***************************************************************************/
/*                       MISC                                              */
/***************************************************************************/


/* Size of pointer to the unformatted node. */
#define UNFM_P_SIZE (sizeof(unsigned long))

#define INODE_PKEY(inode) ((struct key *)((inode)->u.reiserfs_i.i_key))

/* these say to reiserfs_file_read about desired kind of read ahead */

#ifdef REISERFS_CHECK
extern int g_kmalloc_count;
#endif




/***************************************************************************/
/*                PRESERVE LIST STUFF                                      */
/***************************************************************************/
/* This flag tracks whether a buffer might contain data that has been shifted to it from another
   node which is on disk and might be obliterated if power fails and this buffer is not written to
   disk before that other node is. This flag is cleared in unlock buffers..  Note that this assumes
   that write_caching is turned off for the disk.  Linux drivers turn write_caching off by default,
   so this should be correct.  I need this flag to ensure that we don't pass a block, from which
   items were shifted, to the scsi controller, free the preserve list, use a freed block, and then
   have the scsi controller reorder the writes so that the supposedly preserved block is overwritten
   before the block containing the shifted items reaches disk, and then risk the system crashing
   before the shifted items reach disk.  See preserve.c for a discussion of the preserve list and
   its role. -Hans */
#define BH_Suspected_Recipient	12

/* 1 if the node that this buffer has assigned to it has not been written to disk since it was
   assigned to the buffer.  If a buffer is BH_Unwritten then there is no need to preserve the
   contents of its block after balancing shifts data from it.  See preserve.c for a discussion of
   the preserve list and its role. */
#define BH_Unwritten	13

/* 1 if the block that this buffer was last read from or written to has been placed on the preserved
   list.  See preserve.c for a discussion of the preserve list and its role. */
#define BH_Preserved	14


/* modes of preserving */
#define PRESERVE_DIRECT_TO_INDIRECT 1
#define PRESERVE_INDIRECT_TO_DIRECT 2
#define PRESERVE_RENAMING 3
#define NOTHING_SPECIAL 4


/* return value for get_space_from_preserve_list */
#define PRESERVE_LIST_WAS_EMPTY 0
#define FEW_BLOCKS_ARE_FREED 1


#define MAX_UL_INT 0xffffffff
#define MAX_INT    0x7ffffff
#define MAX_US_INT 0xffff

#define MAX_KEY_OFFSET		MAX_UL_INT
#define MAX_KEY_UNIQUENESS	MAX_UL_INT
#define MAX_KEY_OBJECTID	MAX_UL_INT

#define MAX_B_NUM  MAX_UL_INT
#define MAX_FC_NUM MAX_US_INT


/* the purpose is to detect overflow of an unsigned short */
#define REISERFS_LINK_MAX (MAX_US_INT - 1000)


/* The following defines are used in reiserfs_insert_item and reiserfs_append_item  */
#define REISERFS_KERNEL_MEM		0	/* reiserfs kernel memory mode	*/
#define REISERFS_USER_MEM		1	/* reiserfs user memory mode		*/


/***************************************************************************/
/*                  FIXATE NODES                                           */
/***************************************************************************/

#define VI_TYPE_STAT_DATA 1
#define VI_TYPE_DIRECT 2
#define VI_TYPE_INDIRECT 4
#define VI_TYPE_DIRECTORY 8
#define VI_TYPE_FIRST_DIRECTORY_ITEM 16
#define VI_TYPE_INSERTED_DIRECTORY_ITEM 32

#define VI_TYPE_LEFT_MERGEABLE 64
#define VI_TYPE_RIGHT_MERGEABLE 128

/* To make any changes in the tree we always first find node, that contains item to be changed/deleted or
   place to insert a new item. We call this node S. To do balancing we need to decide what we will shift to
   left/right neighbor, or to a new node, where new item will be etc. To make this analysis simpler we build
   virtual node. Virtual node is an array of items, that will replace items of node S. (For instance if we are
   going to delete an item, virtual node does not contain it). Virtual node keeps information about item sizes
   and types, mergeability of first and last items, sizes of all entries in directory item. We use this array
   of items when calculating what we can shift to neighbors and how many nodes we have to have if we do not
   any shiftings, if we shift to left/right neighbor or to both. */
struct virtual_item
{
  unsigned short vi_type;		/* item type, mergeability */
  unsigned short vi_item_len;           /* length of item that it will have after balancing */
  
  short vi_entry_count;			/* number of entries in directory item (including the new one if any,
					   or excluding entry if it must be cut) */
  unsigned short * vi_entry_sizes;	/* array of entry lengths for directory item */
};

struct virtual_node
{
  char * vn_free_ptr;		/* this is a pointer to the free space in the buffer */
  unsigned short vn_nr_item;	/* number of items in virtual node */
  short vn_size;        	/* size of node , that node would have if it has unlimited size and no balancing is performed */
  short vn_mode;		/* mode of balancing (paste, insert, delete, cut) */
  short vn_affected_item_num; 
  short vn_pos_in_item;
  struct item_head * vn_ins_ih;	/* item header of inserted item, 0 for other modes */
  struct virtual_item * vn_vi;	/* array of items (including a new one, excluding item to be deleted) */
};


/***************************************************************************/
/*                  TREE BALANCE                                           */
/***************************************************************************/

/* This temporary structure is used in tree balance algorithms, and
   constructed as we go to the extent that its various parts are
   needed.  It contains arrays of nodes that can potentially be
   involved in the balancing of node S, and parameters that define how
   each of the nodes must be balanced.  Note that in these algorithms
   for balancing the worst case is to need to balance the current node
   S and the left and right neighbors and all of their parents plus
   create a new node.  We implement S1 balancing for the leaf nodes
   and S0 balancing for the internal nodes (S1 and S0 are defined in
   our papers.)*/

#define MAX_FREE_BLOCK 7	/* size of the array of buffers to free at end of do_balance */

/*#define MAX_DIRTIABLE 3*/		/* L, S, R */
#define MAX_PRESERVE_NODES 2

/* maximum number of FEB blocknrs on a single level */
#define MAX_AMOUNT_NEEDED 2

/* someday somebody will prefix every field in this struct with tb_ */
struct tree_balance
{
  struct super_block * tb_sb;
  struct path * tb_path;
  struct buffer_head * L[MAX_HEIGHT];        /* array of left neighbors of nodes in the path */
  struct buffer_head * R[MAX_HEIGHT];        /* array of right neighbors of nodes in the path*/
  struct buffer_head * FL[MAX_HEIGHT];       /* array of fathers of the left  neighbors      */
  struct buffer_head * FR[MAX_HEIGHT];       /* array of fathers of the right neighbors      */
  struct buffer_head * CFL[MAX_HEIGHT];      /* array of common parents of center node and its left neighbor  */
  struct buffer_head * CFR[MAX_HEIGHT];      /* array of common parents of center node and its right neighbor */

  /* array of blocknr's that are free and are the nearest to the left node that are usable
     for writing dirty formatted leaves, using the write_next_to algorithm. */
  /*unsigned long free_and_near[MAX_DIRTIABLE];*/

  /* nodes will be used in preserving */
                                /* so we don't just get blocks, we have to get buffers which we likely won't use.  Seems
                                   like it is not optimal. -Hans */
  struct buffer_head * tb_nodes_for_preserving[MAX_PRESERVE_NODES];
  struct buffer_head * preserved[MAX_PRESERVE_NODES];
  struct buffer_head * FEB[MAX_FEB_SIZE]; /* array of empty buffers. Number of buffers in array equals
					     cur_blknum. */
  struct buffer_head * used[MAX_FEB_SIZE];
  short int lnum[MAX_HEIGHT];	/* array of number of items which must be shifted to the left in
				   order to balance the current node; for leaves includes item
				   that will be partially shifted; for internal nodes, it is
				   the number of child pointers rather than items. It includes
				   the new item being created.  For preserve_shifted() purposes
				   the code sometimes subtracts one from this number to get the
				   number of currently existing items being shifted, and even
				   more often for leaves it subtracts one to get the number of
				   wholly shifted items for other purposes. */
  short int rnum[MAX_HEIGHT];	/* substitute right for left in comment above */
  short int lkey[MAX_HEIGHT];               /* array indexed by height h mapping the key delimiting L[h] and
					       S[h] to its item number within the node CFL[h] */
  short int rkey[MAX_HEIGHT];               /* substitute r for l in comment above */
  short int insert_size[MAX_HEIGHT];        /* the number of bytes by we are trying to add or remove from
					       S[h]. A negative value means removing.  */
  short int blknum[MAX_HEIGHT];             /* number of nodes that will replace node S[h] after
					       balancing on the level h of the tree.  If 0 then S is
					       being deleted, if 1 then S is remaining and no new nodes
					       are being created, if 2 or 3 then 1 or 2 new nodes is
					       being created */

  /* fields that are used only for balancing leaves of the tree */
  short int cur_blknum;	/* number of empty blocks having been already allocated			*/
  short int s0num;             /* number of items that fall into left most  node when S[0] splits	*/
  short int s1num;             /* number of items that fall into first  new node when S[0] splits	*/
  short int s2num;             /* number of items that fall into second new node when S[0] splits	*/
  short int lbytes;            /* number of bytes which can flow to the left neighbor from the	left	*/
  /* most liquid item that cannot be shifted from S[0] entirely		*/
  /* if -1 then nothing will be partially shifted */
  short int rbytes;            /* number of bytes which will flow to the right neighbor from the right	*/
  /* most liquid item that cannot be shifted from S[0] entirely		*/
  /* if -1 then nothing will be partially shifted                           */
  short int s1bytes;		/* number of bytes which flow to the first  new node when S[0] splits	*/
            			/* note: if S[0] splits into 3 nodes, then items do not need to be cut	*/
  short int s2bytes;
  struct buffer_head * buf_to_free[MAX_FREE_BLOCK]; /* buffers which are to be freed after do_balance finishes by unfix_nodes */
  char * vn_buf;		/* kmalloced memory. Used to create
				   virtual node and keep map of
				   dirtied bitmap blocks */
  int vn_buf_size;		/* size of the vn_buf */
  struct virtual_node * tb_vn;	/* VN starts after bitmap of bitmap blocks */
  char preserve_mode;		/* indicates that the deletion that will be done is part of a conversion of
				   a tail to an unformatted node, and the buffer with the item should be
				   preserve_shifted(). see preserve.c */
} ;
#define DIRTY_BITMAP_MAP(tb) ((tb)->vn_buf)

#if 0
				/* when balancing we potentially affect a 3 node wide column of nodes
                                   in the tree (the top of the column may be tapered). C is the nodes
                                   at the center of this column, and L and R are the nodes to the
                                   left and right.  */
  struct seal * L_path_seals[MAX_HEIGHT];
  struct seal * C_path_seals[MAX_HEIGHT];
  struct seal * R_path_seals[MAX_HEIGHT];
  char L_path_lock_types[MAX_HEIGHT];   /* 'r', 'w', or 'n' for read, write, or none */
  char C_path_lock_types[MAX_HEIGHT];
  char R_path_lock_types[MAX_HEIGHT];


  struct seal_list_elem * C_seal[MAX_HEIGHT];        /* array of seals on nodes in the path */
  struct seal_list_elem * L_seal[MAX_HEIGHT];        /* array of seals on left neighbors of nodes in the path */
  struct seal_list_elem * R_seal[MAX_HEIGHT];        /* array of seals on right neighbors of nodes in the path*/
  struct seal_list_elem * FL_seal[MAX_HEIGHT];       /* array of seals on fathers of the left  neighbors      */
  struct seal_list_elem * FR_seal[MAX_HEIGHT];       /* array of seals on fathers of the right neighbors      */
  struct seal_list_elem * CFL_seal[MAX_HEIGHT];      /* array of seals on common parents of center node and its left neighbor  */
  struct seal_list_elem * CFR_seal[MAX_HEIGHT];      /* array of seals on common parents of center node and its right neighbor */
 
  struct char C_desired_lock_type[MAX_HEIGHT]; /* 'r', 'w', or 'n' for read, write, or none */
  struct char L_desired_lock_type[MAX_HEIGHT];        
  struct char R_desired_lock_type[MAX_HEIGHT];        
  struct char FL_desired_lock_type[MAX_HEIGHT];       
  struct char FR_desired_lock_type[MAX_HEIGHT];       
  struct char CFL_desired_lock_type[MAX_HEIGHT];      
  struct char CFR_desired_lock_type[MAX_HEIGHT];      
#endif





/* These are modes of balancing */

/* When inserting an item. */
#define M_INSERT	'i'
/* When inserting into (directories only) or appending onto an already
   existant item. */
#define M_PASTE		'p'
/* When deleting an item. */
#define M_DELETE	'd'
/* When truncating an item or removing an entry from a (directory) item. */
#define M_CUT 		'c'

/* used when balancing on leaf level skipped (in reiserfsck) */
#define M_INTERNAL	'n'

/* When further balancing is not needed, then do_balance does not need
   to be called. */
#define M_SKIP_BALANCING 		's'
#define M_CONVERT	'v'

/* modes of leaf_move_items */
#define LEAF_FROM_S_TO_L 0
#define LEAF_FROM_S_TO_R 1
#define LEAF_FROM_R_TO_L 2
#define LEAF_FROM_L_TO_R 3
#define LEAF_FROM_S_TO_SNEW 4

#define FIRST_TO_LAST 0
#define LAST_TO_FIRST 1

/* used in do_balance for passing parent of node information that has
   been gotten from tb struct */
struct buffer_info {
	struct buffer_head * bi_bh;
	struct buffer_head * bi_parent;
	int bi_position;
};


/* there are 4 types of items: stat data, directory item, indirect, direct.
+-------------------+------------+--------------+------------+
|	            |  k_offset  | k_uniqueness | mergeable? |
+-------------------+------------+--------------+------------+
|     stat data     |	0        |      0       |   no       |
+-------------------+------------+--------------+------------+
| 1st directory item| DOT_OFFSET |DIRENTRY_UNIQUENESS|   no       | 
| non 1st directory | hash value |              |   yes      |
|     item          |            |              |            |
+-------------------+------------+--------------+------------+
| indirect item     | offset + 1 |TYPE_INDIRECT |   if this is not the first indirect item of the object
+-------------------+------------+--------------+------------+
| direct item       | offset + 1 |TYPE_DIRECT   | if not this is not the first direct item of the object
+-------------------+------------+--------------+------------+
*/
#define TYPE_STAT_DATA 0x0
#define TYPE_DIRECT 0xffffffff
#define TYPE_INDIRECT 0xfffffffe
#define TYPE_DIRECTORY_MAX 0xfffffffd


#define KEY_IS_STAT_DATA_KEY(p_s_key) 	( (p_s_key)->k_uniqueness == TYPE_STAT_DATA )
#define KEY_IS_DIRECTORY_KEY(p_s_key)	( (p_s_key)->k_uniqueness == DIRENTRY_UNIQUENESS )
#define KEY_IS_DIRECT_KEY(p_s_key) 	( (p_s_key)->k_uniqueness == TYPE_DIRECT )
#define KEY_IS_INDIRECT_KEY(p_s_key)	( (p_s_key)->k_uniqueness == TYPE_INDIRECT )

#define I_IS_STAT_DATA_ITEM(p_s_ih) 	KEY_IS_STAT_DATA_KEY(&((p_s_ih)->ih_key))
#define I_IS_DIRECTORY_ITEM(p_s_ih) 	KEY_IS_DIRECTORY_KEY(&((p_s_ih)->ih_key))
#define I_IS_DIRECT_ITEM(p_s_ih) 	KEY_IS_DIRECT_KEY(&((p_s_ih)->ih_key))
#define I_IS_INDIRECT_ITEM(p_s_ih) 	KEY_IS_INDIRECT_KEY(&((p_s_ih)->ih_key))

/*
#ifdef __KERNEL__

extern inline int is_left_mergeable (struct item_head * ih, unsigned long bsize)
{
  if (I_IS_DIRECT_ITEM (ih))
    return (ih->ih_key.k_offset % bsize != 1);

  if (I_IS_INDIRECT_ITEM (ih))
    return (ih->ih_key.k_offset != 1);

  if (I_IS_DIRECTORY_ITEM (ih))
   return ((ih)->ih_key.k_offset != DOT_OFFSET);

#ifdef REISERFS_CHECK
  if ( ! I_IS_STAT_DATA_ITEM (ih))
    reiserfs_panic (0, "is_left_mergeable: 1020: item [%lu %lu %lu %lu] must be a stat data",
		    ih->ih_key.k_dir_id, ih->ih_key.k_objectid, ih->ih_key.k_offset, ih->ih_key.k_uniqueness);
#endif
  return 0;
}



#endif*/	/* __KERNEL__ */

#define COMP_KEYS comp_keys
#define COMP_SHORT_KEYS comp_short_keys
/*#define COMP_KEYS(p_s_key1, p_s_key2)		comp_keys((unsigned long *)(p_s_key1), (unsigned long *)(p_s_key2))
#define COMP_SHORT_KEYS(p_s_key1, p_s_key2)	comp_short_keys((unsigned long *)(p_s_key1), (unsigned long *)(p_s_key2))*/


/* number of blocks pointed to by the indirect item */
#define I_UNFM_NUM(p_s_ih)	( (p_s_ih)->ih_item_len / UNFM_P_SIZE )

/* the used space within the unformatted node corresponding to pos within the item pointed to by ih */
#define I_POS_UNFM_SIZE(ih,pos,size) (((pos) == I_UNFM_NUM(ih) - 1 ) ? (size) - (ih)->u.ih_free_space : (size))

/* number of bytes contained by the direct item or the unformatted nodes the indirect item points to */

#define I_BYTES_NUMBER(ih,size) (( I_IS_INDIRECT_ITEM(ih) ) ?\
				 I_UNFM_NUM(ih)*size - (ih)->u.ih_free_space :\
				 (( I_IS_DIRECT_ITEM(ih) ) ? (ih)->ih_item_len : 0 /* stat data */)) 

/* check whether byte number 'offset' is in this item */
#define I_OFF_BYTE_IN_ITEM(p_s_ih, n_offset, n_blocksize) \
                  ( (p_s_ih)->ih_key.k_offset <= (n_offset) && \
                    (p_s_ih)->ih_key.k_offset + I_BYTES_NUMBER(p_s_ih,n_blocksize) > (n_offset) )

/* get the item header */ 
#define B_N_PITEM_HEAD(bh,item_num) ( (struct item_head * )((bh)->b_data + BLKH_SIZE) + (item_num) )

/* get key */
#define B_N_PDELIM_KEY(bh,item_num) ( (struct key * )((bh)->b_data + BLKH_SIZE) + (item_num) )

/* get the key */
#define B_N_PKEY(bh,item_num) ( &(B_N_PITEM_HEAD(bh,item_num)->ih_key) )

/* get item body */
#define B_N_PITEM(bh,item_num) ( (bh)->b_data + B_N_PITEM_HEAD((bh),(item_num))->ih_item_location)

/* get the stat data by the buffer header and the item order */
#define B_N_STAT_DATA(bh,nr) \
( (struct stat_data *)((bh)->b_data+B_N_PITEM_HEAD((bh),(nr))->ih_item_location ) )

                 /* following defines use reiserfs buffer header and item header */
 /* get item body */
#define B_I_PITEM(bh,ih) ( (bh)->b_data + (ih)->ih_item_location )

/* get stat-data */
#define B_I_STAT_DATA(bh, ih) ( (struct stat_data * )((bh)->b_data + (ih)->ih_item_location) )

#define MAX_DIRECT_ITEM_LEN(size) ((size) - BLKH_SIZE - 2*IH_SIZE - SD_SIZE - UNFM_P_SIZE)

/* indirect items consist of entries which contain blocknrs, pos
   indicates which entry, and B_I_POS_UNFM_POINTER resolves to the
   blocknr contained by the entry pos points to */
#define B_I_POS_UNFM_POINTER(bh,ih,pos) (*(((unsigned long *)B_I_PITEM(bh,ih)) + (pos)))

/* Reiserfs buffer cache statistics. */
#ifdef REISERFS_CACHE_STAT
 struct reiserfs_cache_stat
	{
  	int nr_reiserfs_ll_r_block; 		/* Number of block reads. */
  	int nr_reiserfs_ll_w_block; 		/* Number of block writes. */
	int nr_reiserfs_schedule; 		/* Number of locked buffers waits. */
	unsigned long nr_reiserfs_bread;	/* Number of calls to reiserfs_bread function */
	unsigned long nr_returns; /* Number of breads of buffers that were hoped to contain a key but did not after bread completed
				     (usually due to object shifting while bread was executing.)
				     In the code this manifests as the number
				     of times that the repeat variable is nonzero in search_by_key.*/
	unsigned long nr_fixed;		/* number of calls of fix_nodes function */
	unsigned long nr_failed;	/* number of calls of fix_nodes in which schedule occurred while the function worked */
	unsigned long nr_find1;		/* How many times we access a child buffer using its direct pointer from an internal node.*/
	unsigned long nr_find2;	        /* Number of times there is neither a direct pointer to
					   nor any entry in the child list pointing to the buffer. */
	unsigned long nr_find3;	        /* When parent is locked (meaning that there are no direct pointers)
					   or parent is leaf and buffer to be found is an unformatted node. */
	}  cache_stat;
#endif



/***************************************************************************/
/*                    FUNCTION DECLARATIONS                                */
/***************************************************************************/

/*#ifdef __KERNEL__*/

/* journal.c see journal.c for all the comments here */

#define JOURNAL_TRANS_HALF 1018   /* must be correct to keep the desc and commit structs at 4k */

/* first block written in a commit.  BUG, not 64bit safe */
struct reiserfs_journal_desc {
  unsigned long j_trans_id ;			/* id of commit */
  unsigned long j_len ;			/* length of commit. len +1 is the commit block */
  unsigned long j_mount_id ;				/* mount id of this trans*/
  unsigned long j_realblock[JOURNAL_TRANS_HALF] ; /* real locations for each block */
  char j_magic[12] ;
} ;

/* last block written in a commit BUG, not 64bit safe */
struct reiserfs_journal_commit {
  unsigned long j_trans_id ;			/* must match j_trans_id from the desc block */
  unsigned long j_len ;			/* ditto */
  unsigned long j_realblock[JOURNAL_TRANS_HALF] ; /* real locations for each block */
  char j_digest[16] ;			/* md5 sum of all the blocks involved, including desc and commit. not used, kill it */
} ;

/* this header block gets written whenever a transaction is considered fully flushed, and is more recent than the
** last fully flushed transaction.  fully flushed means all the log blocks and all the real blocks are on disk,
** and this transaction does not need to be replayed.
*/
struct reiserfs_journal_header {
  unsigned long j_last_flush_trans_id ;		/* id of last fully flushed transaction */
  unsigned long j_first_unflushed_offset ;      /* offset in the log of where to start replay after a crash */
  unsigned long j_mount_id ;
} ;


/* biggest tunable defines are right here */
#define JOURNAL_BLOCK_COUNT 8192 /* number of blocks in the journal */
#define JOURNAL_MAX_BATCH   900 /* max blocks to batch into one transaction, don't make this any bigger than 900 */
#define JOURNAL_MAX_COMMIT_AGE 30 
#define JOURNAL_MAX_TRANS_AGE 30
#define JOURNAL_PER_BALANCE_CNT 12   /* must be >= (5 + 2 * (MAX_HEIGHT-2) + 1) */
#define JOURNAL_DEL_SIZE_LIMIT 40960 /* size in bytes of the max sized file to use cnodes while deleting */

/* state bits for the per FS journal struct */
#define JOURNAL_UNMOUNTING 1 /* tells the commit thread he's done */

/* hash funcs more or less stolen from buffer cache.  t is a pointer to the hash table */
#define JHASHDEV(d) ((unsigned int) (d))
#define _jhashfn(dev,block)  (((unsigned)(JHASHDEV(dev)^(block))) % JOURNAL_HASH_SIZE)
#define journal_hash(t,dev,block) ((t)[_jhashfn((dev),(block))])

/* finds n'th buffer with 0 being the start of this commit.  Needs to go away, j_ap_blocks has changed
** since I created this.  One chunk of code in journal.c needs changing before deleting it
*/
#define JOURNAL_BUFFER(j,n) ((j)->j_ap_blocks[((j)->j_start + (n)) % JOURNAL_BLOCK_COUNT])

int journal_init(struct super_block *) ;
int journal_release(struct reiserfs_transaction_handle*, struct super_block *) ;
int journal_release_error(struct reiserfs_transaction_handle*, struct super_block *) ;
int journal_end(struct reiserfs_transaction_handle *, struct super_block *, unsigned long) ;
int journal_end_sync(struct reiserfs_transaction_handle *, struct super_block *, unsigned long) ;
int journal_mark_dirty_nolog(struct reiserfs_transaction_handle *, struct super_block *, struct buffer_head *bh) ;
int journal_mark_freed(struct reiserfs_transaction_handle *, struct super_block *, unsigned long blocknr) ;
int push_journal_writer(char *w) ;
int pop_journal_writer(int windex) ;
int journal_lock_dobalance(struct super_block *p_s_sb) ;
int journal_unlock_dobalance(struct super_block *p_s_sb) ;
int journal_transaction_should_end(struct reiserfs_transaction_handle *, int) ;
int reiserfs_in_journal(struct super_block *p_s_sb, kdev_t dev, unsigned long bl, int size, int searchall, unsigned long *next) ;
int journal_begin(struct reiserfs_transaction_handle *, struct super_block *p_s_sb, unsigned long) ;
int journal_join(struct reiserfs_transaction_handle *, struct super_block *p_s_sb, unsigned long) ;
struct super_block *reiserfs_get_super(kdev_t dev) ;
void flush_async_commits(struct super_block *p_s_sb, int *repeat) ;

int remove_from_transaction(struct super_block *p_s_sb, unsigned long blocknr, int already_cleaned) ;
int remove_from_journal_list(struct super_block *s, struct reiserfs_journal_list *jl, struct buffer_head *bh, int remove_freed) ;

int buffer_journaled(struct buffer_head *bh) ;
int mark_buffer_journal_new(struct buffer_head *bh) ;
int reiserfs_sync_all_buffers(kdev_t dev, int wait) ;
int reiserfs_sync_buffers(kdev_t dev, int wait) ;
void reiserfs_file_buffer(struct buffer_head *bh, int list) ;
void reiserfs_update_inode_transaction(struct inode *) ;
int reiserfs_inode_in_this_transaction(struct inode *) ;
void reiserfs_commit_for_inode(struct inode *) ;

				/* Why is this kerplunked right here? -Hans */
/* buffer was journaled, waiting to get to disk */
static inline int buffer_journal_dirty(struct buffer_head *bh) {
  if (bh)
    return test_bit(BH_JDirty_wait, &bh->b_state) ;
  else
    return 0 ;
}
static inline int mark_buffer_notjournal_dirty(struct buffer_head *bh) {
  if (bh)
    clear_bit(BH_JDirty_wait, &bh->b_state) ;
  return 0 ;
}
static inline int mark_buffer_notjournal_new(struct buffer_head *bh) {
  if (bh) {
    clear_bit(BH_JNew, &bh->b_state) ;
  }
  return 0 ;
}


/* objectid.c */
unsigned long reiserfs_get_unused_objectid (struct reiserfs_transaction_handle *th, struct super_block * s);
void reiserfs_release_objectid (struct reiserfs_transaction_handle *th, unsigned long objectid_to_release, struct super_block * s);


/* stree.c */
int reiserfs_file_release(struct inode *p_s_inode, struct file *p_s_filp) ;
int B_IS_IN_TREE(struct buffer_head *);
extern inline void copy_key (void * to, void * from);
extern inline void copy_short_key (void * to, void * from);
extern inline void copy_item_head(void * p_v_to, void * p_v_from);
extern inline int comp_keys (void * p_s_key1, void * p_s_key2);
extern inline int  comp_short_keys (void * p_s_key1, void * p_s_key2);
int comp_items (struct item_head  * p_s_ih, struct path * p_s_path);
struct key * get_rkey (struct path * p_s_chk_path, struct super_block  * p_s_sb);
inline int bin_search (void * p_v_key, void * p_v_base, int p_n_num, int p_n_width, int * p_n_pos);
int search_by_key (struct super_block *, struct key *, struct path *, int * , int, int);
int search_by_objectid (struct super_block *, struct key *, struct path *, int * , int, int);
int search_for_position_by_key (struct super_block * p_s_sb, struct key * p_s_key, struct path * p_s_search_path, int * p_n_pos_in_item, int * p_n_repeat);
extern inline void decrement_bcount (struct buffer_head * p_s_bh);
void decrement_counters_in_path (struct path * p_s_search_path);
void pathrelse (struct path * p_s_search_path);
int reiserfs_insert_item (struct reiserfs_transaction_handle *th, struct super_block * sb, struct path * path, 
                          struct item_head * ih, const char * body, int memmode, int zeros_number, int preserve_mode);
int reiserfs_paste_into_item (struct reiserfs_transaction_handle *th, struct super_block * sb, 
                              struct path * path, int * pos_in_item, struct key * key,	
			      const char * body, int paste_size, int memmode, int zeros_number);	/* returns -1 if failed and paste_size otherwise */
int reiserfs_cut_from_item (struct reiserfs_transaction_handle *th, struct inode * inode, 
			    struct super_block * sb, struct path * path, int * pos_in_item, struct key * key,
			    unsigned long new_file_size, int preserve_mode);
int reiserfs_delete_item (struct reiserfs_transaction_handle *th, struct inode * inode, struct path * p_s_path, 
                          int * p_n_pos_in_item, struct key * p_s_item_key,
			  struct buffer_head  * p_s_un_bh, int preserve_mode);
void reiserfs_delete_object (struct reiserfs_transaction_handle *th, struct inode * p_s_inode);
void reiserfs_truncate_file (struct  inode * p_s_inode);
int lock_inode_to_convert (struct inode * p_s_inode);
void unlock_inode_after_convert (struct inode * p_s_inode);
int increment_i_read_sync_counter (struct inode * p_s_inode);
void decrement_i_read_sync_counter (struct inode * p_s_inode);
int get_buffer_by_range (struct super_block * p_s_sb, struct key * p_s_range_begin, struct key * p_s_range_end, 
			 struct buffer_head ** pp_s_buf, unsigned long * p_n_objectid);
int get_buffers_from_range (struct super_block * p_s_sb, struct key * p_s_range_start, struct key * p_s_range_end, struct buffer_head ** p_s_range_buffers,
			    int n_max_nr_buffers_to_return);
#ifndef REISERFS_FSCK

inline int is_left_mergeable (struct item_head * ih, unsigned long bsize);

#else

int is_left_mergeable (struct super_block * s, struct path * path);
int is_right_mergeable (struct super_block * s, struct path * path);
int are_items_mergeable (struct item_head * left, struct item_head * right, int bsize);

#endif

/* inode.c */
void store_key (struct super_block *s, struct key * key);
void forget_key (struct super_block *s, struct key * key);
struct inode * reiserfs_iget (struct super_block * s, struct key * key);
void reiserfs_read_inode (struct inode * inode);
void reiserfs_delete_inode (struct inode * inode);
extern int reiserfs_notify_change(struct dentry * dentry, struct iattr * attr);
int reiserfs_bmap (struct inode * inode, int block);
void reiserfs_write_inode (struct inode * inode);
struct inode * reiserfs_new_inode (struct reiserfs_transaction_handle *th, 
			           const struct inode * dir, int mode, const char * symname,
				   struct dentry *dentry, struct inode *inode, int * err);
int reiserfs_sync_inode (struct reiserfs_transaction_handle *th, struct inode * inode);
void if_in_ram_update_sd (struct reiserfs_transaction_handle *th, struct inode * inode);
void reiserfs_inode_setattr(struct reiserfs_transaction_handle *th, struct inode * inode,  struct iattr * attr);
void __wait_on_inode(struct inode * inode);

/* namei.c */
int bin_search_in_dir_item (struct item_head * ih, struct reiserfs_de_head * deh, struct key * key, int * pos_in_item);
int search_by_entry_key (struct super_block * sb, struct key * key, struct path * path, int * pos_in_item, int * repeat);
struct dentry * reiserfs_lookup (struct inode * dir, struct dentry *dentry);
int reiserfs_create (struct inode * dir, struct dentry *dentry,	int mode);
int reiserfs_mknod (struct inode * dir_inode, struct dentry *dentry, int mode, int rdev);
int reiserfs_mkdir (struct inode * dir, struct dentry *dentry, int mode);
int reiserfs_rmdir (struct inode * dir,	struct dentry *dentry);
int reiserfs_unlink (struct inode * dir, struct dentry *dentry);
int reiserfs_symlink (struct inode * dir, struct dentry *dentry, const char * symname);
int reiserfs_link (struct dentry * old_dentry, struct inode * dir, struct dentry *dentry);
int reiserfs_rename (struct inode * old_dir, struct dentry *old_dentry, struct inode * new_dir, struct dentry *new_dentry);

/* super.c */
inline void reiserfs_mark_buffer_dirty (struct buffer_head * bh, int flag);
inline void reiserfs_mark_buffer_clean (struct buffer_head * bh);
void reiserfs_panic (struct super_block * s, const char * fmt, ...);
void reiserfs_write_super (struct super_block * s);
void reiserfs_put_super (struct super_block * s);
int reiserfs_remount (struct super_block * s, int * flags, char * data);
/*int read_super_block (struct super_block * s, int size);
int read_bitmaps (struct super_block * s);
int read_old_bitmaps (struct super_block * s);
int read_old_super_block (struct super_block * s, int size);*/
struct super_block * reiserfs_read_super (struct super_block * s, void * data, int silent);
int reiserfs_statfs (struct super_block * s, struct statfs * buf, int bufsiz);
int init_reiserfs_fs (void);

/* dir.c */
extern struct inode_operations reiserfs_dir_inode_operations;

/* file.c */
extern struct inode_operations reiserfs_file_inode_operations;
int do_direct_to_indirect (struct inode * inode);

/* symlink.c */
extern struct inode_operations reiserfs_symlink_inode_operations;

/* buffer.c */
inline int test_and_wait_on_buffer (struct buffer_head * p_s_bh);
struct buffer_head * reiserfs_getblk (kdev_t n_dev, int n_block, int n_size, int * p_n_repeat);
void reiserfs_rehash_buffer (struct buffer_head * bh, unsigned long block);
void mark_suspected_recipients_dirty (struct reiserfs_transaction_handle *th, kdev_t dev);
void fixup_reiserfs_buffers (kdev_t dev);



/* buffer2.c */
void wait_buffer_until_released (struct buffer_head * bh);
struct buffer_head * reiserfs_bread (kdev_t n_dev, int n_block, int n_size, int * p_n_repeat);
struct buffer_head * reiserfs_get_hash_table (kdev_t  n_dev, int n_block, int n_size, int * p_n_repeat);
int reiserfs_sync_file (struct file * p_s_filp, struct dentry * p_s_dentry);
void reiserfs_show_buffers (kdev_t dev);


/*#endif  __KERNEL__ */

/* fix_nodes.c */
void * reiserfs_kmalloc (size_t size, int flags, struct super_block * s);
void reiserfs_kfree (const void * vp, size_t size, struct super_block * s);
int fix_nodes (struct reiserfs_transaction_handle *th, int n_op_mode, struct tree_balance * p_s_tb, 
               int n_pos_in_item, struct item_head * p_s_ins_ih);
void unfix_nodes (struct reiserfs_transaction_handle *th, struct tree_balance *);
void free_buffers_in_tb (struct tree_balance * p_s_tb);
void init_path (struct path *);

/* prints.c */
void reiserfs_panic (struct super_block * s, const char * fmt, ...);
void reiserfs_warning (const char * fmt, ...);
void print_virtual_node (struct virtual_node * vn);
void print_indirect_item (struct buffer_head * bh, int item_num);
void print_tb (int mode, int item_pos, int pos_in_item, struct tree_balance * tb, char * mes);
void print_de (struct reiserfs_dir_entry * de);
void print_bi (struct buffer_info * bi, char * mes);
#define PRINT_LEAF_ITEMS 1   /* print all items */
#define PRINT_DIRECTORY_ITEMS 2 /* print directory items */
#define PRINT_DIRECT_ITEMS 4 /* print contents of direct items */
void print_block (struct buffer_head * bh, ...);//int print_mode, int first, int last);
void print_path (struct tree_balance * tb, struct path * path);
void print_bmap (struct super_block * s, int silent);
void print_objectid_map (struct super_block * s);
void print_block_head (struct buffer_head * bh, char * mes);
void check_leaf (struct buffer_head * bh);
void print_statistics (struct super_block * s);

/* lbalance.c */
int leaf_move_items (struct reiserfs_transaction_handle *th, int shift_mode, struct tree_balance * tb, 
                     int mov_num, int mov_bytes, struct buffer_head * Snew);
int leaf_shift_left (struct reiserfs_transaction_handle *th, struct tree_balance * tb, int shift_num, int shift_bytes);
int leaf_shift_right (struct reiserfs_transaction_handle *th, struct tree_balance * tb, int shift_num, int shift_bytes);
void leaf_delete_items (struct reiserfs_transaction_handle *th, struct buffer_info * cur_bi, 
                        int last_first, int first, int del_num, int del_bytes);
void leaf_insert_into_buf (struct reiserfs_transaction_handle *th, struct buffer_info * bi, 
			   int before, struct item_head * inserted_item_ih, const char * inserted_item_body, 
			   int mem_mode, int zeros_number);
void leaf_paste_in_buffer (struct reiserfs_transaction_handle *th, struct buffer_info * bi, int pasted_item_num, 
			   int pos_in_item, int paste_size, const char * body, int mem_mode, int zeros_number);
void leaf_cut_from_buffer (struct reiserfs_transaction_handle *th, struct buffer_info * bi, int cut_item_num, 
                           int pos_in_item, int cut_size);
void leaf_paste_entries (struct buffer_head * bh, int item_num, int before, int new_entry_count, struct reiserfs_de_head * new_dehs, const char * records,
			 int paste_size);

/*#ifdef __KERNEL__*/
/* preserve.c */
int ready_preserve_list (struct tree_balance *, struct buffer_head * bh);
void preserve_shifted (struct tree_balance *, struct buffer_head **, struct buffer_head *, int, struct buffer_head *);
void add_to_preserve (unsigned long blocknr, struct super_block * sb);
int maybe_free_preserve_list (struct super_block * sb);
int get_space_from_preserve_list (struct super_block * s);
inline void unpreserve (struct super_block * s, struct buffer_head * bh);
void preserve_invalidate (struct reiserfs_transaction_handle *,
                          struct tree_balance * tb, struct buffer_head * bh, struct buffer_head *);
unsigned long free_and_near_block (struct tree_balance * tb);
inline void mark_suspected_recipient (struct super_block * sb, struct buffer_head * bh);
inline void unmark_suspected_recipient (struct super_block * sb, struct buffer_head * bh);
int is_buffer_unwritten (struct buffer_head * bh);
int is_buffer_preserved (struct buffer_head * bh);
int is_buffer_suspected_recipient (struct super_block * s, struct buffer_head * bh);
inline void mark_buffer_unwritten (struct buffer_head * bh);
void unpreserve (struct super_block * s, struct buffer_head * bh);
#ifdef REISERFS_CHECK
void preserve_trace_init_bitmap (struct super_block * s);
void preserve_trace_release_bitmap (struct super_block * s);
void preserve_trace_reset_suspected_recipients (struct super_block * s);
#endif
/*#endif*/ /* __KERNEL__ */

/* ibalance.c */
int balance_internal (struct reiserfs_transaction_handle *th, struct tree_balance * , int, int, struct item_head * , 
                      struct buffer_head **);

/* do_balance.c */
void do_balance (struct reiserfs_transaction_handle *th, struct tree_balance * tb, int pos_in_item, 
                 struct item_head * ih, const char * body, int flag, int mem_mode, int zeros_num);
void reiserfs_invalidate_buffer (struct reiserfs_transaction_handle *th, struct tree_balance * tb, struct buffer_head * bh, int);
int get_left_neighbor_position (struct tree_balance * tb, int h);
int get_right_neighbor_position (struct tree_balance * tb, int h);
void replace_key (struct reiserfs_transaction_handle *th, struct buffer_head *, int, struct buffer_head *, int);
void replace_lkey (struct reiserfs_transaction_handle *th, struct tree_balance *, int, struct item_head *);
void replace_rkey (struct reiserfs_transaction_handle *th, struct tree_balance *, int, struct item_head *);
void make_empty_node (struct buffer_info *);
struct buffer_head * get_FEB (struct tree_balance *);

/* bitmap.c */
int is_reusable (struct super_block * s, unsigned long block, int bit_value);
void reiserfs_free_block (struct reiserfs_transaction_handle *th, struct super_block *, unsigned long);
int reiserfs_new_blocknrs (struct reiserfs_transaction_handle *th, struct super_block *, unsigned long *, unsigned long, int, int);
int reiserfs_new_unf_blocknrs (struct reiserfs_transaction_handle *th,struct super_block *,unsigned long *,unsigned long,int,int);

/* reiserfs_new_blocknrs and reiserfs_free_block use
   reiserfs_mark_buffer_dirty. To complete marking buffer dirty we use
   brelse. But first we do b_count++ instead of getblk, as bitmaps and
   super block were acquired once by getblk at mount time. */
#define COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING(s,bitmapnr) \
	SB_AP_BITMAP(s)[bitmapnr]->b_count ++;\
	brelse (SB_AP_BITMAP(s)[bitmapnr]);\
    	SB_BUFFER_WITH_SB (s)->b_count ++;\
	brelse (SB_BUFFER_WITH_SB (s));\

#define COMPLETE_BITMAP_DIRTING_AFTER_FREEING(s,bitmapnr) \
	SB_AP_BITMAP(s)[bitmapnr]->b_count ++;\
	brelse (SB_AP_BITMAP(s)[bitmapnr]);\
    	SB_BUFFER_WITH_SB (s)->b_count ++;\
	brelse (SB_BUFFER_WITH_SB (s));\

/* teahash3.c */
unsigned long keyed_hash (const char *msg, int len);

/* version.c */
char *reiserfs_get_version_string(void) ;

/* lock.c */
#ifdef __KERNEL__
inline int try_ulong_lock(unsigned long * lock, unsigned short bitnumber);
void unlock_ulong_lock(unsigned long * lock, unsigned short bitnumber, struct  wait_queue ** l_wait);
#endif


#ifdef __i386__

extern __inline__ int 
find_first_nonzero_bit(void * addr, unsigned size) {
  int res;
  int __d0;
  void *__d1;


  if (!size) {
    return (0);
  }
  __asm__ __volatile__ (
	  "cld\n\t"
	  "xorl %%eax,%%eax\n\t"
	  "repe; scasl\n\t"
	  "je 1f\n\t"
	  "movl -4(%%edi),%%eax\n\t"
	  "subl $4, %%edi\n\t"
	  "bsfl %%eax,%%eax\n\t"
	  "1:\tsubl %%edx,%%edi\n\t"
	  "shll $3,%%edi\n\t"
	  "addl %%edi,%%eax"
	  :"=a" (res),
	  "=c"(__d0), "=D"(__d1)
	  :"1" ((size + 31) >> 5), "d" (addr), "2" (addr));
  return (res);
}

#else /* __i386__ */

extern __inline__ int find_next_nonzero_bit(void * addr, unsigned size, unsigned offset)
{
	unsigned int * p = ((unsigned int *) addr) + (offset >> 5);
	unsigned int result = offset & ~31UL;
	unsigned int tmp;

	if (offset >= size)
		return size;
	size -= result;
	offset &= 31UL;
	if (offset) {
		tmp = *p++;
		/* set to zero first offset bits */
		tmp &= ~(~0UL >> (32-offset));
		if (size < 32)
			goto found_first;
		if (tmp != 0U)
			goto found_middle;
		size -= 32;
		result += 32;
	}
	while (size >= 32) {
		if ((tmp = *p++) != 0U)
			goto found_middle;
		result += 32;
		size -= 32;
	}
	if (!size)
		return result;
	tmp = *p;
found_first:
found_middle:
	return result + ffs(tmp);
}

#define find_first_nonzero_bit(addr,size) find_next_nonzero_bit((addr), (size), 0)

#endif /* 0 */


#endif /* _LINUX_REISER_FS_H */


