/*
 * Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */
#ifdef __KERNEL__

#include <linux/sched.h>
#include <linux/reiserfs_fs.h>
#include <linux/locks.h>
#include <asm/uaccess.h>

#else

#include "nokernel.h"
int reiserfs_notify_change(struct dentry * dentry, struct iattr * attr){return 0;}

#endif


void reiserfs_delete_inode (struct inode * inode)
{
  int writers_to_wait_for ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 2; 
  int windex ;
  struct reiserfs_transaction_handle th ;

  writers_to_wait_for = 1; 
  journal_begin(&th, inode->i_sb, jbegin_count) ;
  windex = push_journal_writer("delete_inode") ;
  reiserfs_update_inode_transaction(inode) ;

  /* The = 0 happens when we abort creating a new inode for some reason like lack of space.. */
  if (INODE_PKEY(inode)->k_objectid != 0) {
    reiserfs_delete_object (&th, inode);
    reiserfs_release_objectid (&th, inode->i_ino, inode->i_sb);
  } else {
    /* no object items are in the tree */
    ;
  }
  pop_journal_writer(windex) ;
  journal_end(&th, inode->i_sb, jbegin_count) ;
  clear_inode (inode); /* note this must go after the journal_end to prevent deadlock */
}

#if 0
static void copy_data_blocks_to_inode (struct inode * inode, struct item_head * ih, __u32 * ind_item)
{
  int first_log_block = (ih->ih_key.k_offset - 1) / inode->i_sb->s_blocksize; /* first log block addressed by indirect item */
  int i, j;
  
  for (i = first_log_block, j = 0; i < REISERFS_N_BLOCKS && j < I_UNFM_NUM (ih); i ++, j ++) {
#ifdef REISERFS_CHECK
    if (inode->u.reiserfs_i.i_data [i] && inode->u.reiserfs_i.i_data [i] != ind_item [j])
      reiserfs_panic (inode->i_sb, "vs-13000: reiserfs_bmap: log block %d, data block %d is seet and doe not match to unfmptr %d",
		      i, inode->u.reiserfs_i.i_data [i], ind_item [j]);
#endif
    inode->u.reiserfs_i.i_data [i] = ind_item [j];
  }
}
#endif/*0*/

/* convert logical file block to appropriate unformatted node. */
int reiserfs_bmap (struct inode * inode, int block)
{
  struct key offset_key;
  struct path path_to_blocknr;
  int pos_in_item;
  int repeat;
  struct buffer_head * bh;
  struct item_head * ih;
  int blocknr;

  offset_key.k_offset = block * inode->i_sb->s_blocksize + 1;
  if (INODE_OFFSET_IN_DIRECT (inode, offset_key.k_offset)) {
    return 0;
  }

  /*
  if (block < REISERFS_N_BLOCKS && inode->u.reiserfs_i.i_data [block]) {
    inode->i_sb->u.reiserfs_sb.s_bmaps_without_search ++;
    return inode->u.reiserfs_i.i_data [block];
  }
  */


  copy_short_key (&offset_key, INODE_PKEY (inode));
  offset_key.k_uniqueness = TYPE_INDIRECT;

  init_path (&path_to_blocknr);
  if (search_for_position_by_key (inode->i_sb, &offset_key, &path_to_blocknr, &pos_in_item, &repeat) == POSITION_NOT_FOUND) {
    /*reiserfs_warning ("vs-13020: reiserfs_bmap: there is no required byte (%k) in the file of size %ld. Found item \n%h\n", 
      &offset_key, inode->i_size, PATH_PITEM_HEAD (&path_to_blocknr));*/
    pathrelse (&path_to_blocknr);
    return 0;
  }

  bh = PATH_PLAST_BUFFER (&path_to_blocknr);
  ih = B_N_PITEM_HEAD (bh, PATH_LAST_POSITION (&path_to_blocknr));

  if (I_IS_INDIRECT_ITEM (ih)) {
    __u32 * ind_item = (__u32 *)B_I_PITEM (bh, ih);

    /*
    copy_data_blocks_to_inode (inode, ih, ind_item);
    */
    blocknr = ind_item [pos_in_item];
    pathrelse (&path_to_blocknr);
    inode->i_sb->u.reiserfs_sb.s_bmaps ++;
    return blocknr;
  }

  reiserfs_warning ("vs-13030: reiserfs_bmap: found item \n%h\n is not indirect one\n", ih);
  pathrelse (&path_to_blocknr);
  return 0;
}


#define has_tail(inode) ((inode)->u.reiserfs_i.i_first_direct_byte != NO_BYTES_IN_DIRECT_ITEM)
#define tail_offset(inode) ((inode)->u.reiserfs_i.i_first_direct_byte - 1)

