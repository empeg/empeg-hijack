/*
 * Copyright 1996, 1997, 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */
#ifdef __KERNEL__

#include <stdarg.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/reiserfs_fs.h>
#include <linux/string.h>

#else

#include "nokernel.h"
#include <stdarg.h>
#include <limits.h>

#endif


static char error_buf[1024];
static char fmt_buf[1024];
static char off_buf[80];

static char * offset (struct key * key)
{
  if (KEY_IS_DIRECTORY_KEY (key))
    sprintf (off_buf, "%d(%d)", GET_HASH_VALUE (key->k_offset), GET_GENERATION_NUMBER (key->k_offset));
  else
    sprintf (off_buf, "%d", key->k_offset);
  return off_buf;
}


static void sprintf_key (char * buf, struct key * key)
{
  if (key)
    sprintf (buf, "[%d %d %s %d]", key->k_dir_id, key->k_objectid, offset (key), key->k_uniqueness);
  else
    sprintf (buf, "[NULL]");
}


static void sprintf_item_head (char * buf, struct item_head * ih)
{
  if (ih) {
    sprintf_key (buf, &(ih->ih_key));
    sprintf (buf + strlen (buf), ", item_len %d, item_location %d", ih->ih_item_len, ih->ih_item_location);
  } else
    sprintf (buf, "[NULL]");
}


static void sprintf_direntry (char * buf, struct reiserfs_dir_entry * de)
{
  char name[20];

  memcpy (name, de->de_name, de->de_namelen > 19 ? 19 : de->de_namelen);
  name [de->de_namelen > 19 ? 19 : de->de_namelen] = 0;
  sprintf (buf, "\"%s\"==>[%d %d]", name, de->de_dir_id, de->de_objectid);
}


static void sprintf_block_head (char * buf, struct buffer_head * bh)
{
  sprintf (buf, "level=%d, nr_items=%d, free_space=%d rdkey ",
	   B_LEVEL (bh), B_NR_ITEMS (bh), B_BLK_HEAD (bh)->blk_free_space);
  if (B_LEVEL (bh) == DISK_LEAF_NODE_LEVEL)
    sprintf_key (buf + strlen (buf), B_PRIGHT_DELIM_KEY (bh));
}


static void sprintf_buffer_head (char * buf, struct buffer_head * bh) 
{
  sprintf (buf, "dev x%x, size %ld, blocknr %ld, count %d, state(%s, %s, %s)",
	   bh->b_dev, bh->b_size, bh->b_blocknr, bh->b_count,
	   buffer_uptodate (bh) ? "UPTODATE" : "!UPTODATE",
	   buffer_dirty (bh) ? "DIRTY" : "CLEAN",
	   buffer_locked (bh) ? "LOCKED" : "UNLOCKED");
}


static void sprintf_disk_child (char * buf, struct disk_child * dc)
{
  sprintf (buf, "[dc_number=%lu, dc_size=%u]", dc->dc_block_number, dc->dc_size);
}


static char * is_there_reiserfs_struct (char * fmt, int * what, int * skip)
{
  char * k = fmt;

  *skip = 0;
  
  while (1) {
    k = strstr (k, "%");
    if (!k)
      break;
    if (k && (k[1] == 'k' || k[1] == 'h' || k[1] == 't' ||
	      k[1] == 'z' || k[1] == 'b' || k[1] == 'y')) {
      *what = k[1];
      break;
    }
    (*skip) ++;
    k ++;
  }
  return k;
}


/* debugging reiserfs we used to print out a lot of different
   variables, like keys, item headers, buffer heads etc. Values of
   most fields matter. So it took a long time just to write
   appropriative printk. With this reiserfs_warning you can use format
   specification for complex structures like you used to do with
   printfs for integers, doubles and pointers. For instance, to print
   out key structure you have to write just: 
   reiserfs_warning ("bad key %k", key); 
   instead of 
   printk ("bad key %lu %lu %lu %lu", key->k_dir_id, key->k_objectid, 
           key->k_offset, key->k_uniqueness); 
*/

#define do_reiserfs_warning \
{\
  char * fmt1 = fmt_buf;\
  va_list args;\
  int i, j;\
  char * k;\
  char * p = error_buf;\
  int what, skip;\
\
  strcpy (fmt1, fmt);\
  va_start(args, fmt);\
\
  while (1) {\
    k = is_there_reiserfs_struct (fmt1, &what, &skip);\
    if (k != 0) {\
      *k = 0;\
      p += vsprintf (p, fmt1, args);\
\
      for (i = 0; i < skip; i ++)\
	j = va_arg (args, int);\
\
      switch (what) {\
      case 'k':\
	sprintf_key (p, va_arg(args, struct key *));\
	break;\
      case 'h':\
	sprintf_item_head (p, va_arg(args, struct item_head *));\
	break;\
      case 't':\
	sprintf_direntry (p, va_arg(args, struct reiserfs_dir_entry *));\
	break;\
      case 'y':\
	sprintf_disk_child (p, va_arg(args, struct disk_child *));\
	break;\
      case 'z':\
	sprintf_block_head (p, va_arg(args, struct buffer_head *));\
	break;\
      case 'b':\
	sprintf_buffer_head (p, va_arg(args, struct buffer_head *));\
	break;\
      }\
      p += strlen (p);\
      fmt1 = k + 2;\
    } else {\
      i = vsprintf (p, fmt1, args);\
      break;\
    }\
  }\
\
  va_end(args);\
}


