/*
 * Copyright 1999-2000, 1998 Hans Reiser
 */


#include "nokernel.h"
#include "reiserfs.h"
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

int is_block_unformatted (__u32 b)
{
  return 0;
}


struct super_block g_sb;
extern struct key root_key;


/*------------------------------------------------------------------------*/

/* de->d_iname contains name */
static struct inode * get_inode_by_name (struct inode * dir, struct dentry * dentry)
{
  dentry->d_name.len = strlen (dentry->d_iname);
  dentry->d_name.name = dentry->d_iname;

  reiserfs_lookup (dir, dentry);

  return dentry->d_inode;
}


struct inode * pwd;

static void do_create (char * args)
{
  struct dentry de;

  if (sscanf (args, "%255s", de.d_iname) != 1) {
    reiserfs_warning ("create: usage: create filename\n");
    return;
  }

  if (get_inode_by_name (pwd, &de) == 0) {
    reiserfs_create (pwd, &de, 0100644);
  } else
    reiserfs_warning ("create: file %s exists\n", de.d_name.name);

}

static void do_mkdir (char * args)
{
  struct dentry de;

  if (sscanf (args, "%255s", de.d_iname) != 1) {
    reiserfs_warning ("mkdir: usage: mkdir dirname\n");
    return;
  }

  if (get_inode_by_name (pwd, &de) == 0) {
    reiserfs_mkdir (pwd, &de, 0100644);
  } else
    reiserfs_warning ("mkdir: dir %s exists\n", de.d_name.name);
}

static void do_rmdir (char * args)
{
  struct dentry de;

  if (sscanf (args, "%255s", de.d_iname) != 1) {
    reiserfs_warning ("rmdir: usage: rmdir dirname\n");
    return;
  }

  if (get_inode_by_name (pwd, &de) != 0) {
    reiserfs_rmdir (pwd, &de);
  } else
    reiserfs_warning ("rmdir: dir %s is not exists\n", de.d_name.name);
}


static void do_write (char * args)
{
  int i;
  int count;
  loff_t offset;
  struct file file;
  char * buf;
  struct dentry de;

  if (sscanf (args, "%255s %Ld %d", de.d_iname, &offset, &count) != 3) {
    reiserfs_warning ("write: usage: write filename offset count\n");
    return;
  }

  if (get_inode_by_name (pwd, &de)) {
    file.f_dentry = &de;
    file.f_flags = 0;
    /* if regular file */
    file.f_op = reiserfs_file_inode_operations.default_file_ops;
    buf = (char *)malloc (count);
    if (buf == 0)
      reiserfs_panic (&g_sb, "do_write: no memory, or function not defined");
    for (i = 0; i < count; i ++)
      buf[i] = '0' + i % 10;

    file.f_op->write (&file, buf, count, &offset);
    free (buf);
  }
}

static void do_read (char * args)
{
  int count;
  loff_t offset;
  struct file file;
  char * buf;
  struct dentry de;

  if (sscanf (args, "%255s %Ld %d", de.d_iname, &offset, &count) != 3) {
    reiserfs_warning ("do_read: usage: read filename offset count\n");
    return;
  }

  if (get_inode_by_name (pwd, &de)) {
    file.f_dentry = &de;
    file.f_flags = 0;
    file.f_pos = 0;
    /* if regular file */
    file.f_op = reiserfs_file_inode_operations.default_file_ops;
    buf = (char *)malloc (count);
    if (buf == 0)
      reiserfs_panic (&g_sb, "do_read: no memory, or function not defined");
    memset (buf, 0, count);

    file.f_op->read (&file, buf, count, &offset);
    free (buf);
  }
}