/* "we are reading into the page cache, not into any process's virtual
   memory". Stephen C. Tweedie, therefore no need for local buffer */
static int read_file_tail (struct inode * inode, loff_t offset, char * buf, int size)
{
  struct key key;
  int repeat, pos_in_item;
  struct path path;
  struct item_head * ih;
  char * p;
  int chars, left, read;
  struct buffer_head * bh;
  int block;
  struct buffer_head * pbh[PAGE_SIZE / 512];
  int i, j;

  p = buf;
  left = size;
  read = 0;

  /* read unformatted nodes first */
  i = 0;
  while (offset < tail_offset (inode)) {
    block = offset >> inode->i_sb->s_blocksize_bits;
    block = inode->i_op->bmap(inode, block);
    pbh[i] = getblk (inode->i_dev, block, inode->i_sb->s_blocksize);
    offset += inode->i_sb->s_blocksize;
    i ++;
  }
  
  if (i)
    ll_rw_block (READ, i, pbh);

  p = buf;
  for (j = 0; j < i; j ++) {
    wait_on_buffer (pbh[j]);
    memcpy (p, pbh[j]->b_data, pbh[j]->b_size);
    read += pbh[j]->b_size;
    left -= pbh[j]->b_size;
    p += pbh[j]->b_size;
    brelse (pbh[j]);
  }


  /* read direct item(s) */

  if (offset != tail_offset (inode)) {
#ifdef REISERFS_CHECK
    reiserfs_warning ("vs-18010: read_file_tail: given offset (%d) is not stored in direct item. \
Inode's first direct byte %d\n", offset, inode->u.reiserfs_i.i_first_direct_byte);
#endif
    return 0;
  }

  init_path (&path);

  copy_short_key (&key, INODE_PKEY(inode));
  key.k_offset = offset + 1;
  key.k_uniqueness = TYPE_DIRECT;

  while (left) {
    if (search_for_position_by_key (inode->i_sb, &key, &path, &pos_in_item, &repeat) == POSITION_NOT_FOUND) {
      break;
    }

    bh = PATH_PLAST_BUFFER (&path);
    ih = B_N_PITEM_HEAD (bh, PATH_LAST_POSITION (&path));

    chars = ih->ih_item_len - pos_in_item;
    if (chars > left)
      chars = left;

    memcpy (p, B_I_PITEM (bh, ih) + pos_in_item, chars);
    key.k_offset += chars;
    read += chars;
    left -= chars;
    p += chars;
    if (PATH_LAST_POSITION (&path) != B_NR_ITEMS (bh) - 1)
      /* that was last direct item of the tail */
      break;
  }

  pathrelse (&path);
  return read;
}


int reiserfs_readpage (struct file * file, struct page * page) 
{
  struct inode * inode;

				/* If you get this from the page it is one less indirection -Hans */
  inode = file->f_dentry->d_inode;

  if (has_tail (inode) && tail_offset (inode) < page->offset + PAGE_SIZE) {
    /* there is a tail and it is in this page */
    memset ((char *)page_address (page), 0, PAGE_SIZE);
    read_file_tail (inode, page->offset, (char *)page_address (page), PAGE_SIZE);
    set_bit (PG_uptodate, &page->flags);
    return 0;
  }  else {
    return generic_readpage (file, page);
  }
}

/* Iget accepts only super block and inode number as it hashes inodes
   using device identifier and inode number. If iget could not find
   required inode in its hash queues, then it calls
   reiserfs_read_inode passing to it only inode
   number. Reiserfs_read_inode must know the key. That is why we keep
   key in global array before iget.
   */

#define KEY_ARRAY_SIZE 100

static struct {
  unsigned long objectid;
  unsigned long dirid;
  kdev_t dev ;
} g_key_array [KEY_ARRAY_SIZE] = {{0,},};


static unsigned long look_for_key  (struct super_block *s, unsigned long objectid)
{
  int i;

  for (i = 0; i < sizeof (g_key_array) / sizeof (g_key_array[0]); i ++) {
    if (g_key_array[i].objectid == objectid && 
        g_key_array[i].dev == s->s_dev)
      return g_key_array[i].dirid;
  }
  return (unsigned long)-1;
}


/* looks for stat data in the tree, and fills up the fields of in-core
   inode stat data fields */
