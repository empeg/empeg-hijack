/*
 * Copyright 1996, 1997, 1998, 1999 Hans Reiser, see reiserfs/README for licensing and copyright details
 */

/* Contains the routines related to read and write. */

/*
 * direct_to_indirect
 * get_new_buffer
 * reiserfs_file_write
 * reiserfs_file_read
*/

#ifdef __KERNEL__

#include <asm/uaccess.h>
#include <asm/system.h>

#include <linux/sched.h>

#include <linux/reiserfs_fs.h>

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/locks.h>
#include <linux/stat.h>
#include <linux/string.h>

#else

#include "nokernel.h"

#endif

static ssize_t reiserfs_file_read  (struct file *, char *, size_t, loff_t *);
static ssize_t reiserfs_file_write (struct file *, const char *, size_t, loff_t *);

int reiserfs_bmap (struct inode * inode, int block);
int reiserfs_readpage (struct file * file, struct page * page);


static struct file_operations reiserfs_file_operations = {
                NULL,        		/* lseek */
                reiserfs_file_read,     /* read */
                reiserfs_file_write,    /* write */
                NULL,                   /* readdir */
                NULL,                   /* poll */
                NULL,                   /* ioctl */
                generic_file_mmap,     	/* mmap */
                NULL,                   /* open */
		NULL,			/* flush */
                reiserfs_file_release,  /* release */
                reiserfs_sync_file,   	/* fsync */
                NULL,		        /* fasync */
                NULL, 	                /* check_media_change */
                NULL,		        /* revalidate*/
                NULL	       	        /* lock */
};


struct  inode_operations reiserfs_file_inode_operations = {
          &reiserfs_file_operations,  /* default file operations */
          NULL,                       /* create */
          NULL,                       /* lookup */
          NULL,                       /* link */
          NULL,                       /* unlink */
          NULL,                       /* symlink */
          NULL,                       /* mkdir */
          NULL,                       /* rmdir */
          NULL,                       /* mknod */
          NULL,                       /* rename */
          NULL,                       /* readlink */
	  NULL,			      /* follow_link */
          reiserfs_readpage,          /* readpage */
          NULL,		              /* writepage */
          reiserfs_bmap,              /* bmap */
          reiserfs_truncate_file,     /* truncate */
          NULL,                       /* permission */
          NULL,		              /* smap */ 
          NULL,		              /* updatepage */
          NULL		              /* revalidate */
};

/*
** this will put an updated inode in the current transaction, and
** then restart it.  Make sure you release any paths you are holding
** before calling this.
**
** th is your handle
** p_s_inode is a pointer to the inode to update
** n_pos_in_file is file_write's concept of its current spot in the file.
** 
** after running this, the inode's size will be the greater of
** p_s_inode->i_size and (n_pos_in_file -1)
*/
static void 
update_inode_and_restart_transaction(struct reiserfs_transaction_handle *th,
                               struct inode *p_s_inode,
			       unsigned long n_pos_in_file) {
  int orig_count = th->t_blocks_allocated ;
  struct super_block *s = th->t_super ;
  if ((n_pos_in_file - 1) > p_s_inode->i_size) { 
    p_s_inode->i_size = (n_pos_in_file - 1) ;
    p_s_inode->i_ctime = CURRENT_TIME ;
  }
  p_s_inode->i_mtime = CURRENT_TIME ;
  if_in_ram_update_sd(th, p_s_inode) ; 
  journal_end(th, s, orig_count) ;
  journal_begin(th, s, orig_count) ;
  reiserfs_update_inode_transaction(p_s_inode) ;
}



/* Converts direct items to an unformatted node. Returns 1 if conversion has been done, 0 if it has
   been done somewhere else, -1 if no disk space for conversion */
static int  direct_to_indirect(
	      struct reiserfs_transaction_handle *th,
              struct super_block  * p_s_sb,               /* Pointer to the super block.        */
              struct inode        * p_s_inode,            /* Pointer to the file inode.         */
              struct path         * p_s_search_path,      /* Path to the item to be converted.  */
              int                   n_item_zeros_to_add,  /* Number of zeros to be inserted
                                                              (file hole related).              */
              const char          * p_c_buf,              /* Buffer with user data being
                                                              written.                          */
              int                   n_item_bytes_to_write,/* The number of bytes to be written
                                                              from buf to the new unformatted
                                                              node.                             */
              struct buffer_head  * p_s_un_bh             /* The new unformatted node pointer.  */
            ) {
  struct key		s_key_to_search;  /* Key to search for the last byte of the converted
                                                item.                                           */
  struct item_head   * 	p_s_ih,           /* Pointer to the item header of the converted item.*/
			s_item_to_insert;
  unsigned long		n_unp;            /* Unformatted node block number.                   */
  int			n_blk_size,       /* Block size.                                      */
			n_len_to_move,    /* Length of tail to be converted.                  */
			n_pos_in_item,    /* Found position in the item                       */
			n_repeat,	  /* used for tracking whether schedule occurred */
    			n_retval;	  /* returned value for reiserfs_insert_item and clones */
  char		     *	p_c_to;	    /* End of the unformatted node.                     */
  struct unfm_nodeinfo	s_node_to_write;  /* Handle on an unformatted node that will be
                                                inserted in the tree.                           */

  p_s_sb->u.reiserfs_sb.s_direct2indirect ++;

#ifdef REISERFS_CHECK
  if ( ! p_s_un_bh )
    reiserfs_panic (p_s_sb, "PAP-14010: direct_to_indirect: pointer to the unfm buffer is NULL");
#endif

  n_blk_size = p_s_un_bh->b_size;
  n_unp = p_s_un_bh->b_blocknr;
  /* Set the key to search for the last byte of the converted item and
      key to search for append or insert pointer to the new unformatted node. */
  copy_key(&s_key_to_search,(struct key *)PATH_PITEM_HEAD(p_s_search_path));
  copy_item_head(&s_item_to_insert,PATH_PITEM_HEAD(p_s_search_path));

  s_key_to_search.k_offset += s_item_to_insert.ih_item_len - 1;
  s_item_to_insert.ih_key.k_offset -= (s_item_to_insert.ih_key.k_offset - 1) % n_blk_size;
  s_item_to_insert.ih_key.k_uniqueness = TYPE_INDIRECT;

#ifdef REISERFS_CHECK
  if ( s_item_to_insert.ih_key.k_offset != p_s_inode->u.reiserfs_i.i_first_direct_byte )
    reiserfs_panic(p_s_sb, "PAP-14020: direct_to_indirect: illegal first direct byte position");
#endif

  /* Calculate length of tail to be converted (tail may be stored in two
     direct items in different nodes). */
  n_len_to_move = s_key_to_search.k_offset % n_blk_size;
  /* Calculate address to copy from user buffer. */
  p_c_to = p_s_un_bh->b_data + n_len_to_move;
   /* let only one process append at a time */
  lock_inode_to_convert(p_s_inode);
  if (p_s_inode->u.reiserfs_i.i_first_direct_byte == NO_BYTES_IN_DIRECT_ITEM) {
    /* somewhere else mmap or bmap did this conversion */
    /*brelse(p_s_un_bh);*/
    unlock_inode_after_convert (p_s_inode);
    return 0;
  }

  if ( search_for_position_by_key (p_s_sb, (struct key *)&s_item_to_insert, p_s_search_path, &n_pos_in_item, &n_repeat) == POSITION_FOUND )
    reiserfs_panic (p_s_sb, "PAP-14030: direct_to_indirect: pasted or inserted byte exists in the tree");
  p_s_ih = PATH_PITEM_HEAD(p_s_search_path);

#ifdef REISERFS_CHECK
  if ( n_blk_size - n_len_to_move - n_item_zeros_to_add - n_item_bytes_to_write < 0 )
    reiserfs_panic (p_s_sb, "PAP-14040: direct_to_indirect: illegal ih_free_space");
#endif

  if (p_c_buf)
    s_node_to_write.unfm_freespace = n_blk_size - n_len_to_move - n_item_zeros_to_add - n_item_bytes_to_write;
  else
    s_node_to_write.unfm_freespace = n_blk_size - n_len_to_move;

  /* Insert indirect item. */
  if ( I_IS_STAT_DATA_ITEM(p_s_ih) )  {
    s_item_to_insert.u.ih_free_space = s_node_to_write.unfm_freespace;
    s_item_to_insert.ih_item_len      = UNFM_P_SIZE;
    PATH_LAST_POSITION(p_s_search_path)++;
    n_retval = reiserfs_insert_item (th, p_s_sb, p_s_search_path, &s_item_to_insert, (char *)&n_unp, REISERFS_KERNEL_MEM, 0/*zero number*/, NOTHING_SPECIAL);
  }
  /* Paste into last object indirect item. */
  else  {
    s_node_to_write.unfm_nodenum    = n_unp;
    /*s_node_to_write.unfm_freespace = n_repeat;*/
    n_retval = reiserfs_paste_into_item(th, p_s_sb, p_s_search_path, &n_pos_in_item, &(s_item_to_insert.ih_key),
					(char *)&s_node_to_write, UNFM_P_SIZE, REISERFS_KERNEL_MEM, 0);
  }
  if ( n_retval < 0 ) {
    /*brelse(p_s_un_bh);*/
    unlock_inode_after_convert(p_s_inode);
    return -1; /* Can not convert direct item. */
  }

  /* i_blocks counts only unformatted nodes */
  /*  p_s_inode->i_blocks += p_s_sb->s_blocksize / 512;*/

  /* copy data from user space to a new unformatted node */
  if (p_c_buf) {
    memset(p_c_to, '\0', n_item_zeros_to_add);
    copy_from_user(p_c_to + n_item_zeros_to_add, p_c_buf, n_item_bytes_to_write);

    update_vm_cache (p_s_inode, s_item_to_insert.ih_key.k_offset - 1 + n_len_to_move + n_item_zeros_to_add, 
		     p_c_to + n_item_zeros_to_add, n_item_bytes_to_write);

    memset(p_c_to + n_item_zeros_to_add + n_item_bytes_to_write, '\0',
	   s_node_to_write.unfm_freespace);
  } else {
    /* this works when we convert tail stored in direct item(s) to unformatted node without appending from user buffer */
    memset(p_c_to, 0, s_node_to_write.unfm_freespace);
  }
  /* non-atomic mark_buffer_dirty is allowed here */
  journal_mark_dirty_nolog(th, p_s_sb, p_s_un_bh); /* Destination buffer, preserve not needed. */

  /* Move bytes from the direct items to the new unformatted node. */
  while ( n_len_to_move )  {
    if ( search_for_position_by_key (p_s_sb, &s_key_to_search, p_s_search_path, &n_pos_in_item, &n_repeat) == POSITION_NOT_FOUND )
      reiserfs_panic (p_s_sb, "PAP-14050: direct_to_indirect: indirect item does not exist");
    n_retval = reiserfs_delete_item (th, p_s_inode, p_s_search_path, &n_pos_in_item, &s_key_to_search, 
                                     p_s_un_bh, PRESERVE_DIRECT_TO_INDIRECT);

#ifdef REISERFS_CHECK
    if ( n_retval <= 0 || n_retval > n_len_to_move)
      reiserfs_panic(p_s_sb, "PAP-14060: direct_to_indirect: illegal case");
#endif

    n_len_to_move -= n_retval;
    s_key_to_search.k_offset -= n_retval;
  }
  brelse (p_s_un_bh);

  p_s_inode->u.reiserfs_i.i_first_direct_byte = NO_BYTES_IN_DIRECT_ITEM;
  unlock_inode_after_convert(p_s_inode);
  return 1;
}


#ifdef NEW_GET_NEW_BUFFER