/* in addition to usual conversion specifiers this accepts reiserfs
   specific conversion specifiers: 
   %k to print key, 
   %h to print item_head,
   %t to print directory entry 
   %z to print block head (arg must be struct buffer_head *
   %b to print buffer_head
*/
void reiserfs_warning (const char * fmt, ...)
{
  do_reiserfs_warning;
  printk ("%s", error_buf);
}



/* The format:

           maintainer-errorid: [function-name:] message

    where errorid is unique to the maintainer and function-name is
    optional, is recommended, so that anyone can easily find the bug
    with a simple grep for the short to type string
    maintainer-errorid.  Don't bother with reusing errorids, there are
    lots of numbers out there.

    Example: 
    
    reiserfs_panic(
	p_sb, "reiser-29: reiserfs_new_blocknrs: "
	"one of search_start or rn(%d) is equal to MAX_B_NUM,"
	"which means that we are optimizing location based on the bogus location of a temp buffer (%p).", 
	rn, bh
    );

    Regular panic()s sometimes clear the screen before the message can
    be read, thus the need for the while loop.  

    Numbering scheme for panic used by Vladimir and Anatoly( Hans completely ignores this scheme, and considers it
    pointless complexity):

    panics in reiserfs_fs.h have numbers from 1000 to 1999
    super.c				        2000 to 2999
    preserve.c				    3000 to 3999
    bitmap.c				    4000 to 4999
    stree.c				        5000 to 5999
    prints.c				    6000 to 6999
    namei.c                     7000 to 7999
    fix_nodes.c                 8000 to 8999
    dir.c                       9000 to 9999
	lbalance.c					10000 to 10999
	ibalance.c		11000 to 11999 not ready
	do_balan.c		12000 to 12999
	inode.c			13000 to 13999
	file.c			14000 to 14999
    objectid.c                       15000 - 15999
    buffer.c                         16000 - 16999
    symlink.c                        17000 - 17999

   .  */

void flush_log_buf (void);
extern int g_balances_number;

#ifdef REISERFS_CHECK
extern struct tree_balance * cur_tb;
extern unsigned long log_size;
#endif

void reiserfs_panic (struct super_block * sb, const char * fmt, ...)
{
  do_reiserfs_warning;
  printk ("%s", error_buf);

#ifdef __KERNEL__

  /* comment before release */
  for (;;);

  if (sb && !(sb->s_flags & MS_RDONLY)) {
    sb->u.reiserfs_sb.s_mount_state |= REISERFS_ERROR_FS;
    sb->u.reiserfs_sb.s_rs->s_state = REISERFS_ERROR_FS;
    
    /* should we journal this??? BUG */
    mark_buffer_dirty(sb->u.reiserfs_sb.s_sbh, 1);
    sb->s_dirt = 1;
  }

  /* this is to prevent panic from syncing this filesystem */
  if (sb && sb->s_lock)
    sb->s_lock=0;
  if (sb)
    sb->s_flags |= MS_RDONLY;

  panic ("REISERFS: panic (device %s): %s\n",
	 sb ? kdevname(sb->s_dev) : "sb == 0", error_buf);
#else
  exit (0);
#endif
}

static char * vi_type (struct virtual_item * vi)
{
  static char *types[]={"directory", "direct", "indirect", "stat data"};

  if (vi->vi_type & VI_TYPE_STAT_DATA)
    return types[3];
  if (vi->vi_type & VI_TYPE_INDIRECT)
    return types[2];
  if (vi->vi_type & VI_TYPE_DIRECT)
    return types[1];
  if (vi->vi_type & VI_TYPE_DIRECTORY)
    return types[0];

  reiserfs_panic (0, "vi_type: 6000: unknown type (0x%x)", vi->vi_type);
  return NULL;
}

void print_virtual_node (struct virtual_node * vn)
{
  int i, j;
  

  printk ("VIRTUAL NODE CONTAINS %d items, has size %d,%s,%s, ITEM_POS=%d POS_IN_ITEM=%d MODE=\'%c\'\n",
	  vn->vn_nr_item, vn->vn_size,
	  (vn->vn_vi[0].vi_type & VI_TYPE_LEFT_MERGEABLE )? "left mergeable" : "", 
	  (vn->vn_vi[vn->vn_nr_item - 1].vi_type & VI_TYPE_RIGHT_MERGEABLE) ? "right mergeable" : "",
	  vn->vn_affected_item_num, vn->vn_pos_in_item, vn->vn_mode);


  for (i = 0; i < vn->vn_nr_item; i ++)
    {
      printk ("%s %d %d", vi_type (&vn->vn_vi[i]), i, vn->vn_vi[i].vi_item_len);
      if (vn->vn_vi[i].vi_entry_sizes)
	{
	  printk ("It is directory with %d entries: ", vn->vn_vi[i].vi_entry_count);
	  for (j = 0; j < vn->vn_vi[i].vi_entry_count; j ++)
	    printk ("%d ", vn->vn_vi[i].vi_entry_sizes[j]);
	}
      printk ("\n");
    }
}