void reiserfs_read_inode (struct inode * inode)
{
  struct path path_to_sd;
  struct stat_data * sd;
  struct item_head *ih ;
  int repeat;

  init_path (&path_to_sd);

  inode->i_op = NULL;
  inode->i_mode = 0;

  /* form key of the stat data */
  inode->u.reiserfs_i.i_key[0] = look_for_key (inode->i_sb, inode->i_ino);
  inode->u.reiserfs_i.i_key[1] = inode->i_ino;
  inode->u.reiserfs_i.i_key[2] = SD_OFFSET;
  inode->u.reiserfs_i.i_key[3] = TYPE_STAT_DATA;
  if (inode->u.reiserfs_i.i_key[0] == (unsigned long)-1) {
#ifdef REISERFS_CHECK
    reiserfs_warning ("vs-13040: reiserfs_read_inode: could not find k_dir_id (objectid = %lu)\n",
		    inode->i_ino);
#endif
    /* VERY slow search through all possible packing localities. */
    if (search_by_objectid(inode->i_sb, INODE_PKEY (inode), &path_to_sd, &repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) != ITEM_FOUND) {
      pathrelse (&path_to_sd);
      make_bad_inode(inode) ;
      return ;
    }

    /* ok, the search found our item.  Pull the packing locality from 
    ** the path it returned
    */
    ih = PATH_PITEM_HEAD(&path_to_sd) ;
    inode->u.reiserfs_i.i_key[0] = ih->ih_key.k_dir_id ;
  }

  /* look for the object stat data */
  if (search_by_key (inode->i_sb, INODE_PKEY (inode), &path_to_sd, &repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) == ITEM_NOT_FOUND) {
    pathrelse (&path_to_sd);
    make_bad_inode(inode) ;
    return;
  }

  sd = B_N_STAT_DATA (PATH_PLAST_BUFFER (&path_to_sd), PATH_LAST_POSITION (&path_to_sd));
  inode->i_mode = le16_to_cpu (sd->sd_mode);
  inode->i_uid = le16_to_cpu (sd->sd_uid);
  inode->i_gid = le16_to_cpu (sd->sd_gid);
  inode->i_nlink = le16_to_cpu (sd->sd_nlink);
  inode->i_size = le32_to_cpu (sd->sd_size);
  inode->i_mtime = le32_to_cpu (sd->sd_mtime);
  inode->i_atime = le32_to_cpu (sd->sd_atime);
  inode->i_ctime = le32_to_cpu (sd->sd_ctime);
  inode->i_blksize = inode->i_sb->s_blocksize;
  /* for regular files we calculate any tail as full block */
  /* inode->i_blocks = (sd->sd_first_direct_byte == NO_BYTES_IN_DIRECT_ITEM) ? (inode->i_size / 512) : 
    ((sd->sd_first_direct_byte - 1) / 512 + inode->i_blksize / 512); */

  if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
    inode->i_rdev = le32_to_cpu (sd->u.sd_rdev);
  else {
    inode->i_blocks = le32_to_cpu (sd->u.sd_blocks);
    if (inode->i_blocks == 0) { /* compatibility for 3.5.16 and before */
      /* for regular files we calculate any tail as full block */
      inode->i_blocks = (sd->sd_first_direct_byte == 
                         NO_BYTES_IN_DIRECT_ITEM) ? (inode->i_size / 512) : 
			((sd->sd_first_direct_byte - 1) / 512 + 
			inode->i_blksize / 512); 
    }
  }
  inode->u.reiserfs_i.i_first_direct_byte=le32_to_cpu(sd->sd_first_direct_byte);
  pathrelse (&path_to_sd);

  if (S_ISREG (inode->i_mode))
    inode->i_op = &reiserfs_file_inode_operations;
  else if (S_ISDIR (inode->i_mode)) {
    inode->i_op = &reiserfs_dir_inode_operations;
    inode->i_blocks = inode->i_size / 512 + ((inode->i_size % 512) ? 1 : 0);
  } else if (S_ISLNK (inode->i_mode))
    inode->i_op = &reiserfs_symlink_inode_operations;
  else if (S_ISCHR (inode->i_mode))
    inode->i_op = &chrdev_inode_operations;
  else if (S_ISBLK (inode->i_mode))
    inode->i_op = &blkdev_inode_operations;
  else if (S_ISFIFO (inode->i_mode))
    init_fifo (inode);
  inode->u.reiserfs_i.i_pack_on_close = 0 ;

}


void store_key (struct super_block *s, struct key * key)
{
  int i;

  for (i = 0; i < sizeof (g_key_array) / sizeof (g_key_array[0]); i ++) {
    if (g_key_array[i].objectid == 0) {
      g_key_array[i].dirid = key->k_dir_id;
      g_key_array[i].objectid = key->k_objectid;
      g_key_array[i].dev = s->s_dev ;
      return;
    }
  }
  reiserfs_warning ("vs-13042: store_key: table of keys is full\n");
}