/* returns one buffer with a blocknr near blocknr. */
static int get_new_buffer_near_blocknr(
		   struct reiserfs_transaction_handle *th,
                   struct super_block *  p_s_sb,
                   int blocknr,
                   struct buffer_head ** pp_s_new_bh,
                   struct path         * p_s_path 
                   ) {
  unsigned      long n_new_blocknumber = 0;
  int           n_ret_value,
                n_repeat = CARRY_ON;

#ifdef REISERFS_CHECK
  int repeat_counter = 0;
  
  if (!blocknr)
    printk ("blocknr passed to get_new_buffer_near_blocknr was 0");
#endif


  if ( (n_ret_value = reiserfs_new_unf_blocknrs (th, p_s_sb, &n_new_blocknumber,
                                             blocknr, 1)) == NO_DISK_SPACE )
    return NO_DISK_SPACE;
  
  *pp_s_new_bh = reiserfs_getblk(p_s_sb->s_dev, n_new_blocknumber, p_s_sb->s_blocksize, &n_repeat);
  if ( buffer_uptodate(*pp_s_new_bh) ) {

#ifdef REISERFS_CHECK
    if ( buffer_dirty(*pp_s_new_bh) || (*pp_s_new_bh)->b_dev == NODEV ) {
      reiserfs_panic(p_s_sb, "PAP-14080: get_new_buffer: invalid uptodate buffer %b for the new block", *pp_s_new_bh);
    }
#endif

    /* Free path buffers to prevent deadlock. */
    /* It is possible that this process has the buffer, which this function is getting, already in
       its path, and is responsible for double incrementing the value of b_count.  If we recalculate
       the path after schedule we can avoid risking an endless loop.  This problematic situation is
       possible in a multiple processing environment.  Suppose process 1 has acquired a path P; then
       process 2 balanced and remove block A from the tree.  Process 1 continues and runs
       get_new_buffer, that returns buffer with block A. If node A was on the path P, then it will
       have b_count == 2. If we now will simply wait in while ( (*pp_s_new_bh)->b_count > 1 ) we get
       into an endless loop, as nobody will release this buffer and the current process holds buffer
       twice. That is why we do decrement_counters_in_path(p_s_path) before waiting until b_count
       becomes 1. (it there were other processes holding node A, then eventually we will get a
       moment, when all of them released a buffer). */
    if ( (*pp_s_new_bh)->b_count > 1  ) {
      decrement_counters_in_path(p_s_path);
      n_ret_value |= SCHEDULE_OCCURRED;
    }

    while ( (*pp_s_new_bh)->b_count > 1 ) {

#ifdef REISERFS_INFO
      printk("get_new_buffer() calls schedule to decrement b_count\n");
#endif

#ifdef REISERFS_CHECK
      if ( ! (++repeat_counter % 10000) )
	printk("get_new_buffer(%u): counter(%d) too big", current->pid, repeat_counter);
#endif

      current->policy |= SCHED_YIELD;
      schedule();
    }

#ifdef REISERFS_CHECK
    if ( buffer_dirty(*pp_s_new_bh) || (*pp_s_new_bh)->b_dev == NODEV ) {
      print_buffer_head(*pp_s_new_bh,"get_new_buffer");
      reiserfs_panic(p_s_sb, "PAP-14090: get_new_buffer: invalid uptodate buffer %b for the new block(case 2)", *pp_s_new_bh);
    }
#endif

  }
  else {
    ;

#ifdef REISERFS_CHECK
    /* journal victim */
    if ( 0 && (*pp_s_new_bh)->b_count != 1 && !buffer_journaled(*pp_s_new_bh)) {
      reiserfs_panic(p_s_sb,"PAP-14100: get_new_buffer: not uptodate buffer %b for the new block has b_count more than one",
		     *pp_s_new_bh);
    }
#endif

  }
  /* marking buffer here gives it the chance to die in the journal if we free it before commit */
  if (*pp_s_new_bh) {
    mark_buffer_journal_new(*pp_s_new_bh) ;
  }
  return (n_ret_value | n_repeat);
}


/* returns the block number of the last unformatted node, assumes p_s_key_to_search.k_offset is a byte in the tail of
   the file, Useful for when you want to append to a file, and convert a direct item into an unformatted node near the
   last unformatted node of the file.  Putting the unformatted node near the direct item is potentially very bad to do.
   If there is no unformatted node in the file, then we return the block number of the direct item.  */
inline int get_last_unformatted_node_blocknr_of_file(  struct key * p_s_key_to_search, struct super_block * p_s_sb,
                                                       struct buffer_head * p_s_bh, int * p_n_repeat, 
                                                       struct path * p_unf_search_path, struct inode * p_s_inode)

{
  struct key unf_key_to_search;
  struct item_head * p_s_ih;
  int n_pos_in_item;
  struct buffer_head * p_indirect_item_bh;

      copy_key(&unf_key_to_search,p_s_key_to_search);
      unf_key_to_search.k_uniqueness = TYPE_INDIRECT;
      unf_key_to_search.k_offset = p_s_inode->u.reiserfs_i.i_first_direct_byte -1;

        /* p_s_key_to_search->k_offset -  MAX_ITEM_LEN(p_s_sb->s_blocksize); */
      if (search_for_position_by_key (p_s_sb, &unf_key_to_search, p_unf_search_path, &n_pos_in_item, p_n_repeat) == POSITION_FOUND)
        {
          p_s_ih = B_N_PITEM_HEAD(p_indirect_item_bh = PATH_PLAST_BUFFER(p_unf_search_path), PATH_LAST_POSITION(p_unf_search_path));
          return (B_I_POS_UNFM_POINTER(p_indirect_item_bh, p_s_ih, n_pos_in_item));
        }
     /*  else */
      printk("reiser-1800: search for unformatted node failed, p_s_key_to_search->k_offset = %u,  unf_key_to_search.k_offset = %u, MAX_ITEM_LEN(p_s_sb->s_blocksize) = %ld, debug this\n", p_s_key_to_search->k_offset, unf_key_to_search.k_offset,  MAX_ITEM_LEN(p_s_sb->s_blocksize) );
      print_buffer_head(PATH_PLAST_BUFFER(p_unf_search_path), "the buffer holding the item before the key we failed to find");
      print_block_head(PATH_PLAST_BUFFER(p_unf_search_path), "the block head");
      return 0;                         /* keeps the compiler quiet */
}


                                /* hasn't been out of disk space tested  */
static int get_buffer_near_last_unf (struct reiserfs_transaction_handle *th,
				     struct super_block * p_s_sb, struct key * p_s_key_to_search,
                                                 struct inode *  p_s_inode,  struct buffer_head * p_s_bh, 
                                                 struct buffer_head ** pp_s_un_bh, struct path * p_s_search_path)
{
  int unf_blocknr = 0, /* blocknr from which we start search for a free block for an unformatted node, if 0
                          then we didn't find an unformatted node though we might have found a file hole */
    n_repeat = CARRY_ON;        /* did schedule occur?  if so, then don't carry on, and recalc things */
  struct key unf_key_to_search;
  struct path unf_search_path;

  copy_key(&unf_key_to_search,p_s_key_to_search);
  unf_key_to_search.k_uniqueness = TYPE_INDIRECT;
  
  if (
      (p_s_inode->u.reiserfs_i.i_first_direct_byte > 4095) /* i_first_direct_byte gets used for all sorts of
                                                              crap other than what the name indicates, thus
                                                              testing to see if it is 0 is not enough */
      && (p_s_inode->u.reiserfs_i.i_first_direct_byte < MAX_KEY_OFFSET) /* if there is no direct item then
                                                                           i_first_direct_byte = MAX_KEY_OFFSET */
      )
    {
                                /* actually, we don't want the last unformatted node, we want the last unformatted node
                                   which is before the current file offset */
      unf_key_to_search.k_offset = ((p_s_inode->u.reiserfs_i.i_first_direct_byte -1) < unf_key_to_search.k_offset) ? p_s_inode->u.reiserfs_i.i_first_direct_byte -1 :  unf_key_to_search.k_offset;

      while (unf_key_to_search.k_offset > -1)
        {
                                /* This is our poorly documented way of initializing paths. -Hans */
          init_path (&unf_search_path);
                                /* get the blocknr from which we start the search for a free block. */
          unf_blocknr = get_last_unformatted_node_blocknr_of_file(  p_s_key_to_search, /* assumes this points to the file tail */
                                                                    p_s_sb,     /* lets us figure out the block size */
                                                                    p_s_bh, /* if there is no unformatted node in the file,
                                                                               then it returns p_s_bh->b_blocknr */
                                                                    &n_repeat, /* tells us if schedule occurred */
                                                                    &unf_search_path,
                                                                    p_s_inode
                                                                    );
/*        printk("in while loop: unf_blocknr = %d,  *pp_s_un_bh = %p\n", unf_blocknr, *pp_s_un_bh); */
          if (unf_blocknr) 
            break;
          else                  /* release the path and search again, this could be really slow for huge
                                   holes.....better to spend the coding time adding compression though.... -Hans */
            {
                                /* Vladimir, is it a problem that I don't brelse these buffers ?-Hans */
              decrement_counters_in_path(&unf_search_path);
              unf_key_to_search.k_offset -= 4096;
            }
        }
      if (unf_blocknr) {
        n_repeat |= get_new_buffer_near_blocknr(th, p_s_sb, unf_blocknr, pp_s_un_bh, p_s_search_path);
      }
      else {                    /* all unformatted nodes are holes */
        n_repeat |= get_new_buffer_near_blocknr(th, p_s_sb, p_s_bh->b_blocknr, pp_s_un_bh, p_s_search_path); 
      }
    }
  else {                        /* file has no unformatted nodes */
    n_repeat |= get_new_buffer_near_blocknr(th, p_s_sb, p_s_bh->b_blocknr, pp_s_un_bh, p_s_search_path);
/*     printk("in else: unf_blocknr = %d,  *pp_s_un_bh = %p\n", unf_blocknr, *pp_s_un_bh); */
/*     print_path (0,  p_s_search_path); */
  }

  return n_repeat;
}

#endif /* NEW_GET_NEW_BUFFER */


#ifdef OLD_GET_NEW_BUFFER
/*
** BIG CHANGE HERE!  If this fails to get a block, it will update the inode
** release the path, stop the transaction, start a new transaction, and
** try again.
*/
static int get_new_buffer(
		   struct reiserfs_transaction_handle *th,
		   struct super_block *	 p_s_sb,
		   struct buffer_head *  p_s_bh,
		   struct buffer_head ** pp_s_new_bh,
		   struct path	       * p_s_path,
		   struct inode        * p_s_inode,     /* for update of inode*/
		   unsigned long         n_pos_in_file /* from file_write */
		   ) {
  unsigned	long n_new_blocknumber = 0;
  int		n_repeat, n_repeat1, disk_full_check;


  if ( (n_repeat = reiserfs_new_unf_blocknrs (th, p_s_sb, &n_new_blocknumber, p_s_bh->b_blocknr, 1, 0/*not for preserve list*/)) == NO_DISK_SPACE ) {

    /* We might not be out of space.  The journal might be able to reclaim
    ** some, but not while our transaction is open.  So, we update our inode
    ** and restart the transaction hoping there will be more space when we
    ** get back.  
    */
    n_repeat |= SCHEDULE_OCCURRED;
    decrement_counters_in_path(p_s_path);
    update_inode_and_restart_transaction(th, p_s_inode, n_pos_in_file) ;
    if ( (disk_full_check = reiserfs_new_unf_blocknrs (th, p_s_sb, 
                                                       &n_new_blocknumber, 
						       p_s_bh->b_blocknr, 
						       1, 
						       0)) == NO_DISK_SPACE ) {

      return NO_DISK_SPACE;
    }
  } 
  n_repeat1 = CARRY_ON;
  *pp_s_new_bh = reiserfs_getblk(p_s_sb->s_dev, n_new_blocknumber, p_s_sb->s_blocksize, &n_repeat1);
  n_repeat |= n_repeat1;
  if ((*pp_s_new_bh)->b_count > 1) {
    /* Free path buffers to prevent deadlock which can occur in the
       situation like : this process holds p_s_path; Block
       (*pp_s_new_bh)->b_blocknr is on the path p_s_path, but it is
       not necessary, that *pp_s_new_bh is in the tree; process 2
       could remove it from the tree and freed block
       (*pp_s_new_bh)->b_blocknr. Reiserfs_new_blocknrs in above
       returns block (*pp_s_new_bh)->b_blocknr. Reiserfs_getblk gets
       buffer for it, and it has b_count > 1. If we now will simply
       wait in while ( (*pp_s_new_bh)->b_count > 1 ) we get into an
       endless loop, as nobody will release this buffer and the
       current process holds buffer twice. That is why we do
       decrement_counters_in_path(p_s_path) before waiting until
       b_count becomes 1. (it there were other processes holding node
       pp_s_new_bh, then eventually we will get a moment, when all of
       them released a buffer). */
    decrement_counters_in_path(p_s_path);
    n_repeat |= SCHEDULE_OCCURRED;
    wait_buffer_until_released (*pp_s_new_bh);
  }

#ifdef REISERFS_CHECK
  if (0 && !buffer_journaled(*pp_s_new_bh) && ((*pp_s_new_bh)->b_count != 1 || buffer_dirty (*pp_s_new_bh))) {
    reiserfs_panic(p_s_sb,"PAP-14100: get_new_buffer: not free or dirty buffer %b for the new block",
		   *pp_s_new_bh);
  }
#endif
  if (*pp_s_new_bh) {
    mark_buffer_journal_new(*pp_s_new_bh) ;
  }

  return n_repeat;
}

