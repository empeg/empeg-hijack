#define __NO_VERSION__

#include<linux/version.h>
#include<linux/config.h>
#include<linux/kernel.h>
#include<linux/module.h> 
#include<linux/fs.h>
#include<linux/malloc.h> 
#include<linux/locks.h>
#include<linux/string.h>
#include<linux/file.h>
#include<linux/ctype.h>
#include<linux/pagemap.h>

#include<asm/uaccess.h>
#include<asm/pgtable.h>
#include<asm/system.h>


#include"ftpfs.h"
static int
ftp_readpage(struct file *f, struct page *p){
	struct dentry *dentry = f->f_dentry;
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct ftp_sb_info *info = (struct ftp_sb_info*)sb->u.generic_sbp;
	char buf[FTP_MAXLINE];
	char *buffer;
	unsigned long offset;
	int count, result;
	
	buffer = (char*)page_address(p);
	offset = p->offset;
	count = PAGE_SIZE;

	set_bit(PG_locked, &p->flags);
	atomic_inc(&p->count);
	clear_bit(PG_uptodate, &p->flags);
	clear_bit(PG_error, &p->flags);
	
	do{
		result = ftp_read(dentry, offset, count, buffer);
		if(result < 0){
			result = -EIO;
			DEBUG(" IO error!\n");
			goto io_error;
		}
		count -= result;
		offset += result;
		buffer += result;
		dentry->d_inode->i_atime = CURRENT_TIME;
		if(!result) break;
		
	}while(count);

	memset(buffer, 0, count);
	flush_dcache_page(page_address(p));
	set_bit(PG_uptodate, &p->flags);
	result = 0;

io_error:
	clear_bit(PG_locked, &p->flags);
	wake_up(&p->wait);
	free_page(page_address(p));
	return result;
}

static int
ftp_file_permission(struct inode *inode, int mask){
	int mode = inode->i_mode;

	DEBUG(" mode=%x, mask=%x\n", mode, mask);

	mode >>= 6;
	if((mode & 7 & mask) != mask) 
		return -EACCES;
	return 0;
}

static int
ftp_file_open(struct inode* inode, struct file* f){
	DEBUG(" opening , d_count=%d\n", f->f_dentry->d_count);
	return 0;
}

static int
ftp_file_release(struct inode *inode, struct file* f){
	struct dentry *dentry = f->f_dentry;
	if(dentry->d_count == 1){
		DEBUG(" should do some shit here, dude!\n");
	}
	return 0;
}

static struct file_operations ftp_file_operations = {
	NULL,			// lseek
	generic_file_read,// read
	NULL,			// write
	NULL,			// readdir
	NULL,			// poll
	NULL,			// ioctl
	generic_file_mmap,// mmap
	ftp_file_open,	// open
	NULL,			// flush
	ftp_file_release,// release
	NULL,			// fsync
	NULL,			// fasync
	NULL,			// check_media_change
	NULL,			// revalidate
	NULL			// lock
};

struct inode_operations ftp_file_inode_operations = {
	&ftp_file_operations,	//dir ops
	NULL,					//create
	NULL,					//lookup
	NULL,					//link
	NULL,					//unlink
	NULL,					//symlink
	NULL,					//mkdir
	NULL,					//rmdir
	NULL,					//mknod
	NULL,					//rename
	NULL,					//readlink
	NULL,					//followlink
	ftp_readpage,			//readpage
	NULL,					//writepage
	NULL,					//bmap
	NULL,					//truncate
	ftp_file_permission,	//permission
	NULL,					//smap
	NULL,					//update page
	NULL					//revalidate
};