void print_path (struct tree_balance * tb, struct path * path)
{
  int h = 0;
  
  if (tb) {
    while (tb->insert_size[h]) {
      printk ("block %lu (level=%d), position %d\n", PATH_H_PBUFFER (path, h) ? PATH_H_PBUFFER (path, h)->b_blocknr : 0,
	      PATH_H_PBUFFER (path, h) ? B_BLK_HEAD (PATH_H_PBUFFER (path, h))->blk_level : 0,
	      PATH_H_POSITION (path, h));
      h ++;
    }
  } else {
    int offset = path->path_length;
    struct buffer_head * bh;
    printk ("Offset    Bh     (b_blocknr, b_count) Position Nr_item\n");
    while ( offset > ILLEGAL_PATH_ELEMENT_OFFSET ) {
      bh = PATH_OFFSET_PBUFFER (path, offset);
      printk ("%6d %10p (%9lu, %7d) %8d %7d\n", offset, 
	      bh, bh ? bh->b_blocknr : 0, bh ? bh->b_count : 0,
	      PATH_OFFSET_POSITION (path, offset), bh ? B_NR_ITEMS (bh) : -1);

      offset --;
    }
  }

#if 0
  printk ("#####################\n");
  printk ("Offset    Bh     (b_blocknr, b_count) Position Nr_item\n");
  while ( offset > ILLEGAL_PATH_ELEMENT_OFFSET ) {
    bh = PATH_OFFSET_PBUFFER (path, offset);
    printk ("%6d %10p (%9lu, %7d) %8d %7d\n", offset, 
	    bh, bh ? bh->b_blocknr : 0, bh ? bh->b_count : 0,
	    PATH_OFFSET_POSITION (path, offset), bh ? B_NR_ITEMS (bh) : -1);

    print_buffer_head(bh,"print_path");
    offset --;
  }
  printk ("#####################\n");
#endif
}

char * int2nameprefix (char * nameprefix, unsigned int num);
char * k_offset_to_string (unsigned long offset, unsigned long uniqueness)
{
  static char buf[15];

  if (offset == SD_OFFSET)		/* offset == 0 */
    strcpy (buf, "SD");
  else if (offset == DOT_OFFSET) {	/* offset == 1 */
    /* directory and regular files can have k_offset == 1 */
    if (uniqueness < TYPE_INDIRECT)
      strcpy (buf, ".");
    else
      strcpy (buf, "1");
  } else if (offset == DOT_DOT_OFFSET) {/* offset == 2 */
    /* directory and regular files can have k_offset == 2 */
    if (uniqueness < TYPE_INDIRECT)
      strcpy (buf, "..");
    else
      strcpy (buf, "2");
  } else {				/* offset > 2 */
    if (uniqueness < TYPE_INDIRECT)
       int2nameprefix (buf, offset);
    else
      sprintf (buf, "%lu", offset);
  }
  return buf;
}

char * k_uniqueness_to_string (unsigned long uniqueness)
{
  static char * item_type[] = {"SD", "IND", "TAIL", "DIR"};

  if (uniqueness == TYPE_STAT_DATA)
    return item_type[0];
  if (uniqueness == TYPE_INDIRECT)
    return item_type[1];
  if (uniqueness == TYPE_DIRECT)
    return item_type[2];
  return item_type[3];
}



void print_de (struct reiserfs_dir_entry * de)
{
  printk ("entry key: [%d, %d, \"%s\" %s], object_key: [%u %u], b_blocknr=%lu, item_num=%d, pos_in_item=%d\n",
	  de->de_entry_key.k_dir_id, de->de_entry_key.k_objectid, 
	  k_offset_to_string (de->de_entry_key.k_offset, de->de_entry_key.k_uniqueness),
	  k_uniqueness_to_string (de->de_entry_key.k_uniqueness),
	  de->de_dir_id, de->de_objectid,
	  de->de_bh->b_blocknr, de->de_item_num, de->de_entry_num);
}

void print_bi (struct buffer_info * bi, char * mes)
{
  printk ("%s: bh->b_blocknr=%lu, bh->b_item_order=%d, bh->b_parent->b_blocknr=%lu\n", 
	  mes ? mes : "print_bi", bi->bi_bh->b_blocknr, bi->bi_position, bi->bi_parent ? bi->bi_parent->b_blocknr : 0);
}


static char * item_type (struct item_head * ih)
{
    static char * types[] = {
        "stat data", "directory", "direct", "indirect", "unknown"
    };

    if (I_IS_STAT_DATA_ITEM(ih))
        return types[0];
    if (I_IS_DIRECTORY_ITEM(ih))
        return types[1];
    if (I_IS_DIRECT_ITEM(ih))
        return types[2];
    if (I_IS_INDIRECT_ITEM(ih))
        return types[3];
    return types[4];
}