#endif /* OLD_GET_NEW_BUFFER */


#ifdef GET_MANY_BLOCKNRS
                                /* code not yet functional */
get_next_blocknr (
                  unsigned long *       p_blocknr_array,          /* we get a whole bunch of blocknrs all at once for
                                                                     the write.  This is better than getting them one at
                                                                     a time.  */
                  unsigned long **      p_blocknr_index,        /* pointer to current offset into the array. */
                  unsigned long        blocknr_array_length
)
{
  unsigned long return_value;

  if (*p_blocknr_index < p_blocknr_array + blocknr_array_length) {
    return_value = **p_blocknr_index;
    **p_blocknr_index = 0;
    *p_blocknr_index++;
    return (return_value);
  }
  else
    {
      kfree (p_blocknr_array);
    }
}
#endif /* GET_MANY_BLOCKNRS */



/* Summary:

We have a file consisting of some number of indirect items and direct
items.  We translate filp->f_pos into where we are supposed to start
the write in terms of the tree data structures.  We then write one
node at a time, decrementing the count of bytes that we are to write,
after we write each node, by the appropriate amount.  We loop until
count is zero.  We have a complex if statement that handles each
possible case in writing a node, taking into account holes,
conversions from direct to indirect, overwrites, etc.  This if
statement is horrible, and should be rewritten. -Hans

reiserfs_paste_into_item, direct_to_indirect, and reiserfs_insert_into_item perform
all balancing that results from the write.

We return either the number of bytes successfully written or an error.
Note that if we successfully write some bytes and have an error, we
return the number of bytes written, not the error.  This allows the
user to record that the bytes were written, retry, and then get the
errorid on the retry, or if the error goes away, to never know.  Note
that ENOSPC errors are complicated by the preserve list (see
preserve.c). - Hans

*/

/*

We don't need to worry about schedule for direct items because they
search for one buffer and use it.  Schedule is a problem only when
searching for two things, and the second search invalidates the first.
Insertion of direct items is also not a concern, for the same reason.
It is only {insertion of a pointer into an indirect item coupled with
getting a buffer to hold the unformatted node, and converting direct
to indirect items} that require that upon schedule we decide whether
to continue, free resources gotten, and retry.

When schedule occurs, we look at the indirect item that we found, and
make sure it is still there.  If something else is there that has the
same string of bytes as the item we are looking for, we will have a
bug?  This can happen for preserved nodes?

*/