static void do_fcopy (char * args)
{
    char * src;
    char * dest;
    int fd_source;
    int rd, bufsize;
    loff_t offset;
    char * buf;
    struct dentry de;
    struct inode * inode;
    struct file file;

    current->rlim[RLIMIT_FSIZE].rlim_cur = INT_MAX;
    src = args;
    src [strlen (src) - 1] = 0;
    dest = strrchr (args, '/') + 1;
    if (dest == 0)
	die ("/ must be in the name of source");

    fd_source = open (src, O_RDONLY);
    if (fd_source == -1) 
	die ("fcopy: could not open \"%s\": %s", 
	     src, strerror (errno));

    bufsize = 1024;
    buf = (char *)malloc (bufsize);
    if (buf == 0)
	reiserfs_panic (&g_sb, "fcopy: no memory, or function not defined");

    strcpy (de.d_iname, dest);

    if ((inode = get_inode_by_name (pwd, &de)) == 0) {
	reiserfs_create (pwd, &de, 0100644);
	inode = de.d_inode;
    } else {
	reiserfs_warning ("fcopy: file %s exists\n", de.d_name.name);
	return;
    }

  file.f_dentry = &de;
  file.f_flags = 0;
  file.f_error = 0;
  /* if regular file */
  file.f_op = reiserfs_file_inode_operations.default_file_ops;
    offset = 0;
    while ((rd = read(fd_source, buf, bufsize)) > 0)
      reiserfs_file_inode_operations.default_file_ops->write (&file, buf, rd, &offset);//	offset += _do_write (inode, rd, offset, buf);

    iput (inode);
    free (buf);
  
    close(fd_source);
#if 0
  char source[260];
  char dest[260];
  int fd_source;
  int rd,bufsize;
  loff_t offset;
  struct file file;
  char * buf;
  struct dentry de;

  if (sscanf (args, "%255s %255s", source, dest) != 2) {
    reiserfs_warning ("fcopy: usage: fcopy source dest\n");
    return;
  }

  fd_source=open(source, O_RDONLY);
  if (fd_source == -1) 
    reiserfs_panic (&g_sb, "fcopy: source file could not open");

  bufsize = 1024;
  buf = (char *)malloc (bufsize);
  if (buf == 0)
      reiserfs_panic (&g_sb, "fcopy: no memory, or function not defined");

  strcpy(de.d_iname,dest);


  if (get_inode_by_name (pwd, &de) == 0) {
    reiserfs_create (pwd, &de, 0100644);
  } else {
    reiserfs_warning ("fcopy: file %s exists\n", de.d_name.name);
    return;
  }
  

  file.f_dentry = &de;
  file.f_flags = 0;
  /* if regular file */
  file.f_op = reiserfs_file_inode_operations.default_file_ops;
  
  offset = 0;
  while((rd = read(fd_source, buf, bufsize)) > 0)
    {
      file.f_op->write (&file, buf, rd, &offset);
/*      offset += bufsize;*/
    }
  free (buf);
  
  close(fd_source);
#endif
}


/****************************** readdir *****************************/

struct linux_dirent {
	unsigned long	d_ino;
	unsigned long	d_off;
	unsigned short	d_reclen;
	char		d_name[1];
};

struct getdents_callback {
	struct linux_dirent * current_dir;
	struct linux_dirent * previous;
	int count;
	int error;
};

#define NAME_OFFSET(de) ((int) ((de)->d_name - (char *) (de)))
#define ROUND_UP(x) (((x)+sizeof(long)-1) & ~(sizeof(long)-1))
static int filldir(void * __buf, const char * name, int namlen, off_t offset, ino_t ino)
{
  struct linux_dirent * dirent;
  struct getdents_callback * buf = (struct getdents_callback *) __buf;
  int reclen = ROUND_UP(NAME_OFFSET(dirent) + namlen + 1);

  buf->error = -EINVAL;	/* only used if we fail.. */
  if (reclen > buf->count)
    return -EINVAL;
  dirent = buf->previous;
  if (dirent)
    put_user(offset, &dirent->d_off);
  dirent = buf->current_dir;
  buf->previous = dirent;
  put_user(ino, &dirent->d_ino);
  put_user(reclen, &dirent->d_reclen);
  copy_to_user(dirent->d_name, name, namlen);
  put_user(0, dirent->d_name + namlen);

  ((char *) dirent) += reclen;
  buf->current_dir = dirent;
  buf->count -= reclen;
  return 0;
}


int emu_getdents (struct file * file, void * dirent, int count)
{
  struct getdents_callback buf;
  struct linux_dirent * lastdirent;
  int error;

  buf.current_dir = (struct linux_dirent *) dirent;
  buf.previous = NULL;
  buf.count = count;
  buf.error = 0;
  file->f_op->readdir (file, &buf, filldir);
  error = buf.error;
  lastdirent = buf.previous;
  if (lastdirent) {
    put_user(file->f_pos, &lastdirent->d_off);
    error = count - buf.count;
  }

  return error;
}