char * int2nameprefix (char * nameprefix, unsigned int num)
{
  int j, k;
  char * third;

  if (!num)
    {
      nameprefix[0] = '0';
      nameprefix[1] = 0;
      return nameprefix;
    }
             
  third = (char *)&num;
  for (j = 3, k = 0; j >= 0 && third[j]; j --, k ++)
    nameprefix[k] = third[j];
  nameprefix[k] = 0;

  return nameprefix;
}

static void check_directory_item (struct buffer_head * bh, struct item_head * ih)
{
  int i;
  int namelen;
  struct reiserfs_de_head * deh;

  if (!I_IS_DIRECTORY_ITEM (ih))
    return;

  deh = B_I_DEH (bh, ih);
  for (i = 0; i < I_ENTRY_COUNT (ih); i ++, deh ++) {
    namelen = I_DEH_N_ENTRY_FILE_NAME_LENGTH (ih, deh, i);
  }
}

void print_directory_item (struct buffer_head * bh, struct item_head * ih)
{
  int i;
  int namelen;
  struct reiserfs_de_head * deh;
  char * name;
  static char namebuf [80];

  if (!I_IS_DIRECTORY_ITEM (ih))
    return;

  printk ("\n # %-15s%-30s%-15s%-15s%-15s\n", "Name", "Key of pointed object", "Hash", "Gen number", "Status");
  deh = B_I_DEH (bh, ih);
  for (i = 0; i < I_ENTRY_COUNT (ih); i ++, deh ++) {
    namelen = I_DEH_N_ENTRY_FILE_NAME_LENGTH (ih, deh, i);
    name = B_I_DEH_ENTRY_FILE_NAME (bh, ih, deh);
    namebuf[0] = '"';
    if (namelen > sizeof (namebuf) - 3) {
      strncpy (namebuf + 1, name, sizeof (namebuf) - 3);
      namebuf[sizeof (namebuf) - 2] = '"';
      namebuf[sizeof (namebuf) - 1] = 0;
    } else {
      memcpy (namebuf + 1, name, namelen);
      namebuf[namelen + 1] = '"';
      namebuf[namelen + 2] = 0;
    }


    printk ("%d:  %-15s%-15d%-15d%-15d%-15d(%s)\n", 
            i, namebuf,
	    deh->deh_dir_id, deh->deh_objectid,
/*
            de_with_number(deh) ? (*(unsigned long *)(B_I_PITEM(bh,ih) + deh->deh_location + strlen (namebuf))) : 
	    ((namelen == 1 && name[0] == '.') ? ih->ih_key.k_dir_id : ih->ih_key.k_objectid),
            deh->deh_objectid,
*/
	    GET_HASH_VALUE (deh->deh_offset), GET_GENERATION_NUMBER (deh->deh_offset),
	    (de_hidden (deh)) ? "HIDDEN" : "VISIBLE");
  }
}


//
// printing of indirect item
//
static void start_new_sequence (__u32 * start, int * len, __u32 new)
{
    *start = new;
    *len = 1;
}

static int sequence_finished (__u32 start, int * len, __u32 new)
{
    if (start == INT_MAX)
	return 1;

    if (start == 0 && new == 0) {
	(*len) ++;
	return 0;
    }
    if (start != 0 && (start + *len) == new) {
	(*len) ++;
	return 0;
    }
    return 1;
}

static void print_sequence (__u32 start, int len)
{
    if (start == INT_MAX)
	return;

    if (len == 1)
	printk (" %d", start);
    else
	printk (" %d(%d)", start, len);
}

void print_indirect_item (struct buffer_head * bh, int item_num)
{
    struct item_head * ih;
    int j;
    __u32 * unp, prev = INT_MAX;
    int num;

    ih = B_N_PITEM_HEAD (bh, item_num);
    unp = (__u32 *)B_I_PITEM (bh, ih);

    if (ih->ih_item_len % UNFM_P_SIZE)
	printk ("print_indirect_item: invalid item len");  

    printk ("%d pointers\n[ ", I_UNFM_NUM (ih));
    for (j = 0; j < I_UNFM_NUM (ih); j ++) {
	if (sequence_finished (prev, &num, unp[j])) {
	    print_sequence (prev, num);
	    start_new_sequence (&prev, &num, unp[j]);
	}
    }
    print_sequence (prev, num);
    printk ("]\n");
}



char timebuf[256];

char * timestamp (time_t t)
{
#ifndef __KERNEL__
  strftime (timebuf, 256, "%m/%d/%Y %T", localtime (&t));
#else
  sprintf (timebuf, "%d", (int) t);
#endif
  return timebuf;
}


/* this prints internal nodes (4 keys/items in line) (dc_number,
   dc_size)[k_dirid, k_objectid, k_offset, k_uniqueness](dc_number,
   dc_size)...*/