void forget_key (struct super_block *s, struct key * key)
{
  int i;

  for (i = 0; i < sizeof (g_key_array) / sizeof (g_key_array[0]); i ++) {
    if (g_key_array[i].objectid == key->k_objectid &&
        g_key_array[i].dev == s->s_dev) {
      g_key_array[i].objectid = 0;
      return;
    }
  }
  reiserfs_warning ("vs-13045: forget_key: could not find key in the table [%k]\n", key);
}


struct inode * reiserfs_iget (struct super_block * s, struct key * key)
{
  struct inode * inode;

  store_key (s, key);
  inode = iget (s, key->k_objectid);
  if (comp_short_keys (INODE_PKEY (inode), key)) {
    reiserfs_warning ("vs-13048: reiserfs_iget: key in inode %k and key in entry %k do not match\n",
		      INODE_PKEY (inode), key);
    iput (inode);
    inode = 0;
  }
  forget_key (s, key);
  return inode;
}


static struct buffer_head * reiserfs_update_inode (struct reiserfs_transaction_handle *th, struct inode * inode, 
                  			           struct path * path_to_sd, int read_blocks)
{
  struct stat_data * sd;
  int repeat;
  struct buffer_head * bh;


  init_path (path_to_sd);

  /* look for the object */
  if (search_by_key (inode->i_sb, INODE_PKEY (inode), path_to_sd, &repeat, DISK_LEAF_NODE_LEVEL, read_blocks) == ITEM_NOT_FOUND) {
    if (read_blocks == DONT_READ_BLOCKS) {
      /* this is called from if_in_ram_update_sd */
      /*printk ("reiserfs: stat data not found in memory\n");*/
      return 0;
    }
    if (inode->i_nlink == 0) {
#ifdef REISERFS_CHECK 
      printk ("vs-13050: reiserfs_update_inode: i_nlink == 0, stat data not found\n");
#endif
      return 0;
    }
    print_block (PATH_PLAST_BUFFER (path_to_sd), PRINT_LEAF_ITEMS, -1, -1);
    reiserfs_panic(inode->i_sb, "vs-13060: reiserfs_update_inode: stat data of object %k (nlink == %d) not found (pos %d)\n", 
		   INODE_PKEY (inode), inode->i_nlink, PATH_LAST_POSITION (path_to_sd));
  }
  bh = PATH_PLAST_BUFFER (path_to_sd);
  sd = B_N_STAT_DATA (bh, PATH_LAST_POSITION (path_to_sd));
  sd->sd_mode = cpu_to_le16 (inode->i_mode);
  sd->sd_uid = cpu_to_le16 (inode->i_uid);
  sd->sd_gid = cpu_to_le16 (inode->i_gid);
  sd->sd_nlink = cpu_to_le16 (inode->i_nlink);
  sd->sd_size = cpu_to_le32 (inode->i_size);
  sd->sd_atime = cpu_to_le32 (inode->i_atime);
  sd->sd_ctime = cpu_to_le32 (inode->i_ctime);
  sd->sd_mtime = cpu_to_le32 (inode->i_mtime);
  if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
    sd->u.sd_rdev = cpu_to_le32 (inode->i_rdev);
  else sd->u.sd_blocks = cpu_to_le32 (inode->i_blocks);
  sd->sd_first_direct_byte = cpu_to_le32 (inode->u.reiserfs_i.i_first_direct_byte);
  /* reiserfs_mark_buffer_dirty (bh, 1); journal victim */
  reiserfs_update_inode_transaction(inode) ;
  journal_mark_dirty(th, inode->i_sb, bh);
  return bh;
}

/* looks for stat data, then copies fields to it, marks the buffer
   containing stat data as dirty */
void reiserfs_write_inode (struct inode * inode)
{
  struct path path_to_sd;
  int windex ;
  struct reiserfs_transaction_handle th ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3; 

  journal_begin(&th, inode->i_sb, jbegin_count) ;
  windex = push_journal_writer("write_inode") ;
  reiserfs_update_inode_transaction(inode) ;
  reiserfs_update_inode (&th, inode, &path_to_sd, READ_BLOCKS);
  pathrelse (&path_to_sd);
  pop_journal_writer(windex) ;
  journal_end(&th, inode->i_sb, jbegin_count) ;
}


