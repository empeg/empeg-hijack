/*
 *  Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */


/*
 * Contains code from
 *
 *  linux/include/linux/lock.h and linux/fs/buffer.c /linux/fs/minix/fsync.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */
#ifdef __KERNEL__

#include <linux/sched.h>
#include <linux/locks.h>
#include <linux/reiserfs_fs.h>



/*
 * wait_buffer_until_released
 *  reiserfs_bread
 *  reiserfs_sync_block
 *  sync_unf_nodes
 *  sync_file_item
 *  sync_file_items
 *  reiserfs_sync_file
 */


/* when we allocate a new block (get_new_buffer, get_empty_nodes,
   get_nodes_for_preserving) and get buffer for it, it is possible
   that it is held by someone else or even by this process. In this
   function we wait until all other holders release buffer. To make
   sure, that current process does not hold we did free all buffers in
   tree balance structure (get_empty_nodes and
   get_nodes_for_preserving) or in path structure only
   (get_new_buffer) just before calling this */
void wait_buffer_until_released (struct buffer_head * bh)
{
  int repeat_counter = 0;

  while (bh->b_count > 1) {
    if ( !(++repeat_counter % 200000) ) {
      reiserfs_warning ("vs-3050: wait_buffer_until_released: nobody releases buffer (%b). Still waiting (%d) %cJDIRTY %cJWAIT\n",
			bh, repeat_counter, buffer_journaled(bh) ? ' ' : '!',
			buffer_journal_dirty(bh) ? ' ' : '!');
    }
    current->policy |= SCHED_YIELD;
    schedule();
  }
}


/* This is pretty much the same as the corresponding original linux
   function, it differs only in that it tracks whether schedule
   occurred. */
/*
 * reiserfs_bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
/* It first tries to find the block in cache, and if it cannot do so
   then it creates a new buffer and schedules I/O to read the
   block. */

struct buffer_head  * reiserfs_bread(
                        kdev_t  n_dev,
                        int     n_block,
                        int     n_size,
                        int   * p_n_repeat
                      ) {
  struct buffer_head * p_s_bh;

  p_s_bh = reiserfs_getblk(n_dev, n_block, n_size, p_n_repeat);
  if ( buffer_uptodate(p_s_bh) )
    return p_s_bh;
  ll_rw_block(READ, 1, &p_s_bh);
  *p_n_repeat |= SCHEDULE_OCCURRED;
  wait_on_buffer(p_s_bh);
  if ( buffer_uptodate(p_s_bh) )
    return p_s_bh;
  printk("reiserfs_bread: unable to read dev = %d block = %d size = %d\n", n_dev, n_block, n_size);
  brelse(p_s_bh);
  return NULL;
}





/* Synchronize a block of reiserfs. */
static int  reiserfs_sync_block(
              struct buffer_head  * p_s_bh,   /* Pointer to the buffer header to sync.  */
              int                   n_wait,   /* Wait parameter.                        */
              struct path         * p_s_path  /* Pointer to the path contains buffer.
                                                  NULL in case of an unformatted node.  */
            ) {
  if ( n_wait && buffer_req(p_s_bh) && ! buffer_uptodate(p_s_bh) )  {
    /* Release buffer if it is unformatted node. If not it will be released by pathrelse(). */
    if ( ! p_s_path )
      brelse(p_s_bh);
    return -1;
  }
  if ( n_wait || ! buffer_uptodate(p_s_bh) || ! buffer_dirty(p_s_bh) )  {
    if ( ! p_s_path )
      brelse(p_s_bh);
    return 0;
  }

  /* unformatted nodes can go right to the disk, but they are
  ** skipped if they have been logged recently.  Committing the
  ** transaction will be good enough to call them synced
  */
  if (!p_s_path && !buffer_journaled(p_s_bh) && !buffer_journal_dirty(p_s_bh)) {
    ll_rw_block(WRITE, 1, &p_s_bh); 
  }
  p_s_bh->b_count--;

  /* Decrement path length if it is buffer at the path. */
  if ( p_s_path ) {

#ifdef REISERFS_CHECK
    if ( p_s_path->path_length < FIRST_PATH_ELEMENT_OFFSET )
      reiserfs_panic(0, "PAP-16015: reiserfs_sync_block: path length is too small");
#endif

    p_s_path->path_length--;
  }

  return 0;
}