static int print_internal (struct buffer_head * bh, int first, int last)
{
    struct key * key;
    struct disk_child * dc;
    int i;
    int from, to;

    if (!B_IS_KEYS_LEVEL (bh))
	return 1;

    if (first == -1) {
	from = 0;
	to = B_NR_ITEMS (bh);
    } else {
	from = first;
	to = last < B_NR_ITEMS (bh) ? last : B_NR_ITEMS (bh);
    }

    reiserfs_warning ("INTERNAL NODE (%ld) contains %z\n",  bh->b_blocknr, bh);

    dc = B_N_CHILD (bh, from);
    reiserfs_warning ("PTR %d: %y ", from, dc);

    for (i = from, key = B_N_PDELIM_KEY (bh, from), dc ++; i < to; i ++, key ++, dc ++) {
	reiserfs_warning ("KEY %d: %k PTR %d: %y ", i, key, i + 1, dc);
	if (i && i % 4 == 0)
	    printk ("\n");
    }
    printk ("\n");
    return 0;
}


static int is_symlink = 0;
static int print_leaf (struct buffer_head * bh, int print_mode, int first, int last)
{
    struct block_head * blkh;
    struct item_head * ih;
    int i;
    int from, to;

    if (!B_IS_ITEMS_LEVEL (bh))
	return 1;

    blkh = B_BLK_HEAD (bh);
    ih = B_N_PITEM_HEAD (bh,0);

    printk ("\n===================================================================\n");
    reiserfs_warning ("LEAF NODE (%ld) contains %z\n", bh->b_blocknr, bh);

    if (!(print_mode & PRINT_LEAF_ITEMS)) {
	reiserfs_warning ("FIRST ITEM_KEY: %k, LAST ITEM KEY: %k\n",
			  &(ih->ih_key), &((ih + blkh->blk_nr_item - 1)->ih_key));
	return 0;
    }

    if (first < 0 || first > blkh->blk_nr_item - 1) 
	from = 0;
    else 
	from = first;

    if (last < 0 || last > blkh->blk_nr_item)
	to = blkh->blk_nr_item;
    else
	to = last;


    printk ("------------------------------------------------------------------------------------------------------------------\n");
    printk ("|##|   type    |           key           | ilen | free_space | reserved | loc  |   mode  |  size  | nl | direct byte | mtime | ctime | atime |\n");
    for (i = from; i < to; i++) {
	printk ("------------------------------------------------------------------------------------------------------------\n");
	printk ("|%2d| %9s | %5d %5d %5d %5d | %4d | %10d | %8d | %4d |",
		i, item_type(ih+i), ih[i].ih_key.k_dir_id, ih[i].ih_key.k_objectid, ih[i].ih_key.k_offset, ih[i].ih_key.k_uniqueness,
		ih[i].ih_item_len, ih[i].u.ih_free_space, ih[i].ih_reserved, ih[i].ih_item_location);

	if (I_IS_STAT_DATA_ITEM(ih+i)) {
	    struct stat_data * sd = B_I_STAT_DATA (bh,ih+i);

	    printk (" 0%-6o | %6u | %2u | %d | %s | %s | %s |\n", sd->sd_mode, sd->sd_size, sd->sd_nlink, sd->sd_first_direct_byte, 
		    timestamp (sd->sd_mtime), timestamp (sd->sd_ctime), timestamp (sd->sd_atime));
	    is_symlink = (S_ISLNK(sd->sd_mode)) ? 1 : 0;
	    continue;
	}
	printk ("\n");
	if (I_IS_DIRECTORY_ITEM(ih+i) && print_mode & PRINT_DIRECTORY_ITEMS) {
	    print_directory_item (bh, ih+i);
	    continue;
	}

	if (I_IS_INDIRECT_ITEM(ih+i)) {
	    print_indirect_item (bh, i);
	    continue;
	}

	if (I_IS_DIRECT_ITEM(ih+i)) {
	    int j = 0;
	    if (is_symlink || print_mode & PRINT_DIRECT_ITEMS) {
		printk ("\"");
		while (j < ih[i].ih_item_len)
		    printk ("%c", B_I_PITEM(bh,ih+i)[j++]);
		printk ("\"\n");
	    }
	    continue;
	}
    }
    printk ("===================================================================\n");
    return 0;
}

/*
char buf[20];

#include <linux/kdev_t.h>
static char * devname (int dev)
{
  struct stat st;

  if (fstat (dev, &st) != 0)
    die ("stat failed");
  sprintf (buf, "0x%x:0x%x", MAJOR((int)st.st_rdev), MINOR((int)st.st_rdev));
  return buf;
}
*/

static char * reiserfs_version (char * buf)
{
    __u16 * pversion;

    pversion = (__u16 *)(buf + 30);
    if (*pversion == 0)
	return "0";
    if (*pversion == 2)
	return "2";
    return "Unknown";
}

