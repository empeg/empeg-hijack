#define hFTPFS_DEBUG

#ifndef _FTPFS_H_
#define _FTPFS_H_

#ifdef FTPFS_DEBUG
#define DEBUG(x...) printk("***FTPFS*** " KERN_NOTICE  __FUNCTION__ ":" ## x )
#else
#define DEBUG(x...) ;
#endif

#include<linux/types.h>

#define FTP_SUPER_MAGIC 0xFADE
#define FTP_MAXPATHLEN	256
#define FTP_MAXLINE		256
#define FTP_VERSION		0x01
#define FTP_CACHEHASH	10
#define FTP_CACHELEN	10
#define FTP_MAX_AGE		5*HZ


#define aRONLY	(1L<<0)
#define aHIDDEN	(1L<<1)
#define aSYSTEM	(1L<<2)
#define aVOLID	(1L<<3)
#define aDIR	(1L<<4)
#define aARCH	(1L<<5)


struct ftp_dir_entry{
	umode_t			mode;
	char *			name;
	off_t			size;
	nlink_t			nlink;
	unsigned long	blocksize;
	unsigned long	blocks;
};

struct ftp_dirlist_node{
	struct ftp_dirlist_node *prev, *next;
	struct ftp_dir_entry entry;
};

struct ftp_directory{
	struct ftp_dirlist_node *head;
	int	valid;
	time_t time;
	char *name;
};


struct ftp_hashlist_node{
	struct ftp_hashlist_node *prev, *next;
	struct ftp_directory directory;
};

struct ftp_dir_cache{
	struct ftp_hashlist_node* hash[FTP_CACHEHASH];
	unsigned len[FTP_CACHEHASH];
};

struct ftp_mount_data{
	int version;
//	__kernel_uid_t	mounted_uid;
	__kernel_uid_t	uid;
	__kernel_gid_t	gid;
	__kernel_mode_t	file_mode;
	__kernel_mode_t	dir_mode;
};

struct ftp_sb_info{
	struct sockaddr_in address;
	struct ftp_mount_data mnt;
	char user[64];
	char pass[64];
	struct semaphore sem;
	struct socket *ctrl_sock;
	struct socket *data_sock;	
};

struct ftp_fattr{
//	__u16 attr;

	unsigned long	f_ino;
	umode_t			f_mode;
	nlink_t			f_nlink;
	uid_t			f_uid;
	gid_t			f_gid;
	kdev_t			f_rdev;
	off_t			f_size;
	time_t			f_atime;
	time_t			f_mtime;
	time_t			f_ctime;
	unsigned long	f_blksize;
	unsigned long	f_blocks;
};


struct inode* ftp_iget(struct super_block*, struct ftp_fattr*);
void ftp_init_root_dirent(struct ftp_sb_info *, struct ftp_fattr *);
ino_t ftp_invent_inos(unsigned long);
unsigned long ftp_inet_addr(char*);
//int sock_send(struct socket*,const void*,int);
//int sock_recv(struct socket*,unsigned char*,int,unsigned);
int ftp_connect(struct ftp_sb_info*);
void ftp_disconnect(struct ftp_sb_info*);
int ftp_execute(struct ftp_sb_info*, char*, int);
int ftp_get_response(struct ftp_sb_info*);
int ftp_readline(struct socket*, char*, int);
int ftp_execute_open(struct ftp_sb_info*, char*, char*, unsigned long, struct socket**);
int ftp_get_name(struct dentry*, char*);
int ftp_get_substring(char*, char*, int);
int ftp_get_attr(struct dentry *, struct ftp_fattr *, struct ftp_sb_info*);
int ftp_loaddir(struct ftp_sb_info*, char*, struct ftp_directory*);
struct ftp_directory* ftp_cache_get(struct ftp_sb_info *, char* );
int ftp_read(struct dentry*, unsigned long, unsigned long, char*);
int ftp_close_data(struct ftp_sb_info*);
inline void ftp_lock_server(struct ftp_sb_info*);
inline void ftp_unlock_server(struct ftp_sb_info*);
int ftp_cache_empty(void);
int ftp_cache_init(void);

#endif
