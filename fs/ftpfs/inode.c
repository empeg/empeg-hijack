#include<linux/version.h> 
#include<linux/config.h>
#include<linux/kernel.h>
#include<linux/fs.h>
#include<linux/malloc.h>
#include<linux/locks.h>
#include<linux/string.h> 

#include<asm/uaccess.h>

#include "ftpfs.h"  


//----------------------------------------------------------------------

extern struct inode_operations ftp_dir_inode_operations;
extern struct inode_operations ftp_file_inode_operations;

//----------------------------------------------------------------------
static void
ftp_set_inode_attr(struct inode *inode, struct ftp_fattr *fattr)
{
	inode->i_mode	= fattr->f_mode; 
	inode->i_nlink	= fattr->f_nlink;
	inode->i_uid	= fattr->f_uid;
	inode->i_gid	= fattr->f_gid;
	inode->i_rdev	= fattr->f_rdev;
	inode->i_mtime = fattr->f_mtime;
	inode->i_atime = fattr->f_atime;
	inode->i_ctime	= fattr->f_ctime;
	inode->i_blksize= fattr->f_blksize;
	inode->i_blocks = fattr->f_blocks;
	inode->i_size  = fattr->f_size;
}


struct inode*
ftp_iget(struct super_block* sb, struct ftp_fattr *fattr){
	struct inode *res;

//	DEBUG(" Hangin' in here!\n");

	res = get_empty_inode();
	res->i_sb = sb;
	res->i_dev = sb->s_dev;
	res->i_ino = fattr->f_ino;
	res->u.generic_ip = NULL;
	ftp_set_inode_attr(res,fattr);
	if(S_ISDIR(res->i_mode)){
//		DEBUG(" yup, it's a dir!\n");
		res->i_op = &ftp_dir_inode_operations;
	}else res->i_op = &ftp_file_inode_operations;
	
	insert_inode_hash(res);
	
	return res;
}

static void
ftp_put_super(struct super_block *sb){
	struct ftp_sb_info *info=(struct ftp_sb_info*)sb->u.generic_sbp;

	ftp_disconnect(info);
	ftp_cache_empty();	
	kfree(info);
	DEBUG(" Super Block discarded!\n");
}

static void
ftp_read_inode(struct inode *inode){
	DEBUG(" This piece of shit does nada!!\n"); 
	return;
}


static int
ftp_statfs(struct super_block *sb, struct statfs *buf, int bufsize){
	struct statfs attr;
	DEBUG(" Just got in here:)\n");

	memset(&attr, 0, sizeof(attr));

	attr.f_type = FTP_SUPER_MAGIC;
	attr.f_bsize = 1024;
	attr.f_blocks = 0;
	attr.f_namelen = FTP_MAXPATHLEN;
	attr.f_files = -1;

	return copy_to_user(buf,&attr, bufsize) ? -EFAULT : 0;
}

static void
ftp_put_inode(struct inode* inode){
//	DEBUG(" doing shit...\n");
	if (inode->i_count == 1)
		inode->i_nlink = 0;
}

static void
ftp_delete_inode(struct inode* inode){
//	DEBUG(" doing shit...\n");
	clear_inode(inode);
}

static struct super_operations ftp_sops = {
	ftp_read_inode,	//read inode
	NULL,			//write inode
	ftp_put_inode,	//put inode
	ftp_delete_inode,//delete inode
	NULL,			//notify change
	ftp_put_super,	//put superblock
	NULL,			//write superblock
	ftp_statfs,		//stat fs
	NULL			//remount fs
};