/* return 1 if this is not super block */
static int print_super_block (struct buffer_head * bh)
{
    struct reiserfs_super_block * rs = (struct reiserfs_super_block *)(bh->b_data);
    int skipped, data_blocks;
    

    if (strncmp (rs->s_magic,  REISERFS_SUPER_MAGIC_STRING, strlen ( REISERFS_SUPER_MAGIC_STRING)))
	return 1;

    printk ("%s\'s super block in block %ld\n======================\n", kdevname (bh->b_dev), bh->b_blocknr);
    printk ("Reiserfs version %s\n", reiserfs_version (bh->b_data));

#if 0
    printk ("-------------------------------------------------------------------------------------------------------------------\n");
    printk ("| block count | free block | used block | blocksize | tree height |  state  |   magic   | root block | bmap count |\n");
    printk ("|             |   count    |  count     |           |             |         |           |            |            |\n");
    printk ("|-----------------------------------------------------------------------------------------------------------------|\n");
    printk ("| %11d | %10d | %10d | %9d | %11d | %7s | %9s | %10d | %10d |\n", 
	    rs->s_block_count, rs->s_free_blocks, rs->s_block_count - rs->s_free_blocks, rs->s_blocksize, rs->s_tree_height, 
	    (rs->s_state == REISERFS_VALID_FS) ? "VALID" : "ERROR", rs->s_magic, rs->s_root_block, rs->s_bmap_nr);
    printk ("|-----------------------------------------------------------------------------------------------------------------|\n\n");
#endif

    printk ("Block count %u\n", rs->s_block_count);
    printk ("Blocksize %d\n", rs->s_blocksize);
    printk ("Free blocks %u\n", rs->s_free_blocks);
    skipped = bh->b_blocknr; // FIXME: this would be confusing if
    // someone stores reiserfs super block in reiserfs ;)
    data_blocks = rs->s_block_count - skipped - 1 -
	rs->s_bmap_nr - (rs->s_orig_journal_size + 1) - rs->s_free_blocks;
    printk ("Busy blocks (skipped %d, bitmaps - %d, journal blocks - %d\n"
	    "1 super blocks, %d data blocks\n", 
	    skipped, rs->s_bmap_nr, 
	    (rs->s_orig_journal_size + 1), data_blocks);
    printk ("Root block %u\n", rs->s_root_block);
    printk ("Journal block (first?) %d\n", rs->s_journal_block);
    printk ("Journal dev %d\n", rs->s_journal_dev);    
    printk ("Journal orig size %d\n", rs->s_orig_journal_size);
    printk ("Filesystem state %s\n", (rs->s_state == REISERFS_VALID_FS) ? "VALID" : "ERROR");

#if 0
    __u32 s_journal_trans_max ;           /* max number of blocks in a transaction.  */
    __u32 s_journal_block_count ;         /* total size of the journal. can change over time  */
    __u32 s_journal_max_batch ;           /* max number of blocks to batch into a trans */
    __u32 s_journal_max_commit_age ;      /* in seconds, how old can an async commit be */
    __u32 s_journal_max_trans_age ;       /* in seconds, how old can a transaction be */
#endif
    printk ("Tree height %d\n", rs->s_tree_height);
    return 0;
}


static int print_desc_block (struct buffer_head * bh)
{
    struct reiserfs_journal_desc * desc;

    desc = (struct reiserfs_journal_desc *)(bh->b_data);
    if (memcmp(desc->j_magic, JOURNAL_DESC_MAGIC, 8))
	return 1;

    printk ("Desc block %lu (j_trans_id %ld, j_mount_id %ld, j_len %ld)",
	    bh->b_blocknr, desc->j_trans_id, desc->j_mount_id, desc->j_len);

    return 0;
}


void print_block (struct buffer_head * bh, ...)//int print_mode, int first, int last)
{
    va_list args;
    int mode, first, last;

    va_start (args, bh);

    if ( ! bh ) {
	printk("print_block: buffer is NULL\n");
	return;
    }

    mode = va_arg (args, int);
    first = va_arg (args, int);
    last = va_arg (args, int);
    if (print_leaf (bh, mode, first, last))
	if (print_internal (bh, first, last))
	    if (print_super_block (bh))
		if (print_desc_block (bh))
		    printk ("Block %ld contains unformatted data\n", bh->b_blocknr);
}



