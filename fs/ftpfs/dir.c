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

#include<asm/uaccess.h>


#include"ftpfs.h"

static int
ftp_lookup_validate(struct dentry* dentry, int flags){
//	DEBUG(" Oops!\n");
	int valid;
	struct inode *inode = dentry->d_inode;
	unsigned long age = jiffies - dentry->d_time;

	valid = ( age < FTP_MAX_AGE);
	if( inode ){
		if(is_bad_inode(inode)) valid = 0;
	}
	
	return valid;
}

static int
ftp_hash_dentry(struct dentry *dir, struct qstr* name){
	unsigned long hash;
	int i;
	
//	DEBUG(" Working here!\n");

	hash = init_name_hash();
	for(i = 0; i < name->len; i++)
		hash = partial_name_hash(tolower(name->name[i]), hash);
	name->hash = end_name_hash(hash);
	return 0;
}

static int
ftp_compare_dentry(struct dentry* dir, struct qstr *a, struct qstr *b){
	int i;

//	DEBUG(" yup, i'm here!\n");
	if(a->len != b->len) return 1;
	for(i = 0; i<a->len; i++)
		if(tolower(a->name[i]) != tolower(b->name[i])) return 1;
	return 0;	
}

static void
ftp_delete_dentry(struct dentry *dentry){
	DEBUG(" what!?!\n");
	if(dentry->d_inode){
		if(is_bad_inode(dentry->d_inode)) d_drop(dentry);
//		ftp_close_dentry(dentry);
	}
}

struct dentry_operations ftp_dentry_operations = {
	ftp_lookup_validate,
	ftp_hash_dentry,
	ftp_compare_dentry,
	ftp_delete_dentry
};

void
ftp_renew_times(struct dentry* dentry){
	for(;dentry!=dentry->d_parent; dentry = dentry->d_parent)
			dentry->d_time = jiffies;

}

static ssize_t
ftp_dir_read(struct file *f, char *buf, size_t count, loff_t *ppos){
	return -EISDIR;
}

static int
ftp_readdir(struct file *f, void *dirent, filldir_t filldir){
	struct dentry *entry,*dentry = f->f_dentry;
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct ftp_sb_info *info = (struct ftp_sb_info*)sb->u.generic_sbp;
	char buf[FTP_MAXPATHLEN],cmd[FTP_MAXLINE];
	int res,pos;
//	struct socket *data_sock;

	struct ftp_directory *dir;
	struct ftp_dirlist_node *file;


//	ftp_lock_server(info);
//	DEBUG(" starting to rock...\n");
	DEBUG(" reading %s, f_pos=%d\n", dentry->d_name.name, (int) f->f_pos);
	if(ftp_get_name(dentry, buf)<0){
		DEBUG("ftp_get_name failed!\n");
		goto error;
	}
//	for(res = 0; res<strlen(buf); res++)
//		if(buf[res] == ' '
//	DEBUG(" got name:%s\n", buf);
	res = 0;
	switch((unsigned int) f->f_pos){
		case 0:
				if(filldir(dirent, ".", 1, 0, inode->i_ino)<0) goto error;
				f->f_pos = 1;
		case 1:
				if(filldir(dirent, "..", 2, 1, dentry->d_parent->d_inode->i_ino)<0) goto error;
				f->f_pos = 2;
		default:
				ftp_lock_server(info);
				dir = ftp_cache_get(info, buf);
				ftp_unlock_server(info);
				if(!dir){
					DEBUG(" ftp_cache_get failed!\n");
					goto error;
				}

				pos = 2;
				for(file = dir->head; file != NULL; file = file->next){
					if(pos == f->f_pos){
						struct qstr qname;
						unsigned long ino;

						qname.name = file->entry.name;
						qname.len = strlen(qname.name);

						ino = find_inode_number(dentry, &qname);
						if(!ino) ino = ftp_invent_inos(1);

						if(filldir(dirent, qname.name, qname.len, f->f_pos, ino)>=0) f->f_pos++;

						
					}
					pos++;
				}
	}

	
out:
//	ftp_unlock_server(info);	
	return 0;
error:
//	ftp_unlock_server(info);
	return -1;
}
 

static int
ftp_dir_open(struct inode *dir, struct file *f){
	DEBUG(" wish you would die:)\n");
	return 0;
}
//------------------------------------------------------------------------

static struct dentry*
ftp_lookup(struct inode *dir, struct dentry *dentry){
	struct ftp_fattr fattr;
	struct inode *inode;
	
//	DEBUG(" wish I could help you...\n");
	DEBUG(" dname:%s\n", dentry->d_name.name);
	DEBUG(" i_mode:%u i_ino:%u\n", dir->i_mode, dir->i_ino);

	if(ftp_get_attr(dentry, &fattr, (struct ftp_sb_info*)dir->i_sb->u.generic_sbp) < 0){
		DEBUG(" not found!\n");
		return ERR_PTR(-ENOENT);
	}

	fattr.f_ino = ftp_invent_inos(1);
	inode = ftp_iget(dir->i_sb, &fattr);
	if(inode){
		dentry->d_op = &ftp_dentry_operations;
		d_add(dentry, inode);
		ftp_renew_times(dentry);
	}
	return NULL;
}


//------------------------------------------------------------------------




static struct file_operations ftp_dir_operations = {
	NULL,			// lseek
	ftp_dir_read,	// read
	NULL,			// write
	ftp_readdir,	// readdir
	NULL,			// poll
	NULL,			// ioctl
	NULL,			// mmap
	ftp_dir_open,	// open
	NULL,			// flush
	NULL,			
	NULL			// fsync
};

struct inode_operations ftp_dir_inode_operations = {
	&ftp_dir_operations,	//dir ops
	NULL,					//create
	ftp_lookup,				//lookup
	NULL,					//link
	NULL,					//unlink
	NULL,					//symlink
	NULL,					//mkdir
	NULL,					//rmdir
	NULL,					//mknod
	NULL,					//rename
	NULL,					//readlink
	NULL,					//followlink
	NULL,					//readpage
	NULL,					//writepage
	NULL,					//bmap
	NULL,					//truncate
	NULL,					//permission
	NULL,					//smap
	NULL,					//update page
	NULL					//revalidate
};