int 
ftp_parse_options(struct ftp_sb_info *info, void* opts){
	char *p,*q;
	int i, no_opts = 0;

	info->address.sin_addr.s_addr = ftp_inet_addr("127.0.0.1");
	info->address.sin_port =  htons(21);
	strcpy(info->user,"anonymous");
	strcpy(info->pass,"ftpfs@localhost");

	if(!opts) return -1;

	p = strtok(opts,",");
	for(; p ; p = strtok(NULL,",")){
//		if(p) DEBUG(" Token: %s\n",p);

		if(strncmp(p, "ip=", 3)==0){
			if((info->address.sin_addr.s_addr = ftp_inet_addr(p+3)) == -1){
				DEBUG(" Bad ip option! Defaulting to localhost...\n");
				info->address.sin_addr.s_addr = ftp_inet_addr("127.0.0.1");	
			}
			no_opts++;
		}else
		if(strncmp(p, "port=", 5)==0){
			q = p+5;
			i = simple_strtoul(q,&q,0);
			if((i<0)||(i>0xffff)) {
				DEBUG(" Invalid port option! Defaulting to 21...\n");
				info->address.sin_port = htons(21); 
			}else info->address.sin_port = htons(i);
			no_opts++;
		}else
		if(strncmp(p, "user=", 5)==0){
			strcpy(info->user, p+5);
			no_opts++;
		}else
		if(strncmp(p, "pass=", 5)==0){
			strcpy(info->pass, p+5);
			no_opts++;
		}
	}
	return no_opts;
} 

struct super_block*
ftp_read_super(struct super_block* sb, void *opts, int silent){
	struct ftp_sb_info *info;
	struct ftp_fattr root;
	struct inode *root_inode;

	
	lock_super(sb);

	info=(struct ftp_sb_info *)kmalloc(sizeof(struct ftp_sb_info),GFP_KERNEL);
	if(!info){
		DEBUG(" Not enough kmem to allocate info!!\n");
		goto out;
	}

	ftp_cache_init();
	
	memset(info,0,sizeof(struct ftp_sb_info));
	
	sb->u.generic_sbp = info;
	sb->s_blocksize = 1024;
	sb->s_blocksize_bits = 10;
	sb->s_magic = FTP_SUPER_MAGIC;
	sb->s_op = &ftp_sops;
	sb->s_flags |= MS_RDONLY;


	info->mnt.version = FTP_VERSION;
	info->mnt.file_mode = (S_IRWXU | S_IRGRP | S_IROTH | S_IFREG);
	info->mnt.dir_mode = (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH | S_IFDIR);
	info->mnt.uid = current->uid;
	info->mnt.gid = current->gid;
	info->ctrl_sock = NULL;
	info->data_sock = NULL;
	info->sem = MUTEX;
	
	DEBUG(" uid:%d gid:%d\n",current->uid,current->gid);

	if(ftp_parse_options(info, opts)<0){
		DEBUG(" Wrong options!\n");
		goto out_no_opts;
	}

	DEBUG(" Mounting %u.%u.%u.%u:%u, user %s, password %s\n",
			info->address.sin_addr.s_addr & 0xff,
			(info->address.sin_addr.s_addr >> 8) & 0xff,
			(info->address.sin_addr.s_addr >> 16) & 0xff,
			(info->address.sin_addr.s_addr >> 24) & 0xff,
			ntohs(info->address.sin_port), info->user, info->pass);

	if(ftp_connect(info)<0){
		DEBUG(" Shit!\n");
		goto out_no_opts;
	}

	ftp_init_root_dirent(info, &root);
	root_inode = ftp_iget(sb, &root);
	if(!root_inode) goto out_no_root;
	sb->s_root = d_alloc_root(root_inode, NULL);
	if(!sb->s_root) goto out_no_root;
	
	unlock_super(sb);
	DEBUG(" Mount succeded!\n");
	return sb; 

out_no_root:
	iput(root_inode);
out_no_opts:
	kfree(info);
out:
	unlock_super(sb);
	DEBUG(" Mount failed!!\n");
	return NULL;
}


static struct file_system_type ftpfs_fs_type = {"ftpfs",0,ftp_read_super,NULL};

int init_ftpfs_fs(void)
{
	return register_filesystem(&ftpfs_fs_type);
}

