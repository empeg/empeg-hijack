/*
 * Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */
#ifdef __KERNEL__

#include <linux/sched.h>
#include <linux/reiserfs_fs.h>

#else

#include "nokernel.h"

#endif



#define MAX_GEN_NUMBER  127


#define SET_GENERATION_NUMBER(offset,gen_number) (GET_HASH_VALUE(offset)|(gen_number))


int bin_search_in_dir_item (struct item_head * ih, struct reiserfs_de_head * deh, struct key * key, int * pos_in_item)
{
  int rbound, lbound, j;

  lbound = 0;
  rbound = I_ENTRY_COUNT (ih) - 1;

  for (j = (rbound + lbound) / 2; lbound <= rbound; j = (rbound + lbound) / 2) {
    if (key->k_offset < deh[j].deh_offset) {
      rbound = j - 1;
      continue;
    }
    if (key->k_offset > deh[j].deh_offset) {
      lbound = j + 1;
      continue;
    }
    /* key found */
    *pos_in_item = j;
    return POSITION_FOUND;
  }

  *pos_in_item = lbound;
  return POSITION_NOT_FOUND;
}


/* first calls search_by_key, then, if item is not found looks for the entry inside directory item indicated by
   search_by_key. (We assign a key to each directory item, and place multiple entries in a single directory item.)
   Fills the path to the entry, and to the entry position in the item */
int search_by_entry_key (struct super_block * sb, struct key * key, struct path * path, int * pos_in_item, int * repeat)
{
  /* search for a directory item using key of entry */
  if (search_by_key (sb, key, path, repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) == ITEM_FOUND) {
    *pos_in_item = 0;
    return POSITION_FOUND;
  }
#ifdef REISERFS_CHECK
  if (!PATH_LAST_POSITION (path))
    reiserfs_panic (sb, "vs-7000: search_by_entry_key: search_by_key returned item position == 0");
#endif /* REISERFS_CHECK */
  PATH_LAST_POSITION (path) --;

#ifdef REISERFS_CHECK
  {
    struct item_head * ih = B_N_PITEM_HEAD (PATH_PLAST_BUFFER (path), PATH_LAST_POSITION (path));

    if (!I_IS_DIRECTORY_ITEM (ih) || COMP_SHORT_KEYS (&(ih->ih_key), key)) {
      print_block (PATH_PLAST_BUFFER (path), 0, -1, -1);
      reiserfs_panic (sb, "vs-7005: search_by_entry_key: found item %h is not directory item or "
		      "does not belong to the same directory as key %k", ih, key);
    }
  }
#endif /* REISERFS_CHECK */

  /* binary search in directory item by third component of the key */
  return bin_search_in_dir_item (PATH_PITEM_HEAD (path), B_I_DEH (PATH_PLAST_BUFFER (path), PATH_PITEM_HEAD (path)), key, pos_in_item);
}



/* Keyed 32-bit hash function using TEA in a Davis-Meyer function */
static unsigned long get_third_component (const char * name, int len)
{
  unsigned long res;

  if (!len || (len == 1 && name[0] == '.'))
    return DOT_OFFSET;
  if (len == 2 && name[0] == '.' && name[1] == '.')
    return DOT_DOT_OFFSET;

  res = keyed_hash (name, len);
  res = GET_HASH_VALUE(res);
  if (res == 0)
    res = 128;
  return res + MAX_GEN_NUMBER;
}


/* fills the structure with various parameters of directory entry,
   including key of the pointed object */
static void get_entry_attributes (struct reiserfs_dir_entry * de, int entry_num)
{
#ifdef REISERFS_CHECK
  if (I_ENTRY_COUNT (de->de_ih) < entry_num)
    reiserfs_panic (0, "yr-7006: get_entry_attributes: no such entry (%d-th) in the item (%d)",
		    entry_num, I_ENTRY_COUNT (de->de_ih));
  if (de->de_deh != B_I_DEH (de->de_bh, de->de_ih) + entry_num)
    reiserfs_panic (0, "yr-7008: get_entry_attributes: dir entry header not found");
    
#endif /* REISERFS_CHECK */

  /* few fields are set already (de_bh, de_item_num, de_deh) */
  de->de_entrylen = I_DEH_N_ENTRY_LENGTH (de->de_ih, de->de_deh, entry_num);
  de->de_namelen = de->de_entrylen - (de_with_sd (de->de_deh) ? SD_SIZE : 0);

  de->de_name = B_I_PITEM (de->de_bh, de->de_ih) + de->de_deh->deh_location;

#ifdef REISERFS_ALIGNED
  if ( de->de_name[ de->de_namelen-1 ] == '\0' )
      de->de_namelen = strlen(de->de_name);
#endif

  /* key of object pointed by entry */
  de->de_dir_id = de->de_deh->deh_dir_id;
  de->de_objectid = de->de_deh->deh_objectid;

  /* key of the entry */
  memcpy (&(de->de_entry_key.k_dir_id), &(de->de_ih->ih_key), SHORT_KEY_SIZE);
  de->de_entry_key.k_offset = de->de_deh->deh_offset;
  de->de_entry_key.k_uniqueness = DIRENTRY_UNIQUENESS;

}


static int try_name (struct reiserfs_dir_entry * de, 
		     const char * name,
		     int          namelen)
{
  int retval = POSITION_NOT_FOUND;

  if ((namelen == de->de_namelen) &&
      !memcmp(de->de_name, name, de->de_namelen))
    retval = de_visible (de->de_deh) ? POSITION_FOUND : POSITION_FOUND_INVISIBLE;

  return retval;
}