/* Sync unformatted nodes of the item *p_s_ih. */
static int  sync_unf_nodes(
              struct inode      * p_s_inode,        /* Pointer to the file inode.         */
              struct item_head  * p_s_ih,           /* Pointer to the found item header.  */
              int               * p_n_pos_in_item,  /* Position in the found item.        */
              struct path       * p_s_path_to_item, /* Path to the found item.            */
              int                 n_wait,           /* Wait parameter.                    */
              unsigned long     * p_n_synced  /* Returned value. UNFM number were
                                                        synced in this call.        */
            ) {
  struct key            s_item_key;
  struct buffer_head  * p_s_unfm_bh;
  unsigned long         n_unfm_pointer;
  int                   n_repeat,
                        n_counter,
                        n_unfm_number_to_sync = I_UNFM_NUM(p_s_ih) - *p_n_pos_in_item;

  /* Form key to search for the first unformatted node to be synced. */
  copy_key(&s_item_key, &(p_s_ih->ih_key));
  s_item_key.k_offset += *p_n_pos_in_item * p_s_inode->i_sb->s_blocksize;
  if ( search_for_position_by_key(p_s_inode->i_sb, &s_item_key, p_s_path_to_item, p_n_pos_in_item, &n_repeat) == POSITION_NOT_FOUND )
    return 1; /* Item to sync UNFM was not found. */
  /* Remember found item header.  */
  copy_item_head(p_s_ih, PATH_PITEM_HEAD(p_s_path_to_item));

  /* Calculate number of the UNFM to be synced. */
  if ( n_unfm_number_to_sync > I_UNFM_NUM(p_s_ih) - *p_n_pos_in_item/*vs*/)
    /* do not sync more unformatted nodes, than number of synced unformatted node pointers. If there
       is no so many unformatted nodes in found indirect item as we need, sync what we have. */
    n_unfm_number_to_sync = I_UNFM_NUM(p_s_ih) - *p_n_pos_in_item/*vs*/;

  for ( n_counter = 0; n_counter < n_unfm_number_to_sync; n_counter++ ) {
    /* Found item is not at the path. */
    if ( comp_items(p_s_ih, p_s_path_to_item) )
      return 0;
    /* Calculate block number of the UNFM. */
    n_unfm_pointer = B_I_POS_UNFM_POINTER(PATH_PLAST_BUFFER(p_s_path_to_item),
					  p_s_ih, *p_n_pos_in_item + n_counter);
    /* It is hole. Nothing to sync. */
    if ( ! n_unfm_pointer ) {
      *p_n_synced += 1;
      continue;
    }
    /* Get buffer contains UNFM. */
    n_repeat = CARRY_ON;
    p_s_unfm_bh = reiserfs_get_hash_table(p_s_inode->i_dev, n_unfm_pointer,
					  p_s_inode->i_sb->s_blocksize, &n_repeat);
    /* There is not needed buffer. */
    if ( ! p_s_unfm_bh )  {
      *p_n_synced += 1;
      continue;
    }
    /* Check whether the found item at the path. */
    if ( comp_items(p_s_ih, p_s_path_to_item) ) {
      brelse(p_s_unfm_bh);
      return 0;
    }

    /* Number of the UNFM is changed. */
    if ( n_unfm_pointer != B_I_POS_UNFM_POINTER(PATH_PLAST_BUFFER(p_s_path_to_item), p_s_ih, *p_n_pos_in_item + n_counter) ) {
      brelse(p_s_unfm_bh);
      return 1;
    }

    /* Sync UNFM. */
    if ( reiserfs_sync_block(p_s_unfm_bh, n_wait, NULL) )
      return -1;

    *p_n_synced += 1;

  }
  return 0;
}


/* Sync a reiserfs file item. */
static int  sync_file_item(
              struct inode  * p_s_inode,        /* Pointer to the file inode.             */
              struct path   * p_s_path_to_item, /* Pointer to the path to the found item. */
              int           * p_n_pos_in_item,  /* Position in the found item.            */
              int             n_wait,           /* Sync parameter.                        */
              unsigned long * p_n_synced  /* Returned value. Bytes number were
                                                    synced in this call.            */
            ) {
  struct item_head      s_ih;
  int                   n_ret_value;

  *p_n_synced = 0;
  /* Copy found item header. */
  copy_item_head(&s_ih, PATH_PITEM_HEAD(p_s_path_to_item));

  /* Sync found item. */
  if ( (n_ret_value = reiserfs_sync_block(PATH_PLAST_BUFFER(p_s_path_to_item), n_wait, p_s_path_to_item)) )
    return n_ret_value;

  if ( I_IS_DIRECT_ITEM(&s_ih) )  {
    /* s_ih.ih_item_len bytes was synced. */
    *p_n_synced = s_ih.ih_item_len;
    return 0;
  }
  /* Sync unformatted nodes pointed by the synced indirect item. */
  n_ret_value = sync_unf_nodes(p_s_inode, &s_ih, p_n_pos_in_item,
			       p_s_path_to_item, n_wait, p_n_synced);

#ifdef REISERFS_CHECK
  if (!n_wait &&  ! *p_n_synced && ! n_ret_value )
    reiserfs_warning("sync_file_item: no unformatted nodes were synced (ih %h) pass %d\n", &s_ih, n_wait);
#endif

  *p_n_synced *= p_s_inode->i_sb->s_blocksize;
  return n_ret_value;
}