void do_readdir (void)
{
  struct dentry de;
  struct file file;
  struct dirent * dirent;
  struct linux_dirent * p;

  de.d_inode = pwd;
  file.f_dentry = &de;
  file.f_pos = 0;
  file.f_op = reiserfs_dir_inode_operations.default_file_ops;

  dirent = (struct dirent *)malloc (sizeof (struct dirent));
  while (emu_getdents (&file, dirent, sizeof (struct dirent)) != 0) {
    p = (struct linux_dirent *)dirent;
    while (p->d_reclen && (char *)p + p->d_reclen < (char *)(dirent + 1)) {
      printf ("%s\n", p->d_name);
      p = (struct linux_dirent *)((char *)p + p->d_reclen);
    }
  }
  free (dirent);
}


int do_cd (char * args)
{
  struct inode * dir;
  struct dentry de;

  if (sscanf (args, "%255s", de.d_iname) != 1) {
    reiserfs_warning ("do_cd: usage: cd dirname\n");
    return 1;
  }
  dir = get_inode_by_name (pwd, &de);
  if (dir != 0 && S_ISDIR (dir->i_mode)) {
    pwd = dir;
    return 0;
  }
  reiserfs_warning ("do_cd: no such file or not a directory \"%s\"\n", de.d_iname);
  return 1;
}

char buf1[1024], buf2[1024];

/* path is in buf1 */
static int do_path_cd (char * path)
{
    char * p, * slash;

    strcpy (buf2, path);
    p = buf2;
/*
    while ((slash = strchr (p, '/'))) {
	*slash = 0;
	if (do_cd (p)) {
	    printf ("cd: wrong path element: %s\n", p);
	    return 1;
	}
	p = slash + 1;
    }
    if (do_cd (p)) {
    }
*/
    while (1) {
	slash = strchr (p, '/');
	if (slash)
	    *slash = 0;	
	if (do_cd (p)) {
	    printf ("cd: wrong path element: %s\n", p);
	    return 1;
	}
	if (!slash)
	    break;
	p = slash + 1;
    }
    return 0;
}


#include <dirent.h>

void do_dcopy (char * args)
{
  char name[256], * p;
  char command [256];
  DIR * d;
  struct dirent * de;
  struct stat st;

  if (sscanf (args, "%255s", name) != 1) {
    reiserfs_warning ("do_dcopy: usage: dcopy dirname\n");
    return;
  }
  if ((d = opendir (name)) == NULL || chdir (name) == -1) {
    printf ("%s\n", strerror (errno));
    return;
  }

  p = strrchr (name, '/');
  p ++;
  do_mkdir (p);
  if (do_cd (p))
    return;

  while ((de = readdir (d)) != NULL) {
    if (stat (de->d_name, &st) == -1) {
      printf ("%s\n", strerror (errno));
      return;
    }
    if (!S_ISREG (st.st_mode)) {
      printf ("%s skipped\n", de->d_name);
      continue;
    }
    printf ("%s/%s\n", name, de->d_name);
    sprintf (command, "%s/%s %s\n", name, de->d_name, de->d_name);
    do_fcopy (command);
  }
  
}


char buf1[1024], buf2[1024];

void do_diff (char * args)
{
  char orig[256];
  int fd, rd1, rd2;
  struct file file;
  struct dentry de;

  if (sscanf (args, "%80s %255s", de.d_iname, orig) != 2) {
    reiserfs_warning ("diff: usage: diff filename sourcefilename\n");
    return;
  }

  fd = open (orig, O_RDONLY);
  if (fd == -1) {
    printf ("%s\n", strerror (errno));
    return;
  }

  /* open file on reiserfs */
  if (get_inode_by_name (pwd, &de)) {
    file.f_dentry = &de;
    file.f_flags = 0;
    file.f_pos = 0;
    /* if regular file */
    file.f_op = reiserfs_file_inode_operations.default_file_ops;
  } else {
    printf ("No such file or directory\n");
    return;
  }
  while ((rd1 = read (fd, buf1, 1024)) > 0) {
    rd2 = file.f_op->read (&file, buf2, 1024, &file.f_pos);
    if (rd1 != rd2) {
      printf ("Read error 1\n");
      return;
    }
    if (memcmp (buf1, buf2, rd1)) {
      printf ("Read error 2\n");
      return;
    }
  }
}