/* after this function de_entry_num is set correctly only if name
   found or there was no entries with given hash value */
static int linear_search_in_dir_item (struct key * key, struct reiserfs_dir_entry * de, const char * name, int namelen)
{
  int retval;
  int i;

  i = de->de_entry_num;

  if (i == I_ENTRY_COUNT (de->de_ih) ||
      GET_HASH_VALUE (de->de_deh[i].deh_offset) != GET_HASH_VALUE (key->k_offset)) {
    i --;
  }

#ifdef REISERFS_CHECK
  if (de->de_deh != B_I_DEH (de->de_bh, de->de_ih))
    reiserfs_panic (0, "vs-7010: linear_search_in_dir_item: array of entry headers not found");
#endif /* REISERFS_CHECK */

  de->de_deh += i;

  for (; i >= 0; i --, de->de_deh --) {
    if (GET_HASH_VALUE (de->de_deh->deh_offset) != GET_HASH_VALUE (key->k_offset)) {
      return POSITION_NOT_FOUND;
    }
   
    /* mark, that this generation number is used */
    if (de->de_gen_number_bit_string)
      set_bit (GET_GENERATION_NUMBER (de->de_deh->deh_offset), de->de_gen_number_bit_string);
    
    /* de_bh, de_item_num, de_ih, de_deh are already set. Set others fields */
    get_entry_attributes (de, i);
    if ((retval = try_name (de, name, namelen)) != POSITION_NOT_FOUND) {
      de->de_entry_num = i;
      return retval;
    }
  }

  if (GET_GENERATION_NUMBER (de->de_ih->ih_key.k_offset) == 0)
    return POSITION_NOT_FOUND;

#ifdef REISERFS_CHECK
  if (de->de_ih->ih_key.k_offset <= DOT_DOT_OFFSET || de->de_item_num != 0)
    reiserfs_panic (0, "vs-7015: linear_search_in_dir_item: item must be 0-th item in block (%d)", de->de_item_num);
#endif /* REISERFS_CHECK */

  return GOTO_PREVIOUS_ITEM;
}


static int reiserfs_find_entry (struct inode * dir, const char * name, int namelen, struct path * path_to_entry, struct reiserfs_dir_entry * de)
{
  struct key key_to_search;
  int repeat;
  int retval;

  if (!dir || !dir->i_sb)
    return POSITION_NOT_FOUND;

  if ((unsigned int)namelen > REISERFS_MAX_NAME_LEN (dir->i_sb->s_blocksize))
    return POSITION_NOT_FOUND;

  /* there are no entries having the same third component of key, so
     fourth key component is not used */
  copy_key (&key_to_search, INODE_PKEY (dir));
  key_to_search.k_offset = get_third_component (name, namelen);
  key_to_search.k_uniqueness = DIRENTRY_UNIQUENESS;

  while (1) {
    /* search for a directory item using the formed key */
    if (search_by_key (dir->i_sb, &key_to_search, path_to_entry, &repeat, DISK_LEAF_NODE_LEVEL, READ_BLOCKS) == ITEM_NOT_FOUND) {
      /* take previous item */
#ifdef REISERFS_CHECK
      if (!PATH_LAST_POSITION (path_to_entry))
	reiserfs_panic (dir->i_sb, "vs-7010: reiserfs_find_entry: search_by_key returned bad position == 0");
#endif /* REISERFS_CHECK */
      PATH_LAST_POSITION (path_to_entry) --;
    }
    
    de->de_bh = PATH_PLAST_BUFFER (path_to_entry);
    de->de_item_num = PATH_LAST_POSITION (path_to_entry);
    de->de_ih = B_N_PITEM_HEAD (de->de_bh, de->de_item_num);
    de->de_deh = B_I_DEH (de->de_bh, de->de_ih);

#ifdef REISERFS_CHECK
    if (!I_IS_DIRECTORY_ITEM (de->de_ih) || COMP_SHORT_KEYS (&(de->de_ih->ih_key), INODE_PKEY (dir)))
      reiserfs_panic (dir->i_sb, "vs-7020: reiserfs_find_entry: item must be an item of the same directory item as inode");
#endif /* REISERFS_CHECK */

    /* we do not check whether bin_search_in_dir_item found the given key, even if so, we still have
       to compare names */
    bin_search_in_dir_item (de->de_ih, de->de_deh, &key_to_search, &(de->de_entry_num));

    /* compare names for all entries having given hash value */
    retval = linear_search_in_dir_item (&key_to_search, de, name, namelen);
    if (retval != GOTO_PREVIOUS_ITEM)
      /* there is no need to scan directory anymore. Given entry found or does not exist */
      return retval;

    /* there is left neighboring item of this directory and given entry can be there */
    key_to_search.k_offset = de->de_ih->ih_key.k_offset - 1;
    pathrelse (path_to_entry);

  } /* while (1) */
}


/* add entry to the directory (entry can be hidden). Does not mark dir
   inode dirty, do it after successesfull call to it */