void print_tb (int mode, int item_pos, int pos_in_item, struct tree_balance * tb, char * mes)
{
  int h = 0;
  int i;
  struct buffer_head * tbSh, * tbFh;


  if (!tb)
    return;

  printk ("\n********************** PRINT_TB for %s *******************\n", mes);
  printk ("MODE=%c, ITEM_POS=%d POS_IN_ITEM=%d\n", mode, item_pos, pos_in_item);
  printk ("*********************************************************************\n");

  printk ("* h *    S    *    L    *    R    *   F   *   FL  *   FR  *  CFL  *  CFR  *\n");
/*
01234567890123456789012345678901234567890123456789012345678901234567890123456789
       1        2         3         4         5         6         7         8
  printk ("*********************************************************************\n");
*/
  
  
  for (h = 0; h < sizeof(tb->insert_size) / sizeof (tb->insert_size[0]); h ++) {
    if (PATH_H_PATH_OFFSET (tb->tb_path, h) <= tb->tb_path->path_length && 
	PATH_H_PATH_OFFSET (tb->tb_path, h) > ILLEGAL_PATH_ELEMENT_OFFSET) {
      tbSh = PATH_H_PBUFFER (tb->tb_path, h);
      tbFh = PATH_H_PPARENT (tb->tb_path, h);
    } else {
      /*      printk ("print_tb: h=%d, PATH_H_PATH_OFFSET=%d, path_length=%d\n", 
	      h, PATH_H_PATH_OFFSET (tb->tb_path, h), tb->tb_path->path_length);*/
      tbSh = 0;
      tbFh = 0;
    }
    printk ("* %d * %3ld(%2d) * %3ld(%2d) * %3ld(%2d) * %5ld * %5ld * %5ld * %5ld * %5ld *\n",
	    h, 
	    (tbSh) ? (tbSh->b_blocknr):(-1),
	    (tbSh) ? tbSh->b_count : -1,
	    (tb->L[h]) ? (tb->L[h]->b_blocknr):(-1),
	    (tb->L[h]) ? tb->L[h]->b_count : -1,
	    (tb->R[h]) ? (tb->R[h]->b_blocknr):(-1),
	    (tb->R[h]) ? tb->R[h]->b_count : -1,
	    (tbFh) ? (tbFh->b_blocknr):(-1),
	    (tb->FL[h]) ? (tb->FL[h]->b_blocknr):(-1),
	    (tb->FR[h]) ? (tb->FR[h]->b_blocknr):(-1),
	    (tb->CFL[h]) ? (tb->CFL[h]->b_blocknr):(-1),
	    (tb->CFR[h]) ? (tb->CFR[h]->b_blocknr):(-1));
  }

  printk ("*********************************************************************\n");


  /* print balance parameters for leaf level */
  h = 0;
  printk ("* h * size * ln * lb * rn * rb * blkn * s0 * s1 * s1b * s2 * s2b * curb * lk * rk *\n");
  printk ("* %d * %4d * %2d * %2d * %2d * %2d * %4d * %2d * %2d * %3d * %2d * %3d * %4d * %2d * %2d *\n",
	  h, tb->insert_size[h], tb->lnum[h], tb->lbytes, tb->rnum[h],tb->rbytes, tb->blknum[h], 
	  tb->s0num, tb->s1num,tb->s1bytes,  tb->s2num, tb->s2bytes, tb->cur_blknum, tb->lkey[h], tb->rkey[h]);


/* this prints balance parameters for non-leaf levels */
  do {
    h++;
    printk ("* %d * %4d * %2d *    * %2d *    * %2d *\n",
    h, tb->insert_size[h], tb->lnum[h], tb->rnum[h], tb->blknum[h]);
  } while (tb->insert_size[h]);

  printk ("*********************************************************************\n");


  /* print FEB list (list of buffers in form (bh (b_blocknr, b_count), that will be used for new nodes) */
  h = 0;
  for (i = 0; i < sizeof (tb->FEB) / sizeof (tb->FEB[0]); i ++)
    printk ("%s%p (%lu %d)", i == 0 ? "FEB list: " : ", ", tb->FEB[i], tb->FEB[i] ? tb->FEB[i]->b_blocknr : 0,
	    tb->FEB[i] ? tb->FEB[i]->b_count : 0);
  printk ("\n");

  printk ("********************** END OF PRINT_TB *******************\n\n");

}


void print_bmap_block (int i, struct buffer_head * bmap, int blocks, int silent)
{
    int j, k;
    int bits = bmap->b_size * 8;
    int zeros = 0, ones = 0;
  
    printk ("#%d: block %lu: ", i, bmap->b_blocknr);

    if (test_bit (0, bmap->b_data)) {
	/* first block addressed by this bitmap block is used */
	ones ++;
	if (!silent)
	    printk ("Busy (%d-", i * bits);
	for (j = 1; j < blocks; j ++) {
	    while (test_bit (j, bmap->b_data)) {
		ones ++;
		if (j == blocks - 1) {
		    if (!silent)
			printk ("%d)\n", j + i * bits);
		    goto end;
		}
		j++;
	    }
	    if (!silent)
		printk ("%d) Free(%d-", j - 1 + i * bits, j + i * bits);

	    while (!test_bit (j, bmap->b_data)) {
		zeros ++;
		if (j == blocks - 1) {
		    if (!silent)
			printk ("%d)\n", j + i * bits);
		    goto end;
		}
		j++;
	    }
	    if (!silent)
		printk ("%d) Busy(%d-", j - 1 + i * bits, j + i * bits);

	    j --;
	end:
	}
    } else {
	/* first block addressed by this bitmap is free */
	zeros ++;
	if (!silent)
	    printk ("Free (%d-", i * bits);
	for (j = 1; j < blocks; j ++) {
	    k = 0;
	    while (!test_bit (j, bmap->b_data)) {
		k ++;
		if (j == blocks - 1) {
		    if (!silent)
			printk ("%d)\n", j + i * bits);
		    zeros += k;
		    goto end2;
		}
		j++;
	    }
	    zeros += k;
	    if (!silent)
		printk ("%d) Busy(%d-", j - 1 + i * bits, j + i * bits);
	    
	    k = 0;
	    while (test_bit (j, bmap->b_data)) {
		ones ++;
		if (j == blocks - 1) {
		    if (!silent)
			printk ("%d)\n", j + i * bits);
		    ones += k;
		    goto end2;
		}
		j++;
	    }
	    ones += k;
	    if (!silent)
		printk ("%d) Free(%d-", j - 1 + i * bits, j + i * bits);
	
	    j --;
	end2:
	}
    }

    printk ("used %d, free %d\n", ones, zeros);
}