/* Sync a reiserfs file items. */
static int sync_file_items(
             struct inode  * p_s_inode,
	     int             n_wait
           ) {
  struct key          s_item_key;
  struct path         s_path_to_item;
  int                 n_pos_in_item,
                      n_repeat,
                      n_ret_value = 0;
  unsigned long       n_synced;

  
  init_path (&s_path_to_item);
  /* Form key to search for the first file item. */
  copy_key(&s_item_key, &(p_s_inode->u.reiserfs_i.i_key));
  s_item_key.k_offset = 1;
  if ( 1 == p_s_inode->u.reiserfs_i.i_first_direct_byte )
    s_item_key.k_uniqueness = TYPE_DIRECT;
  else
    s_item_key.k_uniqueness = TYPE_INDIRECT;
  /* While next file item is presented in the tree. */
  while ( search_for_position_by_key(p_s_inode->i_sb, &s_item_key, &s_path_to_item, &n_pos_in_item, &n_repeat) == POSITION_FOUND )  {

#ifdef REISERFS_CHECK
    if ( I_IS_DIRECTORY_ITEM(PATH_PITEM_HEAD(&s_path_to_item)) ||
	 I_IS_STAT_DATA_ITEM(PATH_PITEM_HEAD(&s_path_to_item)) )
      reiserfs_panic (p_s_inode->i_sb, "PAP-16030: sync_file_items: unexpected item type");
#endif

    /* Synchronize the current item. */  
    if ( (n_ret_value = sync_file_item(p_s_inode, &s_path_to_item, &n_pos_in_item,
				       n_wait, &n_synced)) )
      break;
    /* Update key to search for the next file item. */
    if ( (s_item_key.k_offset += n_synced) >= p_s_inode->u.reiserfs_i.i_first_direct_byte )
      s_item_key.k_uniqueness = TYPE_DIRECT;  
  }

  decrement_counters_in_path(&s_path_to_item);
  return n_ret_value;
}



/* Sync a reiserfs file. */
int reiserfs_sync_file(
      struct file   * p_s_filp,
      struct dentry * p_s_dentry
    ) {
  struct inode * p_s_inode = p_s_dentry->d_inode;
  struct reiserfs_transaction_handle th ;
  int n_wait,
      n_err = 0;
  int windex ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 2 ;
  if ( S_ISDIR(p_s_inode->i_mode) )
    reiserfs_panic (p_s_inode->i_sb,
		    "PAP-16040: reiserfs_sync_file: attempt to sync directory using reiserfs_sync_file()");

  if ( ! (S_ISREG(p_s_inode->i_mode) || S_ISLNK(p_s_inode->i_mode)) )
    return -EINVAL;

  /* note, since the transaction can't end while we have the
  ** read_sync_counter incremented (deadlock), sync_file_items is not allowed
  ** to do a polite transaction end.  I've fixed that by 
  ** having it not log anything at all, it only puts unformatted
  ** nodes on the disk, and only if they have not already been logged
  */
  increment_i_read_sync_counter(p_s_inode);
  for ( n_wait = 0; n_wait < 2; n_wait++ )
    n_err |= sync_file_items(p_s_inode, n_wait);
  decrement_i_read_sync_counter(p_s_inode);

  /* now we sync the inode, and commit the transaction.
  ** this will commit any logged blocks involved with this file
  */
  if (reiserfs_inode_in_this_transaction(p_s_inode) || 
      p_s_inode->i_state & I_DIRTY) {
    journal_begin(&th, p_s_inode->i_sb, jbegin_count) ;
    windex = push_journal_writer("reiserfs_sync_file") ;
    n_err |= reiserfs_sync_inode(&th, p_s_inode);
    pop_journal_writer(windex) ;
    journal_end_sync(&th, th.t_super,jbegin_count) ;
  } else {
    reiserfs_commit_for_inode(p_s_inode) ;
  }
  return ( n_err < 0 ) ? -EIO : 0;
}

#endif /* __KERNEL__ */