static int reiserfs_add_entry (struct reiserfs_transaction_handle *th, struct inode * dir, 
                               const char * name, int namelen, struct key * object_key, struct reiserfs_dir_entry * de,
			       int visible
			       )
{
  struct key entry_key;
  char * buffer;
  int buflen;
  struct reiserfs_de_head * deh;
  struct path path;
  char bit_string [MAX_GEN_NUMBER / 8 + 1];
  int gen_number;
  int repeat;
#ifdef REISERFS_ALIGNED
  int aligned_namelen = (namelen+3) & ~3;
#endif

  init_path (&path);

  if (!dir || !dir->i_sb)
    return -ENOENT;

  if ((unsigned int)namelen > REISERFS_MAX_NAME_LEN (dir->i_sb->s_blocksize))
    return -ENAMETOOLONG;

  /* each entry has unique key. compose it */
  copy_key (&entry_key, INODE_PKEY(dir));
  entry_key.k_offset = get_third_component (name, namelen);
  entry_key.k_uniqueness = DIRENTRY_UNIQUENESS;

  /* get memory for composing the entry */
#ifdef REISERFS_ALIGNED
  buflen = DEH_SIZE + aligned_namelen;
#else
  buflen = DEH_SIZE + namelen;
#endif
  buffer = reiserfs_kmalloc (buflen, GFP_KERNEL, dir->i_sb);
  if (buffer == 0)
    return -ENOMEM;

  /* fill buffer : directory entry head, name[, dir objectid | , stat data | ,stat data, dir objectid ] */
  deh = (struct reiserfs_de_head *)buffer;
  deh->deh_location = 0;
  deh->deh_offset = entry_key.k_offset;
  deh->deh_state = 0;
  /* put key (ino analog) to de */
  deh->deh_dir_id = object_key->k_dir_id;
  deh->deh_objectid = object_key->k_objectid;

  /* copy name */
#ifdef REISERFS_ALIGNED
  memset( (char*)(deh+1), '\0', aligned_namelen );
#endif
  memcpy ((char *)(deh + 1), name, namelen);

  /* entry is ready to be pasted into tree, set 'visibility' and 'stat data in entry' attributes */
  mark_de_without_sd (deh);
  visible ? mark_de_visible (deh) : mark_de_hidden (deh);

  /* find the proper place for the new entry */
  memset (bit_string, 0, sizeof (bit_string));
  de->de_gen_number_bit_string = bit_string;
  if (reiserfs_find_entry (dir, name, namelen, &path, de) == POSITION_FOUND) {
    reiserfs_panic (dir->i_sb, "vs-7030: reiserfs_add_entry: entry with this key %k already exists",
		    &entry_key);
  }

  if (find_first_nonzero_bit (bit_string, MAX_GEN_NUMBER + 1) < MAX_GEN_NUMBER + 1) {
    /* there are few names with given hash value */
    gen_number = find_first_zero_bit (bit_string, MAX_GEN_NUMBER + 1);
    if (gen_number > MAX_GEN_NUMBER) {
      /* there is no free generation number */
      reiserfs_kfree (buffer, buflen, dir->i_sb);
      pathrelse (&path);
      return -EHASHCOLLISION;
    }
    /* adjust offset of directory enrty */
    deh->deh_offset = SET_GENERATION_NUMBER (deh->deh_offset, gen_number);
    entry_key.k_offset = deh->deh_offset;

    /* find place for new entry */
    if (search_by_entry_key (dir->i_sb, &entry_key, &path, &(de->de_entry_num), &repeat)) {
      reiserfs_panic (dir->i_sb, "reiserfs_add_entry: 7032: entry with this key (%k) already exists", &entry_key);
    }
  } else {
    deh->deh_offset = SET_GENERATION_NUMBER (deh->deh_offset, 0);
    entry_key.k_offset = deh->deh_offset;    
  }
  
  /* perform the insertion of the entry that we have prepared */
  if (reiserfs_paste_into_item (th, dir->i_sb, &path, &(de->de_entry_num), &entry_key, buffer, 
                                buflen, REISERFS_KERNEL_MEM, 0) == -1) {
    reiserfs_kfree (buffer, buflen, dir->i_sb);
    return -ENOSPC;
  }

  reiserfs_kfree (buffer, buflen, dir->i_sb);
  dir->i_size += buflen;
  dir->i_mtime = dir->i_ctime = CURRENT_TIME;

  return 0;
}


struct dentry * reiserfs_lookup (struct inode * dir, struct dentry * dentry)
{
  struct inode * inode = 0;
  struct reiserfs_dir_entry de;
  struct path path_to_entry;
  int error;

  init_path (&path_to_entry);

  de.de_gen_number_bit_string = 0;
  error = reiserfs_find_entry (dir, dentry->d_name.name, dentry->d_name.len, &path_to_entry, &de);
  pathrelse (&path_to_entry);
  if (error == POSITION_FOUND) {
    inode = reiserfs_iget (dir->i_sb, (struct key *)&(de.de_dir_id));
    if (!inode)
      return ERR_PTR(-EACCES);
  }

  d_add(dentry, inode);
  return NULL;
}


int reiserfs_create (struct inode * dir, struct dentry *dentry, int mode)
{
  int error;
  struct inode * inode;
  struct reiserfs_dir_entry de;
  int windex ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 2 ;
  struct reiserfs_transaction_handle th ;
  int err;
	
  if (!dir)
    return -ENOENT;
	
  inode = get_empty_inode() ;
  if (!inode) {
    return -ENOSPC ;
  }
  journal_begin(&th, dir->i_sb, jbegin_count) ;
  th.t_caller = "create" ;
  windex = push_journal_writer("reiserfs_create") ;
  inode = reiserfs_new_inode (&th, dir, mode, 0, dentry, inode, &err);
  if (!inode) {
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    return err;
  }
  reiserfs_update_inode_transaction(inode) ;
  reiserfs_update_inode_transaction(dir) ;
	
  inode->i_op = &reiserfs_file_inode_operations;
  inode->i_mode = mode;

  error = reiserfs_add_entry (&th, dir, dentry->d_name.name, dentry->d_name.len, INODE_PKEY (inode), &de, 1);
  if (error) {
    inode->i_nlink--;
    if_in_ram_update_sd (&th, inode);
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    iput (inode);
    return error;
  }
  if_in_ram_update_sd (&th, dir); 
  d_instantiate(dentry, inode);
  pop_journal_writer(windex) ;
  journal_end(&th, dir->i_sb, jbegin_count) ;
  return 0;
}