int reiserfs_sync_inode (struct reiserfs_transaction_handle *th, struct inode * inode)
{
  int err = 0;
  struct path path_to_sd;
  struct buffer_head * bh;

  bh = reiserfs_update_inode (th, inode, &path_to_sd, READ_BLOCKS);
#if 0
  if (bh && buffer_dirty (bh)) {
    ll_rw_block(WRITE, 1, &bh);
    wait_on_buffer(bh);
    if (buffer_req(bh) && !buffer_uptodate(bh)) {
      printk ("reiserfs_sync_inode: IO error syncing reiserfs stat data ["
	      "device:\"%s\", object:[%u %u]], blocknr:%lu\n",
	      kdevname(inode->i_dev), inode->u.reiserfs_i.i_key[0], inode->u.reiserfs_i.i_key[1],
	      bh->b_blocknr);
      err = -1;
    }
  } else {
    if (bh == 0)
      err = -1;
  }
#endif
  pathrelse (&path_to_sd);
  return err;
}


/* stat data of object has been inserted, this inserts the item
   containing "." and ".." entries */
static int reiserfs_new_directory (struct reiserfs_transaction_handle *th, 
			           struct super_block * sb, struct item_head * ih, struct path * path, const struct inode * dir)
{
  char empty_dir [EMPTY_DIR_SIZE];
  struct reiserfs_de_head * deh;
  char * body;
  int repeat;
	
  /* item head of empty directory item */
  ih->ih_key.k_offset = DOT_OFFSET;
  ih->ih_key.k_uniqueness = DIRENTRY_UNIQUENESS;
  ih->u.ih_entry_count = 2;
  ih->ih_item_len = EMPTY_DIR_SIZE;

  body = empty_dir;
  deh = (struct reiserfs_de_head *)body;

#ifdef REISERFS_ALIGNED
  deh[0].deh_location = ih->ih_item_len - 4;
#else
  deh[0].deh_location = ih->ih_item_len - strlen (".");
#endif
  deh[0].deh_offset = DOT_OFFSET;
  deh[0].deh_dir_id = ih->ih_key.k_dir_id;
  deh[0].deh_objectid = ih->ih_key.k_objectid;
  mark_de_without_sd (&(deh[0]));
  mark_de_visible (&(deh[0]));
#ifdef REISERFS_ALIGNED
  strncpy( body + deh[0].deh_location, ".", 4 );
#else
  body[deh[0].deh_location] = '.';
#endif

#ifdef REISERFS_ALIGNED
  deh[1].deh_location = deh[0].deh_location - 4;
#else
  deh[1].deh_location = deh[0].deh_location - strlen ("..");
#endif
  deh[1].deh_offset = DOT_DOT_OFFSET;

  /* objectid of ".." directory */
  deh[1].deh_dir_id = INODE_PKEY (dir)->k_dir_id;
  deh[1].deh_objectid = INODE_PKEY (dir)->k_objectid;
  mark_de_without_sd (&(deh[1]));
  mark_de_visible (&(deh[1]));
#ifdef REISERFS_ALIGNED
  strncpy( body + deh[1].deh_location, "..", 4 );
#else
  body[deh[1].deh_location] = '.';
  body[deh[1].deh_location + 1] = '.';
#endif

  /* look for place in the tree for new item */
  if (search_by_key (sb, &ih->ih_key,  path, &repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) == ITEM_FOUND)
    reiserfs_panic (sb, "vs-13070: reiserfs_new_directory: object with this key exists [%lu %lu %lu %lu]",
		    ih->ih_key.k_dir_id, ih->ih_key.k_objectid, ih->ih_key.k_offset, ih->ih_key.k_uniqueness);

  /* insert item, that is empty directory item */
  return reiserfs_insert_item (th, sb, path, ih, body, REISERFS_KERNEL_MEM, 0, NOTHING_SPECIAL);
}


/* stat data of object has been inserted, this inserts the item
   containing the body of symlink */
static int reiserfs_new_symlink (struct reiserfs_transaction_handle *th, 
                                 struct super_block * sb, struct item_head * ih, struct path * path, const char * symname)
{
  int repeat;

  /* item head of the body of symlink */
  ih->ih_key.k_offset = 1;
  ih->ih_key.k_uniqueness = TYPE_DIRECT;
  ih->ih_item_len = strlen (symname);

  /* look for place in the tree for new item */
  if (search_by_key (sb, &ih->ih_key, path, &repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) == ITEM_FOUND)
    reiserfs_panic (sb, "vs-13080: reiserfs_new_symlink: object with this key exists [%lu %lu %lu %lu]",
		    ih->ih_key.k_dir_id, ih->ih_key.k_objectid, ih->ih_key.k_offset, ih->ih_key.k_uniqueness);

  /* insert item, that is body of symlink */
  return reiserfs_insert_item (th, sb, path, ih, symname, REISERFS_KERNEL_MEM, 0, NOTHING_SPECIAL);
}


/* inserts the stat data into the tree, and then calls reiserfs_new_directory
   (to insert ".", ".." item if new object is directory) or
   reiserfs_new_symlink (to insert symlink body if new object is
   symlink) or nothing (if new object is regular file) */