int do_delete (char * args)
{
  struct dentry de;

  if (sscanf (args, "%255s", de.d_iname) != 1) {
    reiserfs_warning ("delete: usage: delete filename\n");
    return 1;
  }
  if (get_inode_by_name (pwd, &de) == 0 || !S_ISREG (de.d_inode->i_mode)) {
    reiserfs_warning ("delete: file %s does not exist or not a directory\n",
		      de.d_name.name);
    return 1;
  }	
  reiserfs_unlink (pwd, &de);
  reiserfs_delete_inode (de.d_inode);
  return 0;
}


void do_for_each_name (void)
{
  struct dentry de;
  struct file file;
  char * buf;
  struct linux_dirent * p;

  de.d_inode = pwd;
  file.f_dentry = &de;
  file.f_pos = 0;
  file.f_op = reiserfs_dir_inode_operations.default_file_ops;

  buf = (char *)malloc (1024);
  while (emu_getdents (&file, buf, 1024) != 0) {
    p = (struct linux_dirent *)buf;
    while (p->d_reclen && (char *)p + p->d_reclen < (buf + 1024)) {
      printf ("Deleting %s.. %s\n", p->d_name, 
	      do_delete (p->d_name) ? "skipped" : "done");
      
      p = (struct linux_dirent *)((char *)p + p->d_reclen);
    }
  }

  free (buf);
}


void do_rm_rf (char * args)
{
  struct dentry de;

  if (sscanf (args, "%255s", de.d_iname) != 1) {
    reiserfs_warning ("rm_rf: usage: rm_rf dirname\n");
    return;
  }

  if (do_cd (de.d_iname))
    return;
  do_for_each_name ();
}


static void do_cd_root (void)
{
    if (pwd)
	iput (pwd);
    pwd = reiserfs_iget (&g_sb, &root_key);
    //pwd = iget4 (&g_sb, REISERFS_ROOT_OBJECTID, 0, REISERFS_ROOT_PARENT_OBJECTID);
}


/* args is name of file which contains list of files to be copied and
   directories to be created */
void do_batch (char * args)
{
    FILE * list;
    char * path;
    char * name;

    args[strlen (args) - 1] = 0;
    list = fopen (args, "r");
    if (list == 0) {
	printf ("do_batch: fopen failed on \'%s\': %s\n", args, 
		strerror (errno));
	return;
    }
    while (fgets (buf1, sizeof (buf1), list) != 0) {
	do_cd_root ();

	/* remove ending \n */
       	buf1[strlen (buf1) - 1] = 0;
	
	/* select last name */
	path = buf1;
	name = path + strlen (buf1) - 1;
	if (*name == '/')
	    name --;
	while (*name != '/' && name != path)
	    name --;
	if (*name == '/')
	    *name++ = 0;
	if (name == path)
	    path = 0;

	printf ("cd to %s..", path);
	if (path && do_path_cd (path)) {
	    printf ("do_batch: cd failed\n");
	    return;
	}
	printf ("ok, ");

	if (name [strlen (name) - 1] == '/') {
	    name [strlen (name) - 1] = 0;
	    printf ("mkdir %s..", name);
	    do_mkdir (name);
	} else {
	    printf ("cp %s..", name);
	    sprintf (buf2, "%s/%s\n", path, name);
	    do_fcopy (buf2);
	}
	printf ("done\n");
    }
    printf ("Ok\n");
    fclose (list);
}



void do_help (void)
{
  printf (" create <file_name>                   - create the file\n");
  printf (" mkdir <dir_name>                    - create the directory\n");
  printf (" rmdir <dir_name>                    - remove the directory\n");
  printf (" write <file_name> <offset> <count>  - write to the file\n");  
  printf (" read <file_name> <offset> <count>  - read from the file\n");  
  printf (" fcopy  <source> <dest>               - copy files: source(ext2) to dest(reiserfs)\n");  
  printf (" ls                                   - read current directory\n");
  printf (" cd <dir_name>                        - change directory\n");
  printf (" dcopy  <source> - copy directory to cwd with the same name (regular files only)\n");
  printf (" diff file1 file2       - compare reiserfs file2 with file1\n");
  printf (" delete file\n");
  printf (" rm_rf dirname          - delete all files in the directory\n");
  printf (" batch filelist\n");
  printf (" q                                    - quit emu\n");
}