int reiserfs_mknod (struct inode * dir, struct dentry *dentry, int mode, int rdev)
{
  int error;
  struct inode * inode;
  struct path path_to_entry;
  struct reiserfs_dir_entry de;
  int windex ;
  struct reiserfs_transaction_handle th ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3; 
  int err;

  if (!dir)
    return -ENOENT;

  init_path (&path_to_entry);

  inode = get_empty_inode() ;
  if (!inode) {
    return -ENOSPC ;
  }
  journal_begin(&th, dir->i_sb, jbegin_count) ;
  windex = push_journal_writer("reiserfs_mknod") ;
  de.de_gen_number_bit_string = 0;
  if (reiserfs_find_entry (dir, dentry->d_name.name, dentry->d_name.len, &path_to_entry, &de) == POSITION_FOUND) {
    pathrelse ( &path_to_entry);
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    iput(inode) ;
    return -EEXIST;
  }

  pathrelse ( &path_to_entry);

  inode = reiserfs_new_inode (&th, dir, mode, 0, dentry, inode, &err);
  if (!inode) {
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    return err;
  }
  reiserfs_update_inode_transaction(inode) ;
  reiserfs_update_inode_transaction(dir) ;

  inode->i_uid = current->fsuid;
  inode->i_mode = mode;
  inode->i_op = NULL;

  if (S_ISREG(inode->i_mode))
    inode->i_op = &reiserfs_file_inode_operations;
  else if (S_ISDIR(inode->i_mode)) {
    inode->i_op = &reiserfs_dir_inode_operations;
    if (dir->i_mode & S_ISGID)
      inode->i_mode |= S_ISGID;
  }
  else if (S_ISLNK(inode->i_mode))
    inode->i_op = &reiserfs_symlink_inode_operations;
  else if (S_ISCHR(inode->i_mode))
    inode->i_op = &chrdev_inode_operations;
  else if (S_ISBLK(inode->i_mode))
    inode->i_op = &blkdev_inode_operations;
  else if (S_ISFIFO(inode->i_mode))
    init_fifo(inode);
  if (S_ISBLK(mode) || S_ISCHR(mode))
    inode->i_rdev = to_kdev_t(rdev);

  if_in_ram_update_sd (&th, inode);

  error = reiserfs_add_entry (&th, dir, dentry->d_name.name, dentry->d_name.len, INODE_PKEY (inode), &de, 1);
  if (error) {
    inode->i_nlink--;
    if_in_ram_update_sd (&th, inode);
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    iput (inode);
    return error;
  }
  if_in_ram_update_sd (&th, dir);
  d_instantiate(dentry, inode);
  pop_journal_writer(windex) ;
  journal_end(&th, dir->i_sb, jbegin_count) ;
  return 0;
}


int reiserfs_mkdir (struct inode * dir, struct dentry *dentry, int mode)
{
  int error;
  struct inode * inode;
  struct path path_to_entry;
  struct reiserfs_dir_entry de;
  int windex ;
  struct reiserfs_transaction_handle th ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3; 
  int err;


  init_path (&path_to_entry);
  if (!dir || !dir->i_sb) {
    return -EINVAL;
  }
  inode = get_empty_inode() ;
  if (!inode) {
    return -ENOSPC ;
  }

  journal_begin(&th, dir->i_sb, jbegin_count) ;
  windex = push_journal_writer("reiserfs_mkdir") ;
  de.de_gen_number_bit_string = 0;
  if (reiserfs_find_entry (dir, dentry->d_name.name, dentry->d_name.len, &path_to_entry, &de) == POSITION_FOUND) {
    pathrelse (&path_to_entry);
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    iput(inode) ;
    return -EEXIST;
  }
  pathrelse (&path_to_entry);
  
  if (dir->i_nlink >= REISERFS_LINK_MAX) {
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    iput(inode) ;
    return -EMLINK;
  }
  
  mode = S_IFDIR | (mode & 0777 & ~current->fs->umask);
  if (dir->i_mode & S_ISGID)
    mode |= S_ISGID;
  inode = reiserfs_new_inode (&th, dir, mode, 0, dentry, inode, &err);
  if (!inode) {
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    return err;
  }
  reiserfs_update_inode_transaction(inode) ;
  reiserfs_update_inode_transaction(dir) ;

  inode->i_op = &reiserfs_dir_inode_operations;

  /* new inode and stat data are uptodate. Inode is clean. */
  error = reiserfs_add_entry (&th, dir, dentry->d_name.name, dentry->d_name.len, INODE_PKEY (inode), &de, 1);
  if (error) {
    inode->i_nlink = 0;
    if_in_ram_update_sd (&th, inode);
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    iput (inode);
    return error;
  }

  /* update dir inode, reiserfs_add_entry does not do that */
  dir->i_nlink ++;
  if_in_ram_update_sd (&th, dir);
  d_instantiate(dentry, inode);
  pop_journal_writer(windex) ;
  journal_end(&th, dir->i_sb, jbegin_count) ;
  return 0;
}