struct inode * reiserfs_new_inode (struct reiserfs_transaction_handle *th,
				   const struct inode * dir, int mode, const char * symname, 
				   struct dentry *dentry, struct inode *inode, int * err)
{
    struct super_block * sb;
    /* struct inode * inode; */
    struct path path_to_key;
    struct item_head ih;
    struct stat_data sd;
    int retvalue;
    int repeat;

    init_path (&path_to_key);

    /* if (!dir || !(inode = get_empty_inode())) journal victim */
    if (!dir || !dir->i_nlink) { 
	iput(inode) ;
	*err = -EPERM;
	return NULL;
    } 
    sb = dir->i_sb;
    inode->i_sb = sb;
    inode->i_flags = inode->i_sb->s_flags;

    ih.ih_key.k_dir_id = INODE_PKEY (dir)->k_objectid;
    ih.ih_key.k_objectid = reiserfs_get_unused_objectid (th, dir->i_sb);
    ih.ih_key.k_offset = SD_OFFSET;
    ih.ih_key.k_uniqueness = TYPE_STAT_DATA;
    ih.u.ih_free_space = MAX_US_INT;
    ih.ih_item_len = SD_SIZE;

    /* free preserve list if we should */
/*    maybe_free_preserve_list (dir->i_sb);*/

    /* find proper place for inserting of stat data */
    if (search_by_key (sb, &ih.ih_key, &path_to_key, &repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) == ITEM_FOUND) {
	pathrelse (&path_to_key);
	iput (inode);
	*err = -EEXIST;
	return 0;
    }

    /* fill stat data */
    sd.sd_mode = inode->i_mode = mode;
    if (mode & S_IFDIR) {
	sd.sd_nlink = inode->i_nlink = 2;
	sd.sd_size = inode->i_size = EMPTY_DIR_SIZE;
	inode->i_blocks = EMPTY_DIR_SIZE / 512 + ((EMPTY_DIR_SIZE % 512) ? 1 : 0);
    } else {
	sd.sd_nlink = inode->i_nlink = 1;
	sd.sd_size = inode->i_size = 0;
    }
    sd.sd_uid = inode->i_uid = current->fsuid;
    sd.sd_gid = inode->i_gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current->fsgid;
  
    sd.sd_mtime = sd.sd_atime = sd.sd_ctime = inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
    if ((mode & S_IFCHR) || (mode & S_IFBLK))
		sd.u.sd_rdev = 0;
    else sd.u.sd_blocks = inode->i_blocks = 0;

    sd.sd_first_direct_byte = inode->u.reiserfs_i.i_first_direct_byte = S_ISLNK(mode) ? 1 : NO_BYTES_IN_DIRECT_ITEM;
    
    /* insert the stat data into the tree */
    retvalue = reiserfs_insert_item (th, sb, &path_to_key, &ih, (char *)(&sd), REISERFS_KERNEL_MEM, 0, NOTHING_SPECIAL);
    if (retvalue == NO_DISK_SPACE) {
	iput (inode);
	*err = -ENOSPC;
	return 0;
    }

    if (S_ISDIR(mode)) {
	/* insert item with "." and ".." */
	retvalue = reiserfs_new_directory (th, sb, &ih, &path_to_key, dir);
    }

    if (S_ISLNK(mode)) {
	/* insert body of symlink */
	retvalue = reiserfs_new_symlink (th, sb, &ih, &path_to_key, symname);
    }
    if (retvalue == NO_DISK_SPACE) {
	/* we must delete stat data here */
	memcpy (INODE_PKEY (inode), &(ih.ih_key), SHORT_KEY_SIZE);
	iput (inode);
	*err = -ENOSPC;
	return 0;
    }

    inode->i_dev = sb->s_dev;
    inode->i_ino = ih.ih_key.k_objectid;
    inode->i_op = NULL;
    inode->i_blocks = 0;
    inode->i_blksize = sb->s_blocksize;

    memcpy (INODE_PKEY (inode), &(ih.ih_key), SHORT_KEY_SIZE);
    insert_inode_hash (inode);
/*  mark_inode_dirty (inode);*/
    return inode;
}


extern spinlock_t inode_lock;

                                /* This is the problem we are trying to solve: with reiserfs it is a bad thing to defer
                                an update of stat data if the buffer holding the stat data is in RAM. If we defer it,
                                then when we later do the update, the buffer might not still be in RAM.  Consider that
                                inodes have a longer lifetime in RAM, and you can see that this is especially important.

                                Unfortunately, sometimes we need to pass an inode, sometimes an iattr, sometimes just
                                one field, to the function that updates the stat data.  Umpteen different functions that
                                do essentially the same thing.  For now I implement just a straight replacement for
                                mark_inode_dirty -Hans */
                                /* This function is inefficient if only one field of the inode was changed. 
                                 This function can cause schedule.  */