/* Write n_count bytes from user buffer p_c_buf into a system buffer. */
static ssize_t reiserfs_file_write(
                                   struct file   * p_s_filp, /* current position in the file, man write() for details. */
                                   const char    * p_c_buf,  /* The user space buffer from which we copy. */
                                   size_t          n_count,   /* The number of bytes to write: once the function
                                                                 starts up, count is the number of bytes remaining
                                                                 to be written. */
                                   loff_t        * p_n_pos
            ) {
  struct inode * p_s_inode = p_s_filp->f_dentry->d_inode;
  struct super_block  * p_s_sb;
  struct key            s_key_to_search;      	/* Key of item we are searching for that we are
                                                  going to write into. */
  /*loff_t*/unsigned long	        n_pos_in_file;        	/* Position (in bytes from beginning of object)
                                                  where you must write. */
  unsigned long         n_unp,                  /* Unformatted node number. */
                        n_append_startpoint;    /* Offset of the byte after the last byte in the file. */
  struct path           s_search_path;        /* path from root of tree to some leaf, we keep this
						 data around so that it is faster to go up and down
						 the tree */
  ssize_t               n_written;              /* Bytes written, or errorid. */
  size_t        n_orig_count = n_count ;           /* original number of bytes wanted */
  int                   n_pos_in_item,
                        n_search_res,
                        n_pos_in_buffer,
                        n_item_bytes_to_write,  /* The number of bytes from the user buffer to be
			                                              written in this pass of the loop to the
                                                    current item. */
                        n_item_zeros_to_add,    /* Number of zeros to be added to this item.  */
                        n_repeat,	              /* If schedule occured. */
                        n_item_size,	          /* Number of bytes in the current item. */
                        n_offset_in_node,       /* Write offset in the unformatted node. */
                        n_zero_bytes,           /* Number of zero bytes which must be created
                                                    (if we write past the end of the file). */
                        n_blk_size,             /* Block size in bytes. */
                        n_bytes_in_item,        /* Item bytes number. */
                        n_cur_len,              /* Length of current item. */
                        retval,                 /* What direct_to_indirect returns */
                        n_max_squeeze_in;       /* Maximum amount that could be added to the direct item
                                                    and still fit into it or an unformatted node that it
                                                    is converted into */
  struct buffer_head  * p_s_bh,                 /* Buffer contains found item. */
                      * p_s_un_bh;              /* Buffer contains unformatted node. */
  struct item_head      s_ih,
                        s_item_to_insert,       /* Header of mythical item: it contains the key to
                                                    search for. */
                      * p_s_ih;                 /* Found item header. */
  struct unfm_nodeinfo	s_node_to_write;      /* Handle on an unformatted node that will be passed
                                                    to do_balance the last byte of the file. */
#ifdef GET_MANY_BLOCKNRS
  unsigned long *       p_blocknr_array,          /* we get a whole bunch of blocknrs all at once for the write.  This is
                                                     better than getting them one at a time.  */
                        p_blocknr_index;        /* current offset into the array. */
  unsigned long        blocknr_array_length;

#endif /* GET_MANY_BLOCKNRS */
  char * local_buf = 0;
  int j;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3 ;
  int windex ;
  struct reiserfs_transaction_handle th ;
  unsigned long	limit = current->rlim[RLIMIT_FSIZE].rlim_cur;
  unsigned long pos = *p_n_pos;
  
/*
  if (!p_s_inode->i_op || !p_s_inode->i_op->updatepage)
      return -EIO;
  */
  if (p_s_filp->f_error) {
      int error = p_s_filp->f_error;
      p_s_filp->f_error = 0;
      return error;
  }

  /* Calculate position in the file. */
  if ( p_s_filp->f_flags & O_APPEND ) {
    n_pos_in_file = p_s_inode->i_size + 1;
    pos = p_s_inode->i_size;
  } else
    n_pos_in_file = *p_n_pos + 1;

  if (pos >= limit)
      return -EFBIG;

  if (n_count > limit - pos)
      n_count = limit - pos;

  n_written = 0;
  p_s_sb = p_s_inode->i_sb;

  journal_begin(&th, p_s_sb, jbegin_count) ;
  windex = push_journal_writer("file-write") ;

  reiserfs_update_inode_transaction(p_s_inode) ;

  /* Set key_to_search to search for first item of object */
  copy_key(&s_key_to_search, INODE_PKEY (p_s_inode));


  /*  printk ("File_write: objectid = %lu offset = %lu, count = %lu\n", s_key_to_search.k_objectid, n_pos_in_file - 1, n_count);*/

  /* Set key_to_search to search for item we write into */
  s_key_to_search.k_offset = n_pos_in_file;
  if ( n_pos_in_file >= p_s_inode->u.reiserfs_i.i_first_direct_byte )
    s_key_to_search.k_uniqueness = TYPE_DIRECT;
  else
    s_key_to_search.k_uniqueness = TYPE_INDIRECT;

  n_written = 0;
  n_blk_size = p_s_sb->s_blocksize;
  p_s_un_bh = NULL;
  init_path (&s_search_path);

  local_buf = reiserfs_kmalloc (n_blk_size, GFP_KERNEL, p_s_sb);
  if (local_buf == 0) {
    pop_journal_writer(windex) ;
    journal_end(&th, p_s_sb, jbegin_count) ;
    return -ENOMEM;
  }

  while ( n_count ) {
    if (current->need_resched)
      schedule();


    /* Search for the item which contains 'n_pos_in_file-th' object byte.  If we are writing past the
       end of the file then item_num and bh are set to the last item in the file.
       Note: the repeat of this expensive search plus do_balance for every iteration
       of this loop is a performance loss source for large files.  We should calculate how
       many unformatted nodes we are going to add and insert their
       pointers into the indirect item all at once. Though, if we used extents.... -Hans */
    n_search_res = search_for_position_by_key (p_s_sb, &s_key_to_search, &s_search_path, &n_pos_in_item, &n_repeat);

    /* The item which contains 'pos-th' object byte.  If we are writing past the end
       of the file p_s_ih is set to the last item in the file. */
    p_s_ih = B_N_PITEM_HEAD(p_s_bh = PATH_PLAST_BUFFER(&s_search_path),
			    n_pos_in_buffer = PATH_LAST_POSITION(&s_search_path));
    /* Remember the found item header. */
    copy_item_head(&s_ih, p_s_ih);

    /* Item which contains 'n_pos_in_file-th' byte is found. */
    if ( n_search_res == POSITION_FOUND )  {
      n_item_zeros_to_add = 0;
      if ( I_IS_DIRECT_ITEM(p_s_ih) ) {
	n_item_bytes_to_write = p_s_ih->ih_item_len - n_pos_in_item;
	if ( n_item_bytes_to_write > n_count )
	  n_item_bytes_to_write = n_count;
	/* copy data from user space to intermediate buffer */
	copy_from_user (local_buf, p_c_buf, n_item_bytes_to_write);

	/* make sure, that direct item is still on its old place */
	if (comp_items (&s_ih, &s_search_path)) {
	  printk ("reiserfs_file_write: item has been moved while we were in copy_from_user (overwriting the direct item)\n");
	  continue;
	}
	/* Overwrite to end of this direct item or end of count bytes from user buffer to direct item. */
	memcpy (B_I_PITEM(p_s_bh, p_s_ih) + n_pos_in_item, local_buf, n_item_bytes_to_write);
	/* mark buffer dirty atomically. It will be refiled in pathrelse */
	journal_mark_dirty(&th, p_s_sb, p_s_bh) ;

	update_vm_cache(p_s_inode, n_pos_in_file - 1,
			B_I_PITEM(p_s_bh, p_s_ih) + n_pos_in_item, n_item_bytes_to_write);

      }
      else  { /* Indirect item. */
	/* Set n_unp to blocknr containing the byte we start the write at. */
        n_unp = B_I_POS_UNFM_POINTER(p_s_bh, p_s_ih, n_pos_in_item);
	/* The number of bytes in the unformatted node that are to be overwritten
	   (calculation adjusts for the case in which not all bytes are used in the
	   unformatted node). */
	n_item_bytes_to_write = (n_item_size = I_POS_UNFM_SIZE(p_s_ih, n_pos_in_item, n_blk_size))
	  			- ((int)n_pos_in_file - 1) % n_blk_size;
	if ( n_item_bytes_to_write > n_count )
	  n_item_bytes_to_write = n_count;

#ifdef REISERFS_CHECK
	if ( n_item_bytes_to_write <= 0 ) {
	  printk("n_item_size = %d n_pos_in_file = %lu\n", n_item_size, n_pos_in_file);
	  reiserfs_panic(p_s_sb, "PAP-14110: reiserfs_file_write: n_item_bytes_to_write <= 0");
	}
#endif

	/* If not a hole in the file. */
	if ( n_unp )  {
	  /* Get the block we are to write into. */
	  n_repeat = CARRY_ON;
          p_s_un_bh = reiserfs_getblk(p_s_bh->b_dev, n_unp, n_blk_size, &n_repeat);

	  /* If we are not overwriting the entire node and we have not yet read the buffer from disk
	     then we must read the block into memory before writing. */
	  if ( n_item_bytes_to_write != n_item_size && ! buffer_uptodate(p_s_un_bh) ) {
	    ll_rw_block(READ, 1, &p_s_un_bh);
	    wait_on_buffer(p_s_un_bh);
	    /* Check for I/O error. */
	    if ( ! buffer_uptodate(p_s_un_bh) ) {
	      brelse(p_s_un_bh);
              p_s_un_bh = NULL;
	      pathrelse(&s_search_path);
	      if ( ! n_written )  {
		printk ("reiserfs_file_write() returned EIO1\n");
		n_written = -EIO;
	      }
	      break;
	    }
          }

	  /* Do the write. */
	  copy_from_user(p_s_un_bh->b_data + ((int)n_pos_in_file - 1) % n_blk_size,
                                                        p_c_buf, n_item_bytes_to_write);
	  update_vm_cache(p_s_inode, n_pos_in_file - 1,
			  p_s_un_bh->b_data + ((int)n_pos_in_file - 1) % n_blk_size, n_item_bytes_to_write);
	  mark_buffer_uptodate (p_s_un_bh, 1);
	  /* non-atomic mark_buffer_dirty is allowed here */
	  /* mark_buffer_dirty(p_s_un_bh, 0);  */
	  journal_mark_dirty_nolog(&th, p_s_sb, p_s_un_bh) ;
	  brelse (p_s_un_bh);
	  p_s_un_bh = NULL;

        }
	else  { /* If writing to a hole. */
          /* Get a new buffer for the unformatted node. */
          if ( ! p_s_un_bh )  {
#ifdef OLD_GET_NEW_BUFFER
	    n_repeat = get_new_buffer(&th, p_s_sb, p_s_bh, &p_s_un_bh, &s_search_path, p_s_inode, n_pos_in_file);
#else
	    /* if there is an unformatted node in the file, then get our unformatted node from near
	       the last one of them else get it from near the direct item.  */
	    n_repeat = get_buffer_near_last_unf ( &th, p_s_sb, &s_key_to_search,
						  p_s_inode, p_s_bh, 
						  &p_s_un_bh, &s_search_path);
#endif /* NEW_GET_NEW_BUFFER */
						  
            if ( ! p_s_un_bh )  {
              /* No disk space for new block. */
              if ( ! n_written )
                n_written = -ENOSPC;
	      pathrelse(&s_search_path);
	      break;
            }
	    memset (p_s_un_bh->b_data, 0, n_blk_size);

	    if ( n_repeat != CARRY_ON && (s_search_path.path_length == ILLEGAL_PATH_ELEMENT_OFFSET || 
					  comp_items(&s_ih, &s_search_path)) ) {
	      /* other processes are allowed */
	      j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
	      COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING (p_s_sb, j);
	      continue;
	    }
          }

#ifdef REISERFS_CHECK
	  if (B_I_POS_UNFM_POINTER (p_s_bh, p_s_ih, n_pos_in_item) != 0)
	    reiserfs_panic (p_s_sb, "vs-14120: reiserfs_file_write: unformatted node pointer %lu, must 0",
			    B_I_POS_UNFM_POINTER(p_s_bh,p_s_ih, n_pos_in_item));
#endif
	  /* Initialize pointer to the unformatted node in the its parent. */
	  B_I_POS_UNFM_POINTER(p_s_bh,p_s_ih, n_pos_in_item) = p_s_un_bh->b_blocknr;
          /* reiserfs_mark_buffer_dirty(p_s_bh, 1); journal victim */
          journal_mark_dirty(&th, p_s_sb, p_s_bh); 

	  /* Put zeros before the place where the write starts. */
	  n_offset_in_node = ((int)n_pos_in_file - 1) % n_blk_size;
	  //memset(p_s_un_bh->b_data, '\0', n_offset_in_node = ((int)n_pos_in_file - 1) % n_blk_size);
	  /* Do the write at the appropriate spot. */
	  copy_from_user(p_s_un_bh->b_data + n_offset_in_node, p_c_buf, n_item_bytes_to_write);

	  update_vm_cache(p_s_inode, n_pos_in_file - 1, 
			  p_s_un_bh->b_data + n_offset_in_node, n_item_bytes_to_write);

	  /* it is not a hole now */
	  p_s_inode->i_blocks += p_s_sb->s_blocksize / 512;

	  /* Put zeros after the place where the write finishes. */
	  //memset(p_s_un_bh->b_data + n_offset_in_node + n_item_bytes_to_write, '\0',
	  //	 n_blk_size - n_offset_in_node - n_item_bytes_to_write);

	  mark_buffer_uptodate(p_s_un_bh, 1);
	  /* non-atomic mark_buffer_dirty is allowed here */
	  /* mark_buffer_dirty(p_s_un_bh, 0);   */
	  journal_mark_dirty_nolog(&th, p_s_sb, p_s_un_bh) ;

	  /* compelte what was not finished in reiserfs_new_blocknrs */
	  j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
	  COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING (p_s_sb, j);

	  brelse (p_s_un_bh);
	  p_s_un_bh = NULL;
        }	/* replace hole with unformatted node */
      }		/* overwrite indirect item */
      pathrelse(&s_search_path);
    } 		/* position we are going to write to is found */

    /* Item containing n_pos_in_file-th byte not found (writing past current end of file). */
    else  {
      /* Form item key to insert. */
      copy_key(&(s_item_to_insert.ih_key),&s_key_to_search);
      /* We calculate offset of the byte after the last byte in the file. */
      n_append_startpoint = ( I_IS_STAT_DATA_ITEM(p_s_ih) ) ? 1 :
				p_s_ih->ih_key.k_offset + I_BYTES_NUMBER(p_s_ih, n_blk_size);
      /* Calculate number of zero bytes which must be created. */

      n_zero_bytes = n_pos_in_file - n_append_startpoint;

#ifdef REISERFS_CHECK
      if ( COMP_SHORT_KEYS(&(p_s_ih->ih_key), &s_key_to_search) || n_zero_bytes < 0 ) {
	printk("n_pos_in_file  = %lu n_append_startpoint = %lu\n",n_pos_in_file, n_append_startpoint);
        reiserfs_panic(p_s_sb, "PAP-14130: reiserfs_file_write: item found %h, key to search %k", p_s_ih, &s_key_to_search);
      }
#endif
#ifdef GET_MANY_BLOCKNRS
                                /* Allocate space for blocknr_array if we don't have one. */
      if (!p_blocknr_array) {
        /* Calculate maximum amount that could be added to the direct item and still
             fit into it or an unformatted node that it is converted into */
        n_max_squeeze_in = n_blk_size - (n_append_startpoint - 1) % n_blk_size;
        blocknr_array_length = (count > n_max_squeeze_in) ?((count - n_max_squeeze_in) / n_blk_size): 0;
        if (blocknr_array_length) {
          p_blocknr_array = kmalloc (sizeof(unsigned long) * blocknr_array_length, GFP_ATOMIC);
          if (!p_blocknr_array) {
            reiserfs_panic("reiserfs-1830: reiserfs_file_write: kmalloc failed");
	  }
        }
      }
#endif /*  GET_MANY_BLOCKNRS */

      /* Last file item is the stat_data. */
      if ( I_IS_STAT_DATA_ITEM(p_s_ih) )  {

        /* Calculate zeros to insert before write into unformatted node. */
	n_item_zeros_to_add = ( n_zero_bytes < n_blk_size ) ? n_zero_bytes : n_blk_size;
	/* Calculate the amount to write into unformatted node. */
	n_item_bytes_to_write = ( n_item_zeros_to_add < n_blk_size ) ?
	  ( n_blk_size - n_item_zeros_to_add) : 0;
	if ( n_item_bytes_to_write > n_count )
          n_item_bytes_to_write = n_count;

	s_item_to_insert.ih_key.k_offset = n_append_startpoint;
	/* Insert pointer to the unformatted node. */
	if ( dont_have_tails (p_s_sb) ||
	     (n_item_zeros_to_add + n_item_bytes_to_write > MAX_DIRECT_ITEM_LEN(n_blk_size)) ||
	     (n_orig_count >= MIN_PACK_ON_CLOSE) ||
	     STORE_TAIL_IN_UNFM (0, n_item_zeros_to_add + n_item_bytes_to_write, n_blk_size) )  {

	  if (!STORE_TAIL_IN_UNFM (0, n_item_zeros_to_add + n_item_bytes_to_write, n_blk_size)) {
	    p_s_inode->u.reiserfs_i.i_pack_on_close = 1 ;
	  } else {
	    p_s_inode->u.reiserfs_i.i_pack_on_close = 0 ;
	  }
          if ( n_item_bytes_to_write &&  ! p_s_un_bh ) {
#ifdef OLD_GET_NEW_BUFFER
            /* Get new buffer for an unformatted node. */
            n_repeat = get_new_buffer(&th, p_s_sb, p_s_bh, &p_s_un_bh, &s_search_path, p_s_inode, n_pos_in_file);
#else
	    /* if there is an unformatted node in the file, then get
	       our unformatted node from near the last one of them
	       else get it from near the direct item.  */
	    n_repeat = get_buffer_near_last_unf (&th,  p_s_sb, &s_key_to_search,
						  p_s_inode, p_s_bh,
						  &p_s_un_bh, &s_search_path);
#endif /* NEW_GET_NEW_BUFFER */
	    if ( ! p_s_un_bh )  {
	      /* No disk space for new block. */
	      if ( ! n_written )
                n_written = -ENOSPC;
	      pathrelse(&s_search_path);
	      break;
            }
	    memset (p_s_un_bh->b_data, 0, n_blk_size);
	    if ( n_repeat != CARRY_ON && (s_search_path.path_length == ILLEGAL_PATH_ELEMENT_OFFSET || comp_items(&s_ih, &s_search_path)) ) {
	      /* compelte what was not finished in reiserfs_new_blocknrs */
	      j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
	      COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING (p_s_sb, j);
	      continue;
	    }
          }
	  n_unp = p_s_un_bh ? p_s_un_bh->b_blocknr : 0;

          /* Form indirect item header. */
	  s_item_to_insert.ih_key.k_uniqueness = TYPE_INDIRECT;
	  s_item_to_insert.u.ih_free_space = n_blk_size - n_item_zeros_to_add - n_item_bytes_to_write;
	  s_item_to_insert.ih_item_len = UNFM_P_SIZE;
          /* reiserfs_insert_item() inserts before given position in the node, so we must
	     increment to point to the next item after searched one. */
	  PATH_LAST_POSITION(&s_search_path)++;
          /* Insert indirect item. */
	  if ( reiserfs_insert_item (&th, p_s_sb, &s_search_path, &s_item_to_insert, (char *)&n_unp, REISERFS_KERNEL_MEM, 0, NOTHING_SPECIAL) < 0 )  {
            if ( ! n_written )
	      n_written = -ENOSPC;
	    if ( p_s_un_bh ) {
	      reiserfs_free_block (&th, p_s_sb, p_s_un_bh->b_blocknr);

	      j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
	      COMPLETE_BITMAP_DIRTING_AFTER_FREEING(p_s_sb,j);

              bforget(p_s_un_bh);
	      p_s_un_bh = NULL;
	    }
	    break; /* No disk space. */
          }
          if ( p_s_un_bh ) {
	    /* pointer to an unformatted node is in tree. Copy data */
            //memset(p_s_un_bh->b_data, '\0', n_item_zeros_to_add);
	    copy_from_user(p_s_un_bh->b_data + n_item_zeros_to_add, p_c_buf, n_item_bytes_to_write);
	    //memset(p_s_un_bh->b_data + n_item_zeros_to_add + n_item_bytes_to_write, '\0',
	    //   n_blk_size - n_item_zeros_to_add - n_item_bytes_to_write);

	    update_vm_cache(p_s_inode, n_pos_in_file - 1,
			    p_s_un_bh->b_data + n_item_zeros_to_add, n_item_bytes_to_write);
            mark_buffer_uptodate(p_s_un_bh, 1);
	    /* non-atomic mark_buffer_dirty is allowed here */
            /* mark_buffer_dirty(p_s_un_bh, 0); journal victim */
	    journal_mark_dirty_nolog(&th, p_s_sb, p_s_un_bh) ;

	    /* compelte what was not finished in reiserfs_new_blocknrs */
	    j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
	    COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING (p_s_sb, j);
	    
            brelse(p_s_un_bh);
            p_s_un_bh = NULL;

	    /* i_blocks counts only unformatted nodes */
	    p_s_inode->i_blocks += p_s_sb->s_blocksize / 512;
          }
        }
        /* Insert direct item. */
        else  {
	  p_s_inode->u.reiserfs_i.i_pack_on_close = 0 ;

#ifdef REISERFS_CHECK
	  if ( p_s_inode->u.reiserfs_i.i_first_direct_byte != NO_BYTES_IN_DIRECT_ITEM )
	    reiserfs_panic(p_s_sb, "PAP-14140: reiserfs_file_write: file must have no direct items");
#endif

	  /* copy data from user space to intermediate buffer */
	  copy_from_user (local_buf, p_c_buf, n_item_bytes_to_write);

	  /* make sure, that direct item is still on its old place */
	  if (comp_items (&s_ih, &s_search_path)) {
	    printk ("reiserfs_file_write: item has been moved while we were in copy_from_user (inserting direct item after stat data)\n");
	    continue;
	  }

          /* Form direct item header. */
	  s_item_to_insert.ih_key.k_uniqueness = TYPE_DIRECT;
	  s_item_to_insert.u.ih_free_space = MAX_US_INT;
	  s_item_to_insert.ih_item_len = n_item_zeros_to_add + n_item_bytes_to_write;
          /* reiserfs_insert_item() inserts before given position in the node, so we must
	     increment to point to the next item after searched one. */
	  PATH_LAST_POSITION(&s_search_path)++;
          /* Insert direct item. */
	  if ( reiserfs_insert_item (&th, p_s_sb, &s_search_path, &s_item_to_insert,
				     local_buf, REISERFS_KERNEL_MEM, n_item_zeros_to_add, NOTHING_SPECIAL) < 0 )  {
	    if ( ! n_written )
	      n_written = -ENOSPC;
	    break; /* No disk space. */
          }
#ifdef REISERFS_CHECK
	  if (n_pos_in_file != n_append_startpoint + n_item_zeros_to_add)
	    reiserfs_panic (p_s_sb, "vs-14145: reiserfs_file_write: wrong positions in file");
#endif
	  update_vm_cache (p_s_inode, n_pos_in_file - 1, p_c_buf, n_item_bytes_to_write);

	  p_s_inode->u.reiserfs_i.i_first_direct_byte = n_append_startpoint;

	  /* calculate direct item as whole block */
	  p_s_inode->i_blocks += p_s_sb->s_blocksize / 512;
        }
      }

      else  {
        /* Last file item is the direct one. */
	if ( I_IS_DIRECT_ITEM(p_s_ih) ) {
	  /* n_cur_len is not always equal to p_s_ih->ih_item_len, if you write past the end
	     of the file, cur_len can be the length the item would be if it extended to
	     where we start the write. */
          n_cur_len =  (n_append_startpoint - 1) % n_blk_size;
#ifndef GET_MANY_BLOCKNRS
          /* Calculate maximum amount that could be added to the direct item and still
             fit into it or an unformatted node that it is converted into */
          n_max_squeeze_in = n_blk_size - (n_append_startpoint - 1) % n_blk_size;
#endif
          /* Calculate whether write requires converting direct item into an unformatted node. */

          if ( dont_have_tails (p_s_sb) ||
	       STORE_TAIL_IN_UNFM(n_append_startpoint - 1, n_cur_len + n_zero_bytes + n_count, n_blk_size) )  {

	    p_s_inode->u.reiserfs_i.i_pack_on_close = 0 ; /* no sense in packing here, we're already doing direct->
							  ** indirect conversion.  This was the case we are trying to
							  ** avoid, it really slows down the journal
	                                                  */

            /* Calculate number of zeros to be added to this item.  */
            n_item_zeros_to_add = ( n_zero_bytes > n_max_squeeze_in ) ? n_max_squeeze_in : n_zero_bytes;
            /* Item_bytes_to_write is the number of bytes from the user buffer to be
               written after the zeros to the new indirect item to be created.  */
            n_item_bytes_to_write = ( n_item_zeros_to_add < n_max_squeeze_in ) ?
              (n_max_squeeze_in - n_item_zeros_to_add) : 0;
            if ( n_item_bytes_to_write > n_count )
              n_item_bytes_to_write = n_count;

            /* Get a new buffer for storing the unformatted node. */
            if ( ! p_s_un_bh )  {
#ifdef OLD_GET_NEW_BUFFER
	      /* Minor design issue: putting the unformatted node in the place where the
		 formatted node used to be would result in more optimal layout, but then we
		 could not preserve the old formatted node.  This means that slowly grown
		 files (e.g. logs) use every other block of the available blocks.  At least
		 it is much better in layout than what some other fs`s do to slowly growing
		 files that are interspersed with other writes. A better solution will wait
		 until later. */
	      n_repeat = get_new_buffer(&th, p_s_sb, p_s_bh, &p_s_un_bh, &s_search_path, p_s_inode, n_pos_in_file);
#else
              /* This code gets the last non-hole blocknr of the last unformatted node without reading from disk that unformatted
                 node, but possibly reading from disk the node of the indirect item pointing to it. For large holes it
                 is inefficient, but better to spend the time writing code to compress holes than to fix that..  */
              /* if there is an unformatted node in the file, then get our unformatted node from near the last one of
                 them else get it from near the direct item.  */
              n_repeat = get_buffer_near_last_unf (&th, p_s_sb, &s_key_to_search,
                                                    p_s_inode, p_s_bh, 
                                                    &p_s_un_bh, &s_search_path);
#endif
              if ( ! p_s_un_bh )  {
                /* No disk space for new block. */
                if ( ! n_written )
                  n_written = -ENOSPC;
                pathrelse(&s_search_path);
                break;
              }
	      memset (p_s_un_bh->b_data, 0, n_blk_size);
              if ( n_repeat != CARRY_ON && (s_search_path.path_length == ILLEGAL_PATH_ELEMENT_OFFSET ||
					    comp_items(&s_ih, &s_search_path)) ) {
		/* compelte what was not finished in reiserfs_new_blocknrs */
		j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
		COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING (p_s_sb, j);
		continue;
	      }
            }
	    mark_buffer_uptodate(p_s_un_bh, 1);

	    /* bitmap block containing set bit */
	    j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
	    /* Perform the conversion. */
	    retval = direct_to_indirect (&th, p_s_sb, p_s_inode, &s_search_path, n_item_zeros_to_add,
					 p_c_buf, n_item_bytes_to_write, p_s_un_bh);
	    if (retval <= 0) {
	      /* conversion is done by another process (in bmap or mmap) or there is no disk space to
                 perform coversion */
	      reiserfs_free_block (&th, p_s_sb, p_s_un_bh->b_blocknr);
	      bforget (p_s_un_bh);

	      COMPLETE_BITMAP_DIRTING_AFTER_FREEING(p_s_sb,j);

	      p_s_un_bh = NULL;
	      if (retval < 0) {
		if ( ! n_written )
		  n_written = -ENOSPC;
		break;/* No disk space */
	      }
	      /* direct2indirect returned 0. Conversion has been done
                 by other process */
	      continue;
	    }
	    /* complete what was not finished in reiserfs_new_blocknrs */
	    COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING (p_s_sb, j);

	    /* ok, conversion is done. Unformatted node brelsed in direct_to_indirect */
	    p_s_un_bh = NULL;
	  }
	  /* If it is possible to perform write without converting to an unformatted node then
	     append to the direct item. */
	  else  {
	    p_s_inode->u.reiserfs_i.i_pack_on_close = 0 ;

	    n_item_bytes_to_write = n_count;
	    n_item_zeros_to_add   = n_zero_bytes;
	    if ( n_append_startpoint)
	      s_item_to_insert.ih_key.k_offset =  n_append_startpoint;
            n_bytes_in_item = p_s_ih->ih_item_len;

	    /* copy data from user space to intermediate buffer */
	    copy_from_user (local_buf, p_c_buf, n_item_bytes_to_write);
	    /* make sure, that direct item is still on its old place */
	    if (comp_items (&s_ih, &s_search_path)) {
	      printk ("reiserfs_file_write: item has been moved while we were in copy_from_user (appending to the direct item)\n");
	      continue;
	    }

	    if ( reiserfs_paste_into_item(&th, p_s_sb, &s_search_path, &n_bytes_in_item, &(s_item_to_insert.ih_key),
					  local_buf, n_count + n_zero_bytes, REISERFS_KERNEL_MEM, n_zero_bytes) < 0 ) {
	      if ( ! n_written )
		n_written = -ENOSPC;
	      break;
            }
	    
	    update_vm_cache (p_s_inode, n_pos_in_file - 1, p_c_buf, n_item_bytes_to_write);

          }
        }
        else  { /* last item is indirect item */

#ifdef REISERFS_CHECK
	  if ( COMP_SHORT_KEYS (&(p_s_ih->ih_key), INODE_PKEY (p_s_inode)) || !I_IS_INDIRECT_ITEM (p_s_ih) || n_pos_in_item != I_UNFM_NUM(p_s_ih) )
	    reiserfs_panic(p_s_sb,
			   "PAP-14150: reiserfs_file_write: item of another file, not indirect item or illegal position in the indirect item");
#endif

	  /* Blocknr from last entry in last item in file. */
	  n_unp = B_I_POS_UNFM_POINTER(p_s_bh,p_s_ih, n_pos_in_item - 1);
	  if ( p_s_ih->u.ih_free_space )  { /* Unformatted node has free space. */
	    /* Set n_pos_in_item to point to last entry of last indirect item. */
	    n_pos_in_item--;
	    /* See comments above, it is the same except that we paste into unused space at the
	       end of the unformatted node. */
	    n_max_squeeze_in = p_s_ih->u.ih_free_space;
	    n_item_zeros_to_add = ( n_zero_bytes < n_max_squeeze_in ) ? n_zero_bytes : n_max_squeeze_in;
	    n_item_bytes_to_write = ( n_item_zeros_to_add < n_max_squeeze_in ) ?
	      ( n_max_squeeze_in - n_item_zeros_to_add) : 0;
	    if ( n_item_bytes_to_write > n_count )
	      n_item_bytes_to_write = n_count;
	    if ( n_unp )  {
	      if ( ! p_s_un_bh ) {
		n_repeat = CARRY_ON;
                p_s_un_bh = reiserfs_getblk(p_s_bh->b_dev, n_unp, n_blk_size, &n_repeat);
		if ( ! buffer_uptodate(p_s_un_bh) ) {
		  n_repeat |= SCHEDULE_OCCURRED;
		  ll_rw_block(READ, 1, &p_s_un_bh);
		  wait_on_buffer(p_s_un_bh);
		  if ( ! buffer_uptodate(p_s_un_bh) ) {
		    pathrelse(&s_search_path);
		    brelse(p_s_un_bh);
		    if ( ! n_written )  {
		      
#ifdef REISERFS_INFO
		      printk ("REISERFS: reiserfs_file_write() returned EIO2\n");
#endif
		      
		      n_written = -EIO;
		    }
		    break;
		  }
		}
		
		if ( n_repeat != CARRY_ON && comp_items(&s_ih, &s_search_path) )
		  continue;
	      }

	      /* set free space of the indirect item */
	      p_s_ih->u.ih_free_space -= (n_item_zeros_to_add + n_item_bytes_to_write);
	      /* reiserfs_mark_buffer_dirty(p_s_bh, 1); */
	      journal_mark_dirty(&th,  p_s_sb, p_s_bh);

	      /* copy user data to the unformatted node */
	      memset(p_s_un_bh->b_data + n_blk_size - n_max_squeeze_in, '\0', n_item_zeros_to_add);
	      copy_from_user(p_s_un_bh->b_data + n_blk_size - n_max_squeeze_in + n_item_zeros_to_add,
			     p_c_buf, n_item_bytes_to_write);
	      update_vm_cache(p_s_inode, n_pos_in_file - 1,
			      p_s_un_bh->b_data + n_blk_size - n_max_squeeze_in + n_item_zeros_to_add,
			      n_item_bytes_to_write);
	      /* unformatted node is uptodate already */
	      /* non-atomic mark_buffer_dirty is allowed here */
	      /* mark_buffer_dirty(p_s_un_bh, 0);  */
	      journal_mark_dirty_nolog(&th, p_s_sb, p_s_un_bh) ;
	      brelse(p_s_un_bh);
	      p_s_un_bh = NULL;
            }
            else
              /* If last entry of last item is a hole (an undesirable feature, that can occur after
                 truncate). */
              if ( n_item_bytes_to_write ) {/* If writing to this item rather than to somewhere past it. */
                if ( ! p_s_un_bh )  {
#ifdef OLD_GET_NEW_BUFFER
		  n_repeat = get_new_buffer(&th, p_s_sb, p_s_bh, &p_s_un_bh, &s_search_path, p_s_inode, n_pos_in_file);
#else
		  n_repeat = get_buffer_near_last_unf (&th,p_s_sb, &s_key_to_search, p_s_inode, p_s_bh, &p_s_un_bh, &s_search_path);
#endif
                  if ( ! p_s_un_bh )  {
                    /* No disk space for new block. */
                    if ( ! n_written )
                      n_written = -ENOSPC;
                    pathrelse(&s_search_path);
                    break;
                  }
		  memset (p_s_un_bh->b_data, 0, n_blk_size);
		  if ( n_repeat != CARRY_ON && (s_search_path.path_length == ILLEGAL_PATH_ELEMENT_OFFSET || comp_items(&s_ih, &s_search_path)) ) {
		    /* compelte what was not finished in reiserfs_new_blocknrs */
		    j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
		    COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING (p_s_sb, j);
		    continue;
		  }
                }

#ifdef REISERFS_CHECK
		if (B_I_POS_UNFM_POINTER (p_s_bh, p_s_ih, n_pos_in_item) != 0)
		  reiserfs_panic (p_s_sb, "vs-14160: reiserfs_file_write: unformatted node pointer %lu, must 0 (hole at the end of file)",
				  B_I_POS_UNFM_POINTER(p_s_bh,p_s_ih, n_pos_in_item));
#endif

		/* set pointer to the unformatted node and free space of the indirect item */
		B_I_POS_UNFM_POINTER(p_s_bh, p_s_ih, n_pos_in_item) = p_s_un_bh->b_blocknr;
		n_cur_len = n_blk_size - p_s_ih->u.ih_free_space + n_item_zeros_to_add;
		p_s_ih->u.ih_free_space -= (n_item_zeros_to_add + n_item_bytes_to_write);
		/* reiserfs_mark_buffer_dirty(p_s_bh, 1); journal victim */
		journal_mark_dirty(&th, p_s_sb, p_s_bh); 

		/* copy user data to the unformatted node */
                //memset(p_s_un_bh->b_data, '\0',
		//     n_cur_len = n_blk_size - p_s_ih->u.ih_free_space + n_item_zeros_to_add);
		copy_from_user(p_s_un_bh->b_data + n_cur_len, p_c_buf, n_item_bytes_to_write);

		update_vm_cache(p_s_inode, n_pos_in_file - 1,
				p_s_un_bh->b_data + n_cur_len, n_item_bytes_to_write);

		p_s_inode->i_blocks += p_s_sb->s_blocksize / 512;

		//memset(p_s_un_bh->b_data + n_cur_len + n_item_bytes_to_write, '\0',
		//     n_blk_size - n_cur_len - n_item_bytes_to_write);
                mark_buffer_uptodate(p_s_un_bh, 1);
		/* non-atomic mark_buffer_dirty is allowed here */
                /* mark_buffer_dirty(p_s_un_bh, 0); journal victim */
		journal_mark_dirty_nolog(&th, p_s_sb, p_s_un_bh) ;

		/* compelte what was not finished in reiserfs_new_blocknrs */
		j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
		COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING (p_s_sb, j);

		brelse(p_s_un_bh);
		p_s_un_bh = NULL;
              }
            pathrelse(&s_search_path);
          } /* appending to the free space */

	  else  { /* Unformatted node doesn't have free space. */
	    /* This is where we could see a performance improvement by writing a little bit of code to:
	       1) calculate number of unformatted nodes to add at a time
	       entries_can_add_to_indirect_item = (end_of_node - end_of_item)/ indirect item entry size
	       if ( entries_can_add_to_indirect_item > 0) 
	       entries_can_add_to_indirect_item = min (entries_can_add_to_indirect_item, disk space free,
	       count) 
	       else
	       entries_can_add_to_indirect_item = min (max_indirect_item_size, disk space free, count)
	       2) construct new indirect item,
	       3) fill new indirect item with new blocknrs using reiserfs_new_block_nrs 
	       3) for each new blocknr, get_new_buffer, and write to that buffer
	       4) replace old indirect item with new indirect item
	       5) let this loop continue its work
	       
	       What do you think Volodya? -Hans
	       */
	    /* If we need to create an unformatted node. */
            if ( dont_have_tails (p_s_sb) ||
		 (n_orig_count >= MIN_PACK_ON_CLOSE) ||
		 STORE_TAIL_IN_UNFM(n_append_startpoint - 1, n_zero_bytes + n_count, n_blk_size) ) {

	      if (!STORE_TAIL_IN_UNFM(n_append_startpoint - 1, n_zero_bytes + n_count, n_blk_size) ) {
		p_s_inode->u.reiserfs_i.i_pack_on_close = 1 ;
	      } else {
		p_s_inode->u.reiserfs_i.i_pack_on_close = 0 ;
	      }
              /* Calculate zeros to insert before write into unformatted node. */
              n_item_zeros_to_add = ( n_zero_bytes < n_blk_size ) ? n_zero_bytes : n_blk_size;
              /* Calculate the amount to write into unformatted node. */
              n_item_bytes_to_write = ( n_item_zeros_to_add < n_blk_size ) ?
                ( n_blk_size - n_item_zeros_to_add) : 0;
              if ( n_item_bytes_to_write > n_count )
                n_item_bytes_to_write = n_count;
              /* If not making a hole. */
              if ( n_item_bytes_to_write )  {
                if ( ! p_s_un_bh )  {
#ifdef OLD_GET_NEW_BUFFER		  
		  n_repeat = get_new_buffer(&th, p_s_sb, p_s_bh, &p_s_un_bh, &s_search_path, p_s_inode, n_pos_in_file);
#else
                  if (n_unp) {
                    n_repeat = get_new_buffer_near_blocknr(&th, p_s_sb, n_unp, &p_s_un_bh, &s_search_path);
                  }
                  else {
		    n_repeat = get_buffer_near_last_unf (&th,p_s_sb,&s_key_to_search,p_s_inode, p_s_bh, &p_s_un_bh, &s_search_path);
		  }
#endif /* NEW_GET_NEW_BUFFER */  
                  if ( ! p_s_un_bh )  {
                    /* No disk space for new block. */
                    if ( ! n_written )
                      n_written = -ENOSPC;
                    pathrelse(&s_search_path);
                    break;
                  }
		  memset (p_s_un_bh->b_data, 0, n_blk_size);
		  if ( n_repeat != CARRY_ON && (s_search_path.path_length == ILLEGAL_PATH_ELEMENT_OFFSET || comp_items(&s_ih, &s_search_path)) ) {
		    /* compelte what was not finished in reiserfs_new_blocknrs */
		    j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
		    COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING (p_s_sb, j);
		    continue;
		  }
                }

		s_node_to_write.unfm_nodenum = p_s_un_bh->b_blocknr;
		s_node_to_write.unfm_freespace = n_blk_size - n_item_zeros_to_add - n_item_bytes_to_write;
              }
              else  { /* If making a hole. */

#ifdef REISERFS_CHECK
		if ( p_s_un_bh ) {
		  reiserfs_panic(p_s_sb, "PAP-14170: reiserfs_file_write: pointer to the unformatted node buffer must be equals NULL");
		}
#endif
		s_node_to_write.unfm_nodenum = 0;
		s_node_to_write.unfm_freespace = 0;
              }

#ifdef REISERFS_CHECK
	      if ( n_append_startpoint < n_blk_size + 1 )
		reiserfs_panic (p_s_sb, "PAP-14180: reiserfs_file_write: offset is 0");
	      if ( p_s_ih->ih_item_len % UNFM_P_SIZE ) {
		reiserfs_panic (p_s_sb, "PAP-14190: reiserfs_file_write: item %h length is incorrect", p_s_ih);
	      }
#endif
              s_item_to_insert.ih_key.k_offset = n_append_startpoint;
	      n_bytes_in_item = p_s_ih->ih_item_len / UNFM_P_SIZE;

	      /* Paste entry for p_s_un_bh into last indirect item. */
	      if ( reiserfs_paste_into_item(&th, p_s_sb, &s_search_path, &n_bytes_in_item, &(s_item_to_insert.ih_key),
					    (char *)&s_node_to_write, UNFM_P_SIZE, REISERFS_KERNEL_MEM, 0) < 0 ) {
		/* If no disk space for balancing required to insert entry for new unformatted
		   node into last indirect item. */
		if ( ! n_written )
		  n_written = -ENOSPC;
                if ( p_s_un_bh )  {
		  reiserfs_free_block(&th, p_s_sb, p_s_un_bh->b_blocknr);
		  j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
		  COMPLETE_BITMAP_DIRTING_AFTER_FREEING(p_s_sb,j);

                  bforget(p_s_un_bh);
                  p_s_un_bh = NULL;
                }
                break;
              }
              if ( p_s_un_bh )	{ /* If not a hole. */
		/* copy user data to the unformatted node */
                //memset(p_s_un_bh->b_data, '\0', n_item_zeros_to_add);
		copy_from_user(p_s_un_bh->b_data + n_item_zeros_to_add, p_c_buf, n_item_bytes_to_write);
		//memset(p_s_un_bh->b_data + n_item_zeros_to_add + n_item_bytes_to_write, '\0', s_node_to_write.unfm_freespace);

		update_vm_cache(p_s_inode, n_pos_in_file - 1,
				p_s_un_bh->b_data + n_item_zeros_to_add, n_item_bytes_to_write);
                mark_buffer_uptodate(p_s_un_bh,1);
		/* non-atomic mark_buffer_dirty is allowed here */
		/* mark_buffer_dirty(p_s_un_bh, 0); */
		journal_mark_dirty_nolog(&th, p_s_sb, p_s_un_bh) ;

		/* compelte what was not finished in reiserfs_new_blocknrs */
		j = p_s_un_bh->b_blocknr / (p_s_sb->s_blocksize * 8);
		COMPLETE_BITMAP_DIRTING_AFTER_ALLOCATING (p_s_sb, j);

		brelse(p_s_un_bh);
                p_s_un_bh = NULL;

		/* i_blocks counts only unformatted nodes */
		p_s_inode->i_blocks += p_s_sb->s_blocksize / 512;
	      }
            }
            else  { /* Insert direct item. */
	      p_s_inode->u.reiserfs_i.i_pack_on_close = 0 ;

	      n_item_bytes_to_write = n_count;
	      n_item_zeros_to_add = n_zero_bytes;
	      /* Create direct item header. */
	      s_item_to_insert.ih_key.k_offset = n_append_startpoint;
              /* Mark item as not mergeable. */
	      s_item_to_insert.ih_key.k_uniqueness = TYPE_DIRECT;
              /* Mark item as direct. */
	      s_item_to_insert.u.ih_free_space = MAX_US_INT;
	      s_item_to_insert.ih_item_len = n_zero_bytes + n_item_bytes_to_write;

	      /* copy data from user space to intermediate buffer */
	      copy_from_user (local_buf, p_c_buf, n_item_bytes_to_write);
	      /* make sure, that direct item is still on its old place */
	      if (comp_items (&s_ih, &s_search_path)) {
		printk ("reiserfs_file_write: item has been moved while we were in copy_from_user (inserting the direct item after the last indirect)\n");
		continue;
	      }

              /* reiserfs_insert_item() inserts before given position in the node, so we must
		 increment to point to the next item after searched one. */
	      PATH_LAST_POSITION(&s_search_path)++;
	      if ( reiserfs_insert_item (&th, p_s_sb, &s_search_path, &s_item_to_insert,
					 local_buf, REISERFS_KERNEL_MEM, n_zero_bytes, NOTHING_SPECIAL) < 0 )  {
		if ( ! n_written )
		  n_written = -ENOSPC;
		break; /* No disk space. */
              }
	      
	      update_vm_cache(p_s_inode, n_pos_in_file - 1, p_c_buf, n_item_bytes_to_write);

	      /* calculate direct item as whole block */
	      p_s_inode->i_blocks += p_s_sb->s_blocksize / 512;

#ifdef REISERFS_CHECK
	      if ( p_s_inode->u.reiserfs_i.i_first_direct_byte != NO_BYTES_IN_DIRECT_ITEM ||
		   n_append_startpoint + n_zero_bytes != n_pos_in_file)
		reiserfs_panic(p_s_sb, "PAP-14200: reiserfs_file_write: file must have no direct items");
#endif

	      p_s_inode->u.reiserfs_i.i_first_direct_byte = n_append_startpoint;
            }
          }
	}
      }
    }

    n_count       -= n_item_bytes_to_write;
    p_c_buf       += n_item_bytes_to_write;
    n_pos_in_file += n_item_bytes_to_write;
    n_written     += n_item_bytes_to_write;

    if ( (s_key_to_search.k_offset += n_item_bytes_to_write) >= p_s_inode->u.reiserfs_i.i_first_direct_byte )
      s_key_to_search.k_uniqueness = TYPE_DIRECT;
    else
      s_key_to_search.k_uniqueness = TYPE_INDIRECT;

    /* here we do a polite test to see if the journal needs a little more room.
    ** if so, we write our inode to make sure it stays with this transaction, and give
    ** the journal_end/begin pair the chance to end the current transaction
    ** don't bother ending if we're already done writing
    */
    if (n_count > 0 && journal_transaction_should_end(&th, jbegin_count)) {
      pathrelse(&s_search_path);
      update_inode_and_restart_transaction(&th, p_s_inode, n_pos_in_file) ;
    }
  }

  
  if ( --n_pos_in_file > p_s_inode->i_size )  {
    p_s_inode->i_size   = n_pos_in_file;
    p_s_inode->i_ctime  = CURRENT_TIME;
  }

  p_s_inode->i_mtime  = CURRENT_TIME;
  *p_n_pos = n_pos_in_file;

  if_in_ram_update_sd (&th, p_s_inode); 

  reiserfs_kfree (local_buf, n_blk_size, p_s_inode->i_sb);
  pop_journal_writer(windex) ;
  journal_end(&th, p_s_sb, jbegin_count) ;
  return n_written;
}