static int reiserfs_empty_dir(struct inode * inode)
{
  return inode->i_size == EMPTY_DIR_SIZE;
}


int rmdir_not_allowed (struct inode * dir, struct reiserfs_dir_entry * de, struct inode * inode)
{
  if ((dir->i_mode & S_ISVTX) && !fsuser() &&
      current->fsuid != inode->i_uid &&
      current->fsuid != dir->i_uid)
    return -EPERM;

  if (inode->i_dev != dir->i_dev)
    return -EPERM;

  if (inode == dir)	/* we may not delete ".", but "../dir" is ok */
    return -EPERM;

  if (!S_ISDIR(inode->i_mode))
    return -ENOTDIR;

  if (!reiserfs_empty_dir(inode))
    return -ENOTEMPTY;

  if (de->de_objectid != inode->i_ino)
    return -ENOENT;

  return 0;
}


int reiserfs_rmdir (struct inode * dir, struct dentry *dentry)
{
  struct inode * inode;
  int retval;
  struct reiserfs_dir_entry de;
  struct path path;
  struct super_block *s ;
  int windex ;
  struct reiserfs_transaction_handle th ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3; 

  init_path (&path);

  retval = -ENOENT;
  de.de_gen_number_bit_string = 0;
  journal_begin(&th, dir->i_sb, jbegin_count) ;
  windex = push_journal_writer("reiesrfs_rmdir") ;
  if (reiserfs_find_entry (dir, dentry->d_name.name, dentry->d_name.len, &path, &de) == POSITION_NOT_FOUND)
    goto end_rmdir;

  inode = dentry->d_inode;

  /* we don't need call this so early here, I'm just being cautious */
  reiserfs_update_inode_transaction(inode) ;
  reiserfs_update_inode_transaction(dir) ;

  retval = rmdir_not_allowed (dir, &de, inode);
  if (retval)
    goto end_rmdir;

  /* free preserve list if we should */
/*  maybe_free_preserve_list (dir->i_sb);*/

  if (!reiserfs_empty_dir (inode))
    retval = -ENOTEMPTY;
  else {
    /* cut entry from dir directory */
    if (reiserfs_cut_from_item (&th, dir, dir->i_sb, &path, &(de.de_entry_num), &(de.de_entry_key), 0, NOTHING_SPECIAL) == 0) {
      retval = -ENOENT;
    }
  }
  if (retval)
    goto end_rmdir;

  if (inode->i_nlink != 2)
    printk ("reiserfs_rmdir: empty directory has nlink != 2 (%d)\n", inode->i_nlink);
  inode->i_nlink = 0;
  inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
  if_in_ram_update_sd (&th, inode);
  dir->i_nlink --;
  dir->i_size -= (DEH_SIZE + de.de_entrylen);
  if_in_ram_update_sd (&th, dir);

  s = dir->i_sb ;
  pop_journal_writer(windex) ;
  journal_end(&th, s, jbegin_count) ;
  d_delete(dentry); /* note, we've moved this after the journal end */
  return 0;
	
end_rmdir:
  /* we must release path, because we did not call reiserfs_cut_from_item, or reiserfs_cut_from_item
     does not release path if operation was not complete */
  pathrelse (&path);
  pop_journal_writer(windex) ;
  journal_end(&th, dir->i_sb, jbegin_count) ;
  return retval;	
}


int reiserfs_unlink (struct inode * dir, struct dentry *dentry)
{
  int retval;
  struct inode * inode;
  struct reiserfs_dir_entry de;
  struct path path;
  int windex ;
  int call_journal_end = 1 ;
  struct reiserfs_transaction_handle th ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3; 

  init_path (&path);

  retval = -ENOENT;
	
  journal_begin(&th, dir->i_sb, jbegin_count) ;
  windex = push_journal_writer("reiserfs_unlink") ;
  /* free preserve list if we should */
/*  maybe_free_preserve_list (dir->i_sb);*/
	
  de.de_gen_number_bit_string = 0;
  if (reiserfs_find_entry (dir, dentry->d_name.name, dentry->d_name.len, &path, &de) == POSITION_NOT_FOUND) {
    goto end_unlink;
  }

  inode = dentry->d_inode;

  reiserfs_update_inode_transaction(inode) ;
  reiserfs_update_inode_transaction(dir) ;

  retval = -EPERM;
  if (S_ISDIR (inode->i_mode)) {
    goto end_unlink;
  }
  if ((dir->i_mode & S_ISVTX) && !fsuser() &&
      current->fsuid != inode->i_uid &&
      current->fsuid != dir->i_uid) {
    goto end_unlink;
  }

  retval = -ENOENT;
  if (comp_short_keys ((struct key *)&(de.de_dir_id), INODE_PKEY (inode))) {
    goto end_unlink;
  }
  
  if (!inode->i_nlink) {
    printk("reiserfs_unlink: deleting nonexistent file (%s:%lu), %d\n",
	   kdevname(inode->i_dev), inode->i_ino, inode->i_nlink);
    inode->i_nlink = 1;
  }
  if (reiserfs_cut_from_item (&th, dir, dir->i_sb, &path, &(de.de_entry_num), &(de.de_entry_key), 0, NOTHING_SPECIAL) == 0) {
    retval = -ENOENT;
    goto end_unlink;
  }

  inode->i_nlink--;
  inode->i_ctime = CURRENT_TIME;
  if_in_ram_update_sd (&th, inode);

  dir->i_size -= (de.de_entrylen + DEH_SIZE);
  dir->i_ctime = dir->i_mtime = CURRENT_TIME;
  if_in_ram_update_sd (&th, dir) ;
  pop_journal_writer(windex) ;
  journal_end(&th, dir->i_sb, jbegin_count) ;
  call_journal_end = 0 ;
  d_delete(dentry); 
  retval = 0;

end_unlink:
  pathrelse (&path);
  pop_journal_writer(windex) ;
  if (call_journal_end) 
    journal_end(&th, dir->i_sb, jbegin_count) ;
  return retval;
}