void if_in_ram_update_sd (struct reiserfs_transaction_handle *th, struct inode * inode)
{
  struct path path_to_sd;

  reiserfs_update_inode (th, inode, &path_to_sd, READ_BLOCKS);
  pathrelse (&path_to_sd);
  return ;

#ifdef DIRTY_LATER
  mark_inode_dirty(inode);
  return;
#else /* DIRTY_LATER */

  struct path path_to_sd;
  extern struct list_head inode_in_use;


  init_path (&path_to_sd);
  
  if (!inode->i_nlink || (S_ISFIFO (inode->i_mode))) { 
    mark_inode_dirty(inode);    /* let us deferr the update since in this case we always follow this with an iput which
                                   will do the update (that is to say, the removal) of the stat data */
    return;
  }

#ifdef REISERFS_CHECK
  if (inode->i_sb) {
#endif /* REISERFS_CHECK */

    /* wait until inode is not locked and then lock it */
    while ((inode->i_state & I_LOCK)) {
      __wait_on_inode(inode);
    }
    inode->i_state |=  I_LOCK;

    if (reiserfs_update_inode (th, inode, &path_to_sd, DONT_READ_BLOCKS)) {
      spin_lock(&inode_lock);   /* locks inode lists (not the inode) */
      if ((inode->i_state & I_DIRTY)) {
        struct list_head *insert = &inode_in_use;

        /* mark inode clean and take it off dirty list */
        inode->i_state &= ~I_DIRTY;
#ifdef REISERFS_CHECK
        /* Only add valid (ie hashed) inodes to the in_use list */
        if (!list_empty(&inode->i_hash)) {
#endif /* REISERFS_CHECK */
          list_del(&inode->i_list);
          list_add(&inode->i_list, insert);
#ifdef REISERFS_CHECK
        } 
        else 
          printk("reiser-1805: if_in_ram_update_sd: a dirty inode was not on any inode list\n");
#endif /* REISERFS_CHECK */
      }
      spin_unlock(&inode_lock);
      /*mark_buffer_dirty (bh, 1);*/
    } else {   /* stat data item for this inode no longer in RAM */
      /* if stat data was not found then it must be because it is no longer in RAM, so just mark it
	 dirty for now, and let somebody else write it */
      mark_inode_dirty(inode); 
    }  
    inode->i_state &= ~I_LOCK;
    wake_up(&inode->i_wait);
    pathrelse(&path_to_sd);
#ifdef REISERFS_CHECK
  }
  else
    printk("reiser-1804: if_in_ram_update_sd: !sb, should not happen\n");
#endif /* REISERFS_CHECK */

#endif /* ! DIRTY_LATER */
}


#ifdef __KERNEL__
/* this generates little if any speedup compared to if_in_ram_update_sd(), might not be worthwhile
   code, probably search_by_key dominates cpu consumption */