void release_bitmaps (struct super_block * s)
{
  int i;

  for (i = 0; i < SB_BMAP_NR (s); i ++) {
    /* brelse (SB_AP_CAUTIOUS_BITMAP (s)[i]); */
    brelse (SB_AP_BITMAP (s)[i]);
  }
  
  kfree (SB_AP_BITMAP (s));
  /* reiserfs_kfree (SB_AP_CAUTIOUS_BITMAP (s), sizeof (struct buffer_head *) * SB_BMAP_NR (s), s); */
}


void init_pwd (void)
{
  struct path path;
  
  path.path_length = ILLEGAL_PATH_ELEMENT_OFFSET;
  pwd = reiserfs_iget (&g_sb, &root_key);
}

int is_block_used (__u32 b)
{
  return 1;
}

int is_formatted_pointed_by_indirect (__u32 b)
{
  return 0;
}


int main (int argc, char * argv [])
{
    char cmd[81];
    char * file_name;
    int dev;

    printf ("\n<-----------REISERFS EMU, version 0.91a, 1999----------->\n");

    if(argc < 2) 
	die ("Usage: emu <device>\n");

    file_name = argv[1];

    /* open_device will die if it could not open device */
    dev = open (file_name, O_RDWR);
    if (dev == -1)
	reiserfs_panic (0, "emu: can not open '%s': %s", file_name, strerror (errno));

    g_sb.s_dev = dev;
    set_super(&g_sb) ;
    if (uread_super_block (&g_sb))
	die ("emu: no reiserfs found on %s", file_name);

    if (uread_bitmaps (&g_sb))
	die ("emu: read_bitmap failed");


    journal_init(&g_sb) ;
 
    /* check whether device contains mounted tree file system */
    if (is_mounted (file_name))
	reiserfs_panic (0, "emu: '%s' contains a not mounted file system\n", file_name);

    init_pwd ();


    while (1) {
	printf ("Enter command: >");
	fgets (cmd, 80, stdin);
	if (strncasecmp (cmd, "create ", 7) == 0)
	    do_create (cmd + 7);
	else if (strncasecmp (cmd, "delete ", 7) == 0)
	    do_delete (cmd + 7);
	else if (strncasecmp (cmd, "write ", 6) == 0)
	    do_write (cmd + 6);    
	else if (strncasecmp (cmd, "read ", 5) == 0)
	    do_read (cmd + 5);    
	else if (strncasecmp (cmd, "mkdir ", 6) == 0)
	    do_mkdir (cmd + 6);    
	else if (strncasecmp (cmd, "rmdir ", 6) == 0)
	    do_rmdir (cmd + 6);    
	else if (strncasecmp (cmd, "dcopy ", 6) == 0)
	    do_dcopy (cmd + 6);
	else if (strncasecmp (cmd, "fcopy ", 6) == 0)
	    do_fcopy (cmd + 6);    
	else if (strncasecmp (cmd, "ls", 2) == 0)
	    do_readdir ();
	else if (strncasecmp (cmd, "cd ", 3) == 0)
	    do_cd (cmd + 3);
	else if (strncasecmp (cmd, "diff ", 5) == 0)
	    do_diff (cmd + 5);
	else if (strncasecmp (cmd, "rm_rf ", 6) == 0)
	    do_rm_rf (cmd + 6);
	else if (strncasecmp (cmd, "batch ", 6) == 0)
	    do_batch (cmd + 6);
	else if (strncmp (cmd, "QUIT", strlen ("QUIT")) == 0)
	    break;
	else if (strncmp (cmd, "q", strlen ("q")) == 0)
	    break;
	else {
	    do_help ();
	}
    }
    sync_inodes ();
    release_bitmaps (&g_sb);
    brelse (g_sb.u.reiserfs_sb.s_sbh);
    journal_release(0, &g_sb) ;
    fsync_dev(dev) ;
    check_and_free_mem ();
    return 0;
}