int reiserfs_symlink (struct inode * dir, struct dentry * dentry, const char * symname)
{
  struct inode * inode;
  struct path path_to_entry;
  struct reiserfs_dir_entry de;
  int error;
  int windex ;
  struct reiserfs_transaction_handle th ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3; 
  int err;

  init_path (&path_to_entry);
  if (strlen (symname) + 1 + SD_SIZE > MAX_ITEM_LEN (dir->i_sb->s_blocksize)) {
    return -ENAMETOOLONG;
  }
  inode = get_empty_inode() ;
  if (!inode) {
    return -ENOSPC ;
  }
 
  journal_begin(&th, dir->i_sb, jbegin_count) ;
  windex = push_journal_writer("reiserfs_symlink") ;
  inode = reiserfs_new_inode (&th, dir, S_IFLNK, symname, dentry, inode, &err);
  if (inode == 0) {
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    return err;
  }
  reiserfs_update_inode_transaction(inode) ;
  reiserfs_update_inode_transaction(dir) ;

  inode->i_op = &reiserfs_symlink_inode_operations;
  inode->i_size = strlen (symname);
  inode->i_mode = S_IFLNK | 0777;
  if_in_ram_update_sd (&th, inode);

  de.de_gen_number_bit_string = 0;
  if (reiserfs_find_entry (dir, dentry->d_name.name, dentry->d_name.len, &path_to_entry, &de) == POSITION_FOUND) {
    pathrelse (&path_to_entry);
    inode->i_nlink--;
    if_in_ram_update_sd (&th, inode);
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    iput (inode);
    return -EEXIST;
  }
  pathrelse (&path_to_entry);

  error = reiserfs_add_entry (&th, dir, dentry->d_name.name, dentry->d_name.len, INODE_PKEY (inode), &de, 1);
  if (error) {
    inode->i_nlink--;
    if_in_ram_update_sd (&th, inode);
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    iput (inode);
    return error;
  }
  if_in_ram_update_sd (&th, dir);
  d_instantiate(dentry, inode);
  pop_journal_writer(windex) ;
  journal_end(&th, dir->i_sb, jbegin_count) ;
  return 0;
}


int reiserfs_link (struct dentry * old_dentry, struct inode * dir, struct dentry * dentry)
{
  struct inode *inode = old_dentry->d_inode;
  struct path path_to_entry;
  struct reiserfs_dir_entry de;
  int error;
  int windex ;
  struct reiserfs_transaction_handle th ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3; 
  
  init_path (&path_to_entry);

  /* object must not be directory */
  if (S_ISDIR(inode->i_mode)) {
    return -EPERM;
  }
  
  /* file has too many links */
  if (inode->i_nlink >= REISERFS_LINK_MAX) {
    return -EMLINK;
  }

  journal_begin(&th, dir->i_sb, jbegin_count) ;
  windex = push_journal_writer("reiserfs_link") ;

  reiserfs_update_inode_transaction(inode) ;
  reiserfs_update_inode_transaction(dir) ;

  de.de_gen_number_bit_string = 0;
  if (reiserfs_find_entry (dir, dentry->d_name.name, dentry->d_name.len, &path_to_entry, &de) == POSITION_FOUND) {
    pathrelse (&path_to_entry);
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    return -EEXIST;
  }
  
  pathrelse (&path_to_entry);

  /* free preserve list if we should */
/*  maybe_free_preserve_list (dir->i_sb);*/

  /* create new entry */
  error = reiserfs_add_entry (&th, dir, dentry->d_name.name, dentry->d_name.len, INODE_PKEY (inode), &de, 1);
  if (error) {
    pop_journal_writer(windex) ;
    journal_end(&th, dir->i_sb, jbegin_count) ;
    return error;
  }
  inode->i_nlink++;
  inode->i_ctime = CURRENT_TIME;
  if_in_ram_update_sd (&th, inode);
  if_in_ram_update_sd (&th, dir);
  inode->i_count++;
  d_instantiate(dentry, inode);
  pop_journal_writer(windex) ;
  journal_end(&th, dir->i_sb, jbegin_count) ;
  return 0;
}


static int de_still_valid (const char * name, int len, struct reiserfs_dir_entry * de)
{
  struct item_head * ih;
  struct reiserfs_de_head * deh;

  if (!de || !de->de_bh)
    return 0;

  deh = B_I_DEH (de->de_bh, ih = B_N_PITEM_HEAD (de->de_bh, de->de_item_num));
  /* compare dir entry headers, record loacation and names */
  if (memcmp (&(deh[de->de_entry_num]), de->de_deh, DEH_SIZE) ||
      B_I_E_NAME (de->de_entry_num, de->de_bh, ih) != de->de_name ||
      memcmp (name, de->de_name, len))
    return 0;
  return 1;
}