/* if silent == 1, do not print details */
void print_bmap (struct super_block * s, int silent)
{
    int bmapnr = SB_BMAP_NR (s);
    int i;
    int blocks = s->s_blocksize * 8; /* adressed by bitmap */

    printk ("Bitmap blocks are:\n");
    for (i = 0; i < bmapnr; i ++) {

	if (i == bmapnr - 1)
	    if (SB_BLOCK_COUNT (s) % (s->s_blocksize * 8))
		blocks = SB_BLOCK_COUNT (s) % (s->s_blocksize * 8);
	print_bmap_block (i, SB_AP_BITMAP(s)[i], blocks, silent);
    }

    /* check unused part of last bitmap */
    {
	int bad_unused_bitmap = 0;
	int ones;

	ones = s->s_blocksize * 8 - SB_BLOCK_COUNT (s) % (s->s_blocksize * 8);
	if (ones == s->s_blocksize * 8)
	    ones = 0;
      
	for (i = s->s_blocksize * 8; --i >= blocks; )
	    if (!test_bit (i, SB_AP_BITMAP (s)[bmapnr - 1]->b_data))
		bad_unused_bitmap ++;

	if (bad_unused_bitmap) {
	    printk ("Unused part of bitmap is wrong: should be %d ones, found %d zeros\n",
		    ones, bad_unused_bitmap);
	}
    }
    
}



void print_objectid_map (struct super_block * s)
{
  int i;
  struct reiserfs_super_block * rs;
  unsigned long * omap;

  rs = SB_DISK_SUPER_BLOCK (s);
  omap = (unsigned long *)(rs + 1);
  printk ("Map of objectids\n");
      
  for (i = 0; i < rs->s_oid_cursize; i ++) {
    if (i % 2 == 0)
      printk ("busy(%lu-%lu) ", omap[i], omap[i+1] - 1); 
    else
      printk ("free(%lu-%lu) ", 
	      omap[i], ((i+1) == rs->s_oid_cursize) ? -1 : omap[i+1] - 1);
    }
  printk ("\n");
  
  printk ("Object id array has size %d (max %d):", rs->s_oid_cursize, 
	  rs->s_oid_maxsize);
  
  for (i = 0; i < rs->s_oid_cursize; i ++)
    printk ("%lu ", omap[i]); 
  printk ("\n");

}


static void check_leaf_block_head (struct buffer_head * bh)
{
  struct block_head * blkh;

  blkh = B_BLK_HEAD (bh);
  if (le16_to_cpu (blkh->blk_nr_item) > (bh->b_size - BLKH_SIZE) / IH_SIZE)
    reiserfs_panic (0, "vs-6010: check_leaf_block_head: invalid item number %z", bh);
  if (le16_to_cpu (blkh->blk_free_space) > 
      bh->b_size - BLKH_SIZE - IH_SIZE * le16_to_cpu (blkh->blk_nr_item))
    reiserfs_panic (0, "vs-6020: check_leaf_block_head: invalid free space %z", bh);
    
}

static void check_internal_block_head (struct buffer_head * bh)
{
  struct block_head * blkh;

  return  ;
  blkh = B_BLK_HEAD (bh);
  if (!(le16_to_cpu (blkh->blk_level) > DISK_LEAF_NODE_LEVEL && le16_to_cpu (blkh->blk_level) <= MAX_HEIGHT))
    reiserfs_panic (0, "vs-6025: check_internal_block_head: invalid level %z", bh);

  if (le16_to_cpu (blkh->blk_nr_item) > (bh->b_size - BLKH_SIZE) / IH_SIZE)
    reiserfs_panic (0, "vs-6030: check_internal_block_head: invalid item number %z", bh);

  if (le16_to_cpu (blkh->blk_free_space) != 
      bh->b_size - BLKH_SIZE - KEY_SIZE * le16_to_cpu (blkh->blk_nr_item) - DC_SIZE * (blkh->blk_nr_item + 1))
    reiserfs_panic (0, "vs-6040: check_internal_block_head: invalid free space %z", bh);

}


void check_leaf (struct buffer_head * bh)
{
  int i;
  struct item_head * ih;

  if (!bh)
    return;
  check_leaf_block_head (bh);
  for (i = 0, ih = B_N_PITEM_HEAD (bh, 0); i < B_NR_ITEMS (bh); i ++, ih ++) {
    if (I_IS_DIRECTORY_ITEM (ih))
      check_directory_item (bh, ih);
  }
}


void check_internal (struct buffer_head * bh)
{
  if (!bh)
    return;
  check_internal_block_head (bh);
}


void print_statistics (struct super_block * s)
{
  /*
  printk ("reiserfs_put_super: session statistics: balances %d, fix_nodes %d, preserve list freeings %d, \
bmap with search %d, without %d, dir2ind %d, ind2dir %d\n",
	  s->u.reiserfs_sb.s_do_balance, s->u.reiserfs_sb.s_fix_nodes, s->u.reiserfs_sb.s_preserve_list_freeings,
	  s->u.reiserfs_sb.s_bmaps, s->u.reiserfs_sb.s_bmaps_without_search,
	  s->u.reiserfs_sb.s_direct2indirect, s->u.reiserfs_sb.s_indirect2direct);
  */

}