void if_in_ram_update_some_sd (struct reiserfs_transaction_handle *th, struct inode * inode,  struct iattr * attr)
{
  struct stat_data * sd;
  struct path path_to_sd;
  int repeat;
  struct buffer_head * bh;
  extern struct list_head inode_in_use;
  unsigned int ia_valid = attr->ia_valid;

  init_path (&path_to_sd);

  if (!inode->i_nlink || (S_ISFIFO (inode->i_mode))) { /* Vladimir, what do you think, is the S_FIFO needed here?-Hans */
    mark_inode_dirty(inode);    /* let us deferr the update since in this case we always follow this with an iput which
                                   will do the update (that is to say, the removal) of the stat data */
    return;
  }

#ifdef REISERFS_CHECK
  if (inode->i_sb) {
#endif /* REISERFS_CHECK */

                                /* wait until inode is not locked and then lock it */
    while ((inode->i_state & I_LOCK)) {
      __wait_on_inode(inode);
    }
    inode->i_state |=  I_LOCK;  /* depends on interrupts never locking inodes, I suppose it is correct.... */

    /* look for the stat data item, if we find it in RAM, sync to it now, else put the inode on the list of dirty inodes
       to be synced someday using mark_inode_dirty and perhaps the sync will happen when the buffer is back in RAM or
       after a bunch of other changes to it have been successfully cached by deferring the I/O */
    if (search_by_key (inode->i_sb, INODE_PKEY (inode), &path_to_sd, &repeat, DISK_LEAF_NODE_LEVEL, DONT_READ_BLOCKS) == ITEM_FOUND) {
                                /* all of these branches are very inefficient, but the search_by_key is probably the
                                   worst of it */
      bh = PATH_PLAST_BUFFER (&path_to_sd);
      sd = B_N_STAT_DATA (bh, PATH_LAST_POSITION (&path_to_sd));
        if (ia_valid & ATTR_MODE)
          sd->sd_mode = cpu_to_le16 (inode->i_mode);
        if (ia_valid & ATTR_UID)
          sd->sd_uid = cpu_to_le16 (inode->i_uid);
        if (ia_valid & ATTR_GID)
          sd->sd_gid = cpu_to_le16 (inode->i_gid);
        if (ia_valid & ATTR_SIZE)
          sd->sd_size = cpu_to_le32 (inode->i_size);
        if (ia_valid & ATTR_MTIME)
          sd->sd_mtime = cpu_to_le32 (inode->i_mtime);
        if (ia_valid & ATTR_CTIME)
          sd->sd_ctime = cpu_to_le32 (inode->i_ctime);
      spin_lock(&inode_lock);   /* locks inode lists (not the inode) */
      if ((inode->i_state & I_DIRTY)) {
        struct list_head *insert = &inode_in_use;

        /*      mark inode clean and take it off dirty list */
        inode->i_state &= ~I_DIRTY;
#ifdef REISERFS_CHECK
        /* Only add valid (ie hashed) inodes to the in_use list */
        if (!list_empty(&inode->i_hash)) {
#endif /* REISERFS_CHECK */
          list_del(&inode->i_list);
          list_add(&inode->i_list, insert);
#ifdef REISERFS_CHECK
        }
        else 
          printk("reiser-1806: if_in_ram_update_some_sd: a dirty inode was not on any inode list, maybe a pipe?");
#endif /* REISERFS_CHECK */
      }
      spin_unlock(&inode_lock);
      /* mark_buffer_dirty (bh, 1); journal victim */
      journal_mark_dirty (th, inode->i_sb, bh);
    } else {   /* stat data item for this inode no longer in RAM */
      inode->i_state &= ~I_LOCK; /* probably should combine two lines that unlock inode by postponing the unlock.... */
                                /* if inode was not found then it must be because it is no longer in RAM, so just
                                   mark it dirty for now, and let somebody else write it */
      mark_inode_dirty(inode); 
    }  
    inode->i_state &= ~I_LOCK;
    wake_up(&inode->i_wait);
    pathrelse(&path_to_sd);
#ifdef REISERFS_CHECK
  }
  else
    printk("reiser-1807: if_in_ram_update_some_sd: !sb, should not happen");
#endif /* REISERFS_CHECK */
}


void inline reiserfs_inode_setattr(struct reiserfs_transaction_handle *th, struct inode * inode, struct iattr * attr)
{
        unsigned int ia_valid = attr->ia_valid;
	/* struct path path_to_sd; */

        if (ia_valid & ATTR_UID)
                inode->i_uid = attr->ia_uid;
        if (ia_valid & ATTR_GID)
                inode->i_gid = attr->ia_gid;
        if (ia_valid & ATTR_SIZE)
                inode->i_size = attr->ia_size;
        if (ia_valid & ATTR_ATIME)
                inode->i_atime = attr->ia_atime;
        if (ia_valid & ATTR_MTIME)
                inode->i_mtime = attr->ia_mtime;
        if (ia_valid & ATTR_CTIME)
                inode->i_ctime = attr->ia_ctime;
        if (ia_valid & ATTR_MODE) {
                inode->i_mode = attr->ia_mode;
                if (!in_group_p(inode->i_gid) && !capable(CAP_FSETID))
                        inode->i_mode &= ~S_ISGID;
        }
#ifdef DIRTY_LATER
        mark_inode_dirty(inode) ;  
	/* reiserfs_update_inode (inode, &path_to_sd, READ_BLOCKS); 
	pathrelse (&path_to_sd); */
#else /* note, this code is broken and MUST not be used */
        if_in_ram_update_some_sd (th, inode, attr);
#endif
}


int reiserfs_notify_change(struct dentry * dentry, struct iattr * attr)
{
        struct inode *inode = dentry->d_inode;
	struct reiserfs_transaction_handle th ;
        int error;

	/* I'm cheating here.  reiserfs_inode_setattr does not make journal calls with
	** dirty later turned on.  Dirty later must be on for now, so I'm not doing
	** a journal_begin here.
	*/
	th.t_trans_id = 0 ; 
        error = inode_change_ok(inode, attr);
        if (!error)
          reiserfs_inode_setattr(&th, inode, attr);
        return error;
}

/* I believe that further optimizing this code is best done by optimizing search_by_key.. */

#endif /* __KERNEL__ */