static int entry_points_to_object (const char * name, int len, struct reiserfs_dir_entry * de, struct inode * inode)
{
  if (!de_still_valid (name, len, de))
    return 0;

  if (inode) {
    if (!de_visible (de->de_deh))
      reiserfs_panic (0, "vs-7042: entry_points_to_object: entry must be visible");
    return (de->de_objectid == inode->i_ino) ? 1 : 0;
  }

  /* this must be added hidden entry */
  if (de_visible (de->de_deh))
    reiserfs_panic (0, "vs-7043: entry_points_to_object: entry must be visible");

  return 1;
}


/* sets key of parent directory in ".." entry */
static void set_ino_in_dir_entry (struct reiserfs_dir_entry * de, struct key * key)
{
  de->de_deh->deh_dir_id = key->k_dir_id;;
  de->de_deh->deh_objectid = key->k_objectid;
  /*
  *((unsigned long *)(de->de_name + 2)) = key->k_dir_id;
  mark_de_with_directory_id (de->de_deh);*/
}

/* 
 * process, that is going to call fix_nodes/do_balance must hold only
 * one path. If it holds 2 or more, it can get into endless waiting in
 * get_empty_nodes or its clones 
 */
static int do_reiserfs_rename (struct reiserfs_transaction_handle *th, struct inode * old_dir, struct dentry *old_dentry,
			       struct inode * new_dir, struct dentry *new_dentry)
{
  int retval;
  struct path old_entry_path, new_entry_path, dot_dot_entry_path;
  struct reiserfs_dir_entry old_de, new_de, dot_dot_de;
  struct inode * old_inode, * new_inode;
  int new_entry_added = 0;

  init_path (&old_entry_path);
  init_path (&new_entry_path);
  init_path (&dot_dot_entry_path);
  goto start_up;

try_again:
  current->policy |= SCHED_YIELD;
  schedule();
	
start_up:
  old_inode = new_inode = NULL;
  dot_dot_de.de_bh = 0;
  new_de.de_bh = 0;

  /* 
   * look for the old name in old directory 
   */
  retval = -ENOENT;
  old_de.de_gen_number_bit_string = 0;
  if (reiserfs_find_entry (old_dir, old_dentry->d_name.name, old_dentry->d_name.len, &old_entry_path, &old_de) == POSITION_NOT_FOUND)
    goto end_rename;

  pathrelse (&old_entry_path);

  old_inode = old_dentry->d_inode;
  retval = -EPERM;

  if ((old_dir->i_mode & S_ISVTX) && 
      current->fsuid != old_inode->i_uid &&
      current->fsuid != old_dir->i_uid && !fsuser())
    goto end_rename;

  new_inode = new_dentry->d_inode;

  /* look for the new entry in target directory */
  new_de.de_gen_number_bit_string = 0;
  if (reiserfs_find_entry (new_dir, new_dentry->d_name.name, new_dentry->d_name.len, &new_entry_path, &new_de) == POSITION_FOUND) {
    if (!new_inode) {
      printk ("do_reiserfs_rename: new entry found, inode == 0 though\n");
    }
    /* this entry already exists, we can just set key of object */
    new_entry_added = 1;
  } else {
#ifdef REISERFS_CHECK
    if (new_entry_added) {
      if (new_de.de_namelen != new_dentry->d_name.len || memcmp (new_de.de_name, new_dentry->d_name.name, new_de.de_namelen) ||
	  de_visible (new_de.de_deh))
	reiserfs_panic (old_dir->i_sb, "vs-7045: reiserfs_rename: suspicious entry found");
    }
#endif /* REISERFS_CHECK */
  }
  pathrelse (&new_entry_path);


  if (new_inode == old_inode) {
    retval = 0;
    goto end_rename;
  }

  if (new_inode && S_ISDIR(new_inode->i_mode)) {
    /* new name exists and points to directory */
    retval = -EISDIR;
    if (!S_ISDIR(old_inode->i_mode))
      goto end_rename;
    retval = -EINVAL;
    if (is_subdir (new_dentry, old_dentry))
      goto end_rename;
    retval = -ENOTEMPTY;
    if (!reiserfs_empty_dir (new_inode))
      goto end_rename;
    retval = -EBUSY;
    if (new_inode->i_count > 1)
      goto end_rename;
  }

  retval = -EPERM;
  if (new_inode && (new_dir->i_mode & S_ISVTX) && 
      current->fsuid != new_inode->i_uid &&
      current->fsuid != new_dir->i_uid && !fsuser())
    goto end_rename;

  if (S_ISDIR(old_inode->i_mode)) {
    /* old name points to directory */
    retval = -ENOTDIR;
    if (new_inode && !S_ISDIR(new_inode->i_mode))
      goto end_rename;

    retval = -EINVAL;
    if (is_subdir(new_dentry, old_dentry))
      goto end_rename;

    retval = -EIO;
    /* directory is renamed, its parent directory will be changed, so find ".." entry */
    dot_dot_de.de_gen_number_bit_string = 0;
    if (reiserfs_find_entry (old_inode, "..", 2, &dot_dot_entry_path, &dot_dot_de) == POSITION_NOT_FOUND)
      goto end_rename;
    if (dot_dot_de.de_objectid != old_dir->i_ino)
      goto end_rename;
    pathrelse (&dot_dot_entry_path);

    retval = -EMLINK;
    if (!new_inode && new_dir->i_nlink >= REISERFS_LINK_MAX)
      goto end_rename;
  }
  