/* Please note that the benchmarking of the right numbers for
   RESIERFS_NBUF, etc., was insufficiently investigated.

   Hans */



/* Wait for and then release the read-ahead blocks. We need a brelse that does not wait. */
#define RELEASE_READ_AHEAD_BLOCKS       while ( p_s_bhe != p_s_bhb ) {\
                                                brelse(p_s_bhe->bi_buf);\
                                                if ( ++p_s_bhe == &a_p_s_range_bufs_ids[REISERFS_NBUF] )\
                                                        p_s_bhe = a_p_s_range_bufs_ids;\
                                        }
#define INCREASE_P_S_BHE		if ( ++p_s_bhe == &a_p_s_range_bufs_ids[REISERFS_NBUF] )\
              					p_s_bhe = a_p_s_range_bufs_ids


/* if Hans understands correctly this works by oscillating between a
   request and a fulfill loop.  (It would be nice if Anatoly edited the
   code to make it clearer as to its design objectives and algorithm,
   as that might make it easier to see ways to simplify it).

   The request loop assembles a list of not more than NBUF buffers
   (buflist) which are within the range and which are cache children
   (their parents are in cache, they are not).  It then requests I/O
   on that list, and then the fulfill loop starts processing the list.

   The fulfill loop goes through the list, and completes as much of
   the read as it can.  If the fulfill loop processes a buffer whose
   children contain data that needs to be read, then it oscillates
   back to the request loop, which will then request not only those
   new cache children which were children of the buffer that the
   fulfill loop stopped on, but all cache children in the range.

   The request loop works by calling get_buffer_by_range(), which
   returns a buffer which either contains the node containing the
   readkey, or which is prepared for requesting I/O on to get the
   cache child corresponding to the readkey.  The readkey is
   incremented as a result of each get_buffer_by_range so that it
   holds the key of the next byte after the buffer that was returned
   in the range.
   
   The reason for this algorithm is to allow one to read as much in
   parallel as possible, and this is done by ensuring that there is an
   outstanding request for all of the first NBUF cache children that
   are in the range, and submitting new requests that contain more
   cache children as soon as possible.  Question: what happens if
   there are NBUF outstanding requests, the first of them completes,
   it contains an indirect item with lots of buffers which must be
   read, but bhreq is full?  Is the indirect item requested one buffer
   at a time?  Or will all of the nodes on the request list get
   processed, and bhreq gets freed up? Will we get a pathological
   behaviour in which the disk head starts to move towards the other
   nodes on the request list, but keeps getting dragged back to handle
   one more node from the indirect item, each request for which is
   separated by a rotation? Can we test to see if this ever
   happens?  Maybe we can printk some blocknumbers? -Hans */