  if (new_entry_added == 0) {
    /* add new entry if we did not do it, but do not mark it as visible */
    retval = reiserfs_add_entry (th, new_dir, new_dentry->d_name.name, new_dentry->d_name.len, INODE_PKEY (old_inode), &new_de, 0);
    if (retval)
      goto end_rename;
    if_in_ram_update_sd (th, new_dir);
    new_entry_added = 1;
    goto try_again;
  }


  /* 
   * look for old name, new name and ".." when renaming directories again
   */
  if (reiserfs_find_entry (old_dir, old_dentry->d_name.name, old_dentry->d_name.len, &old_entry_path, &old_de) == POSITION_NOT_FOUND)
    reiserfs_panic (old_dir->i_sb, "vs-7050: reiserfs_rename: old name not found");
  if (reiserfs_find_entry (new_dir, new_dentry->d_name.name, new_dentry->d_name.len, &new_entry_path, &new_de) == POSITION_NOT_FOUND)
    reiserfs_panic (old_dir->i_sb, "vs-7055: reiserfs_rename: new name not found");
  if (S_ISDIR(old_inode->i_mode) && reiserfs_find_entry (old_inode, "..", 2, &dot_dot_entry_path, &dot_dot_de) == POSITION_NOT_FOUND)
    reiserfs_panic (old_dir->i_sb, "vs-7060: reiserfs_rename: \"..\" name not found");
 

  /* sanity checking before doing the rename - avoid races */
  if (!entry_points_to_object (new_dentry->d_name.name, new_dentry->d_name.len, &new_de, new_inode))
    goto try_again;
  if (!entry_points_to_object (old_dentry->d_name.name, old_dentry->d_name.len, &old_de, old_inode))
    /* go to re-looking for old entry */
    goto try_again;

  if (S_ISDIR(old_inode->i_mode) && !entry_points_to_object ("..", 2, &dot_dot_de, old_dir))
    /* go to re-looking for ".." entry of renamed directory */
    goto try_again;
  
  /* ok, all the changes can be done in one fell swoop when we have
     claimed all the buffers needed.*/

  /* make old name hidden */
  mark_de_hidden (old_de.de_deh);
  journal_mark_dirty(th, old_dir->i_sb, old_de.de_bh) ;

  /* make new name visible and set key of old object (if entry
     existed, it is already visible, if not, key is correct already) */
  mark_de_visible (new_de.de_deh);
  new_de.de_deh->deh_dir_id = INODE_PKEY (old_inode)->k_dir_id;
  new_de.de_deh->deh_objectid = INODE_PKEY (old_inode)->k_objectid;
  journal_mark_dirty(th, old_dir->i_sb, new_de.de_bh) ;

  old_dir->i_ctime = old_dir->i_mtime = CURRENT_TIME;
  if_in_ram_update_sd (th, old_dir);

  new_dir->i_ctime = new_dir->i_mtime = CURRENT_TIME;
  if_in_ram_update_sd (th, new_dir);

  if (new_inode) {
    new_inode->i_nlink--;
    new_inode->i_ctime = CURRENT_TIME;
    if_in_ram_update_sd (th, new_inode);
  }
  if (dot_dot_de.de_bh) {
    set_ino_in_dir_entry (&dot_dot_de, INODE_PKEY (new_dir));
    journal_mark_dirty(th, old_dir->i_sb, dot_dot_de.de_bh) ;
    old_dir->i_nlink--;
    if_in_ram_update_sd (th, old_dir);
    if (new_inode) {
      new_inode->i_nlink--;
      if_in_ram_update_sd (th, new_inode);
    } else {
      new_dir->i_nlink++;
      if_in_ram_update_sd (th, new_dir);
    }
  }

  /* ok, renaming done */
  decrement_counters_in_path (&new_entry_path);
  decrement_counters_in_path (&dot_dot_entry_path);

  /* remove old name (it is hidden now) */
  if (reiserfs_cut_from_item (th, old_dir, old_dir->i_sb, &old_entry_path, &(old_de.de_entry_num),
			      &(old_de.de_entry_key), 0, PRESERVE_RENAMING) == 0)
    printk ("reiserfs_rename: could not remove old name\n");
  else {
    old_dir->i_size -= DEH_SIZE + old_de.de_entrylen;
    old_dir->i_blocks = old_dir->i_size / 512 + ((old_dir->i_size % 512) ? 1 : 0);
    if_in_ram_update_sd (th, old_dir);
  }

  /* Update the dcache */
  d_move(old_dentry, new_dentry);
  retval = 0;

end_rename:
  pathrelse (&old_entry_path);
  return retval;
}


int	reiserfs_rename (
			 struct inode * old_dir, struct dentry *old_dentry,
			 struct inode * new_dir, struct dentry *new_dentry
			 )
{
  static struct wait_queue * wait = NULL;
  static int lock = 0;
  int result;
  int windex ;
  struct reiserfs_transaction_handle th ;
  int jbegin_count = JOURNAL_PER_BALANCE_CNT * 3; 
  
  while (lock)
    sleep_on(&wait);
  lock = 1;
  journal_begin(&th, old_dir->i_sb, jbegin_count) ;
  windex = push_journal_writer("reiesrfs_rename") ;
  /* we are trusting if_in_ram_update_sd to update the transaction 
  ** info in each inode as they get chagned
  */
  result = do_reiserfs_rename (&th, old_dir, old_dentry, new_dir, new_dentry);
  pop_journal_writer(windex) ;
  journal_end(&th, old_dir->i_sb, jbegin_count) ;
  lock = 0;
  wake_up(&wait);
  return result;
}






