static ssize_t  reiserfs_file_read(		/*  Read from file system into user buffer.		*/
              struct file     *	p_s_filp,   	/*  Object table entry for this inode. ( p_s_filp->f_pos
						    will provide us with the offset from which we 
						    start the read, and we will update it to reflect
						    how much we have read as we perform the read.)	*/
              char 	      *	p_c_buf,	/*  Address of the user buffer.				*/
              size_t		n_count,		/*  Count of bytes copied into user buffer.		*/
	      loff_t	      * p_n_pos
            ) {
  struct inode * p_s_inode = p_s_filp->f_dentry->d_inode;
  struct super_block  *	p_s_sb;			/* Pointer to the super block.			*/
  struct key            s_range_begin,		/*  Minimal range key to request.       	*/
                        s_range_end,		/*  Maximal range key to request.       	*/
                        s_readkey;		/*  Current read key,
					    	    (the key version of offset )		*/
  ssize_t		n_read;			/*  Number of bytes which have been read.	*/
  unsigned int		n_pos_in_file,		/* Current offset in the file.			*/
			n_file_size,
			n_left;			/*  Number of bytes remaining to read.		*/


 
  int                   n_offset_in_item,       /*  Offset in unformatted node or direct item.  */
                        n_chars,                /*  Number of bytes to copy.                    */
#ifdef READ_LOCK_REISERFS
                        this_syscall_has_the_read_lock = 0, /* flag to indicate whether this read syscall is the one that
                                                        locked the FS, if so then don't worry about the FS being read
                                                        locked */
#endif /* READ_LOCK_REISERFS */
                        n_blocksize;            /* Buffer size.                                 */

  char                * p_c_addr = NULL;        /*  Address in a system buffer.                 */


  struct buffer_head  * a_p_s_bhreq[REISERFS_NBUF],      /*  Array of the not uptodate buffers
                                                    from  the read range.                       */
                      * p_s_bh;
  struct buffer_and_id a_p_s_range_bufs_ids[REISERFS_NBUF];      /*  Array of all buffers and ids from
                                                            the read range.                     */

                                /* it seems that bhb is used in the
                                   request preparation to point to
                                   where to insert the next buffer
                                   onto bhreq, and bhe is used in the
                                   post-request processing to go
                                   through the array to do things with
                                   every buffer that has completed its
                                   requested I/O and is now uptodate
                                   and unlocked.  The case in which
                                   bhe = bhb represents the case in
                                   which either all requests have
                                   completed, or bhreq is completely
                                   filled with requests uncompleted.
				   
				   Note that both bhb and bhe are
				   allowed to wrap around the end of
				   buflist.  This is necessary for
				   when the read is larger than bhreq
				   can hold.  -Hans */
  struct buffer_and_id	      *	p_s_bhb,	/*  We need two variables to go through		*/
    			      *	p_s_bhe;	/*  array a_p_s_range_bufs_ids.                 */
  int                   	n_bhrequest,	/* offset in the array a_p_s_bhreq.		*/
                        	n_uptodate;
  char * local_buf;		/* copy_to_user can cause schedule. Therefore we can not copy bytes
				   directly from direct item to user buffer. Local_buf is used as
				   intermediate buffer */

#ifdef REISERFS_CHECK
  int				n_repeat_counter = 0;
#endif

#ifdef __KERNEL__
  if (use_genericread (p_s_inode->i_sb)) {
    increment_i_read_sync_counter(p_s_inode);
    n_read = generic_file_read (p_s_filp, p_c_buf, n_count, p_n_pos);
    decrement_i_read_sync_counter(p_s_inode);
    return n_read;
  }
#endif

  if ( ! p_s_inode ) {
    printk("reiserfs_file_read: pointer to the inode = NULL\n");
    return -EINVAL;
  }

  if ( ! S_ISREG(p_s_inode->i_mode) && ! S_ISLNK(p_s_inode->i_mode) ) {
    printk("reiserfs_file_read: mode = %07o\n",p_s_inode->i_mode);
    return -EINVAL;
  }

  /* Calculate position in the file. */
  n_pos_in_file = *p_n_pos + 1 ;

  /* Calculate object size. */
  n_file_size = p_s_inode->i_size;

  /* Using position in the file, file size, and the given number of bytes to read
     calculate the number of bytes, that should be actually read;
     put it in variable n_left. */
  if ( n_pos_in_file > n_file_size || n_count <= 0 ) /* Nothing to read. */
    return 0;

  increment_i_read_sync_counter(p_s_inode);

  n_left = n_file_size - n_pos_in_file + 1;
  if ( n_left > n_count )
    n_left = n_count;
  n_read = 0;

  p_s_sb = p_s_inode->i_sb;

  /* Initialize read range. */
  copy_key(&s_range_begin, &(p_s_inode->u.reiserfs_i.i_key));
  s_range_begin.k_offset = n_pos_in_file;
  if ( INODE_OFFSET_IN_DIRECT(p_s_inode, n_pos_in_file) )
    s_range_begin.k_uniqueness = TYPE_DIRECT;
  else
    s_range_begin.k_uniqueness = TYPE_INDIRECT;

  copy_key(&s_range_end, &(p_s_inode->u.reiserfs_i.i_key));
  s_range_end.k_offset = n_pos_in_file + n_left - 1;
  if ( INODE_OFFSET_IN_DIRECT(p_s_inode, s_range_end.k_offset) )
    s_range_end.k_uniqueness = TYPE_DIRECT;
  else
    s_range_end.k_uniqueness = TYPE_INDIRECT;

#ifdef REISERFS_OBJECT_READ_AHEAD
  s_range_end.k_offset = n_file_size;
  s_range_end.k_uniqueness = TYPE_DIRECT;
#endif

#ifdef PACKING_LOCALITY_READ_AHEAD
  s_range_end.k_objectid = MAX_KEY_OBJECTID;
  s_range_end.k_offset = MAX_KEY_OFFSET;
  s_range_end.k_uniqueness = MAX_KEY_UNIQUENESS;
#endif

  /* Set current key to read . */
  copy_key(&s_readkey, &s_range_begin);

  p_s_bhb = p_s_bhe = a_p_s_range_bufs_ids;

  n_blocksize = p_s_sb->s_blocksize;

  local_buf = reiserfs_kmalloc (n_blocksize, GFP_KERNEL, p_s_sb);
  if (local_buf == 0) {
    return -ENOMEM;
  }

  /* Here is the loop to cause us to oscillate between requesting and fulfilling */
  do {

#ifdef REISERFS_CHECK
    if ( ! (++n_repeat_counter % 50000) ) {
      reiserfs_panic(p_s_sb, "PAP-14205: reiserfs_fileread: counter(%d) too big. Range begin %k",
		     n_repeat_counter, &s_range_begin);
    }
#endif

    n_bhrequest = 0;

    /* This says no, there are not any buffers that we are waiting for I/O to complete for. -Hans */
    n_uptodate = 1;

    if ( p_s_bhb == p_s_bhe && COMP_KEYS(&s_range_begin, &s_range_end) == 1 ) {

#ifdef REISERFS_INFO
      printk("reiserfs_fileread: request key is not in the range and request is empty but there are bytes to read \n");
#endif

      copy_key(&s_range_begin, &s_readkey);
    }

    /* Request loop (well, actually the prepare to request loop) */
    /* This while loop assembles a request array (bhreq) which contains either a single buffer which does not require
       I/O to fetch and which we will proceed to read into the user buffer immediately or enough cache children in the
       range to fill bhreq so that we can get REISERFS_NBUF buffers all at once.  -Hans */
    while ( COMP_KEYS(&s_range_begin, &s_range_end) != 1 )  { /* While current key is in the range. */

      /* Calculate next buffer from range. This returns either a
         buffer that was in cache, or a buffer all set for us to
         request I/O on. */
      get_buffer_by_range(p_s_sb, &s_range_begin, &s_range_end, &(p_s_bhb->bi_buf), &(p_s_bhb->bi_id));

      /* Buffer is not uptodate (in other words, it wasn't in cache
         and we need to read it). Put it in the request array. */
      if ( p_s_bhb->bi_buf && ! buffer_uptodate(p_s_bhb->bi_buf) ) {
        n_uptodate = 0;
        a_p_s_bhreq[n_bhrequest++] = p_s_bhb->bi_buf;
      }

      /* Increment, and possibly wraparound to beginning of list */
      if ( ++p_s_bhb == &a_p_s_range_bufs_ids[REISERFS_NBUF] )
        p_s_bhb = a_p_s_range_bufs_ids;

      /* If all of the buffers we have processed so far in this pass
         of the loop are uptodate (in which case there is only one
         such buffer) then go ahead and copy its contents to user
         space now rather than assembling more buffers. */

      if ( n_uptodate )
        break;
      if ( p_s_bhb == p_s_bhe )
        break;
    }

#ifdef REISERFS_CHECK
    /* Check whether buffers from the request have valid device. */
    for ( n_chars = 0; n_chars < n_bhrequest; n_chars++ )
      if ( a_p_s_bhreq[n_chars]->b_dev == NODEV )
	reiserfs_panic(p_s_sb, "PAP-14210: reiserfs_file_read: device is NODEV");
#endif

    /* Now request them all. */
    if ( n_bhrequest ) {
#ifdef READ_LOCK_REISERFS
                                /* So why read_lock the FS?  Because serial reads are more efficient than parallel
                                   reads, substantially so say the benchmarks.  now you might ask, why not wait on the
                                   lock?  The reason is that I have an untested hope that it will cause a series of
                                   large reads from the same process succeeding in its lock once to tend to get
                                   priority.  It is deliberately unfair.  Don't go moving that disk head.... -Hans */
      while (!try_ulong_lock(&(p_s_sb->u.reiserfs_sb.read_lock), 0) && !this_syscall_has_the_read_lock)
        {
/*        printk("blocked for lock %lu:", p_s_sb->u.reiserfs_sb.read_lock); */
          schedule();
          /*  don't know if schedule can invalidate what we are reading, or if the read picks up all the pieces properly
              when this is done. -Hans */
        }
      this_syscall_has_the_read_lock = 1;
#endif /* READ_LOCK_REISERFS */
      ll_rw_block(READ, n_bhrequest, a_p_s_bhreq);
    }

    /* fulfillment loop */
    /* Finish off all I/O that has actually completed. */
    do {
      /* Check to see if read error occured. In this case we break read function.  */
      if ( (p_s_bh = p_s_bhe->bi_buf) ) {
        wait_on_buffer(p_s_bh);
        if ( ! buffer_uptodate(p_s_bh) ) {
          brelse(p_s_bh);
	  INCREASE_P_S_BHE;
          n_left = 0;
	  printk ("reiserfs_file_read: I/O error (block %lu, dev 0%o, size %ld)\n", p_s_bh->b_blocknr, p_s_bh->b_dev, p_s_bh->b_size);
          break;
        }
      }

      /* If buffer is not in tree, or is key level, then repeat buffer calculating.  Buffer is not in tree means that
        some balancing occured while we were waiting for the needed buffer or getting next buffer from range. This
        balancing removed needed buffer from the tree.  It is possible for all tree levels. It is not possible just for
        unformatted nodes.  If after waiting we have internal node buffer we can not read from it(it does not contain
        data).  In both cases we call get_buffer_from range once more to get bytes for read. -Anatoly.  */

      /* ok, so if we now have a formatted node, check to confirm that
         we got the right one, then copy our data to user space,
         checking as we copy to see if we need to descend into any
         unformatted node children.  If we so need, then we better
         oscillate back to the request loop to read those children
         into memory (and while in that request loop we might as well
         try to read everything else in the range that we can....)
         -Hans */
      if ( p_s_bh && p_s_bhe->bi_id == MAX_KEY_OBJECTID ) {
	if ( ! B_IS_IN_TREE(p_s_bh) || ! B_IS_ITEMS_LEVEL(p_s_bh) ) {
	  /* We are repeating the read starting from the current s_readkey */   
          brelse(p_s_bh);
          INCREASE_P_S_BHE;
          RELEASE_READ_AHEAD_BLOCKS;
          copy_key(&s_range_begin, &s_readkey);
          p_s_bhb = p_s_bhe = a_p_s_range_bufs_ids;
          break;
        }

        /* Needed byte should be in the leaf we were waiting for */
        if ( COMP_KEYS(B_N_PKEY(p_s_bh, 0), &s_readkey) < 1 &&
	     COMP_KEYS(B_PRIGHT_DELIM_KEY(p_s_bh), &s_readkey) == 1 ) {

          int 			n_search_res,
				n_item_pos;
	  struct item_head    *	p_s_ih;

          /* Find item contains needed byte. */
          n_search_res = bin_search(&s_readkey, B_N_PITEM_HEAD(p_s_bh, 0), B_NR_ITEMS(p_s_bh), IH_SIZE, &n_item_pos);
          p_s_ih = B_N_PITEM_HEAD(p_s_bh, n_item_pos);
          /* We are looking for an item contains needed byte of the needed object. In case of n_search_res = 0 it can
             not be *p_s_ih. Probably *(p_s_ih--) it is. We are checking it. */
          if ( n_search_res == ITEM_NOT_FOUND )
            p_s_ih--;

	  /* error checking: if ih does not contain the byte corresponding to readkey -Hans */
	  if ( ! I_K_KEY_IN_ITEM(p_s_ih, &s_readkey, n_blocksize) ) {

#ifdef REISERFS_CHECK
            printk ("reiserfs_file_read: can not read bytes (file was truncated or deleted)\n");
#endif

            brelse(p_s_bh);
	    INCREASE_P_S_BHE;
            n_left = 0;
            break;
          }

          /* If needed byte is located in an unformatted node then oscillate
             back to request loop so that it will be gotten for us,
             but only do so after waiting for completion of all
             read_ahead blocks. */
          if ( I_IS_INDIRECT_ITEM(p_s_ih) ) {

#ifdef REISERFS_CHECK
	    if ( s_readkey.k_uniqueness != TYPE_INDIRECT )
	      reiserfs_panic(p_s_sb, "PAP-14240: reiserfs_file_read: invalid uniqueness in the read key");
#endif

            brelse(p_s_bh);
            INCREASE_P_S_BHE;
                                /* waits for as well as releases */
            RELEASE_READ_AHEAD_BLOCKS;
            copy_key(&s_range_begin, &s_readkey);
            p_s_bhb = p_s_bhe = a_p_s_range_bufs_ids;
            break;
          }

#ifdef REISERFS_CHECK
	  if ( s_readkey.k_uniqueness != TYPE_DIRECT )
	    reiserfs_panic(p_s_sb, "PAP-14250: reiserfs_file_read: invalid uniqueness in the read key");
#endif

	  /* So, there are bytes to copy from the direct item. */
          n_offset_in_item = n_pos_in_file - p_s_ih->ih_key.k_offset;
          n_chars = p_s_ih->ih_item_len - n_offset_in_item;
          p_c_addr  = B_I_PITEM(p_s_bh, p_s_ih) + n_offset_in_item;
	}
        else {

#ifdef REISERFS_CHECK
          printk("reiserfs_file_read: key is not in the buffer\n");
#endif

          brelse(p_s_bh);
	  INCREASE_P_S_BHE;
          RELEASE_READ_AHEAD_BLOCKS;

          copy_key(&s_range_begin, &s_readkey);
          p_s_bhb = p_s_bhe = a_p_s_range_bufs_ids;
          break;
        }
      }

      else {
	/* We waited for an unformatted node. Read from it. */
	if ( p_s_bh ||  p_s_bhe->bi_id != MAX_KEY_OBJECTID ) {

	  if ( p_s_bhe->bi_id != s_readkey.k_objectid ) {
	    
#ifdef REISERFS_CHECK
	    /*printk("reiserfs_file_read: can not read bytes(3) (file was truncated or deleted)\n");*/
#endif
            
            brelse(p_s_bh);
            INCREASE_P_S_BHE;
            n_left = 0;
            
            break;
          }
          
#ifdef REISERFS_CHECK
          if ( s_readkey.k_uniqueness != TYPE_INDIRECT ) {
            print_block(p_s_bh, 0, -1, -1);
            printk("size = %ld, first_direct = %d\n", p_s_inode->i_size, p_s_inode->u.reiserfs_i.i_first_direct_byte);
            printk ("p_s_bhe->bi_id==%lu\n", p_s_bhe->bi_id);
            reiserfs_panic(p_s_sb, "PAP-14270: reiserfs_file_read: invalid uniqueness in the read key %k. TYPE_INDIRECT expected",
			   &s_readkey);
          }
#endif

	}

        /* Calculate offset in the unformatted node. */
        n_offset_in_item = (n_pos_in_file - 1) % n_blocksize;
        n_chars = n_blocksize - n_offset_in_item;
        if ( p_s_bh )
          p_c_addr = n_offset_in_item + p_s_bh->b_data;
      }

      if ( n_chars > n_left )
        n_chars = n_left;
      *p_n_pos += n_chars ; /* p_s_filp->f_pos += n_chars; */
      n_left -= n_chars;
      n_read += n_chars;
      n_pos_in_file += n_chars;
      /* Here is one place where we reset readkey so that the next
         buffer is gotten on the next loop iteration.. */
      s_readkey.k_offset = n_pos_in_file;
      if ( n_pos_in_file >= p_s_inode->u.reiserfs_i.i_first_direct_byte )
	s_readkey.k_uniqueness = TYPE_DIRECT;

#ifdef REISERFS_CHECK
      if ( n_chars < 0 || n_chars > n_blocksize )
	reiserfs_panic(p_s_sb, "PAP-14280: reiserfs_file_read: illegal bytes number to read");
#endif

      if (p_s_bhe->bi_id != MAX_KEY_OBJECTID) {
	/* when copying bytes from an unformatted node, we do not need intermediate buffer */
	if ( p_s_bh ) {
	  copy_to_user(p_c_buf, p_c_addr, n_chars);
	  brelse(p_s_bh);
	  p_c_buf += n_chars;
	} else {
	  while ( n_chars-- > 0 )
	    if ( put_user(0, p_c_buf++) )
	      reiserfs_panic(p_s_sb, "PAP-14290: reiserfs_file_read: put_user failed");
	}
      } else {
	/* Copy bytes from direct item into intermediate buffer. */
	if ( p_s_bh ) {
	  memcpy (local_buf, p_c_addr, n_chars);
	  brelse(p_s_bh);
	} else {
	  memset (local_buf, 0, n_chars);
	}

	/* copy bytes from intermediate buffer to the user buffer */
	copy_to_user(p_c_buf, local_buf, n_chars);
	p_c_buf += n_chars;
      }

      INCREASE_P_S_BHE;
    } while ( n_left > 0 && p_s_bhe != p_s_bhb && (! p_s_bhe->bi_buf || ! buffer_locked(p_s_bhe->bi_buf)) ) ;
  } while ( n_left > 0 );

  RELEASE_READ_AHEAD_BLOCKS;

  decrement_i_read_sync_counter(p_s_inode);

#ifdef READ_LOCK_REISERFS
  unlock_ulong_lock(&(p_s_sb->u.reiserfs_sb.read_lock), 0,  &(p_s_sb->u.reiserfs_sb.read_wait));
 /*  printk("unlocked lock %lu:", p_s_sb->u.reiserfs_sb.read_lock); */
#endif /* READ_LOCK_REISERFS */

  reiserfs_kfree (local_buf, n_blocksize, p_s_sb);
  if ( ! n_read ) {
    return -EIO;
  }
  UPDATE_ATIME(p_s_inode);

  return n_read;
}


