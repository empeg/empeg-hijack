#define __NO_VERSION__ 

#include<linux/version.h>
#include<linux/config.h>
#include<linux/kernel.h>
#include<linux/module.h> 
#include<linux/fs.h>
#include<linux/malloc.h>
#include<linux/locks.h>
#include<linux/string.h>

#include<asm/uaccess.h>

#include"ftpfs.h"

extern int sock_send(struct socket*, const void*, int);
extern int sock_recv(struct socket*, unsigned char*, int, unsigned);

static char last_exec[FTP_MAXLINE] = "";
static unsigned long off = 0;


unsigned long
ftp_inet_addr(char *ip){
	unsigned long res = 0;
	int i,no=0,np=0;

	for(i=0;i<strlen(ip);i++){
		if(((ip[i]<'0')||(ip[i]>'9'))&&(ip[i]!='.')) return -1;
		if(ip[i]=='.') {
			if(++np>3) return -1;
			if((no<0)||(no>255)) return -1;
			res = (res >> 8) + (no << 24);
			no = 0;
		}else no = no*10 + ip[i] - '0';
	}
	if((no<0)||(no>255)) return -1;
	res = (res >> 8) + (no << 24);
	if(np!=3) return -1;

	return res;
}

ino_t
ftp_invent_inos(unsigned long n){
	static ino_t ino = 2;

	if(ino + 2*n < ino) ino = 2;
	ino += n;
	return ino;
}  

static void
ftp_init_dirent(struct ftp_sb_info *server, struct ftp_fattr *fattr)
{
	memset(fattr, 0, sizeof(*fattr));

	fattr->f_nlink = 1;
	fattr->f_uid = server->mnt.uid;
	fattr->f_gid = server->mnt.gid;
	fattr->f_blksize = 512;
}

static void
ftp_finish_dirent(struct ftp_sb_info *server, struct ftp_fattr *fattr)
{
	fattr->f_mode = server->mnt.file_mode;
//	if (fattr->attr & aDIR)
//	{
//		fattr->f_mode = server->mnt.dir_mode;
//		fattr->f_size = 512;
//	}
	/* Check the read-only flag */
//	if (fattr->attr & aRONLY)
//		fattr->f_mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
 
	fattr->f_blocks = 0;
	if ((fattr->f_blksize != 0) && (fattr->f_size != 0))
	{
		fattr->f_blocks =
		    (fattr->f_size - 1) / fattr->f_blksize + 1;
	}
	return;
}

void
ftp_init_root_dirent(struct ftp_sb_info *server, struct ftp_fattr *fattr)
{
	ftp_init_dirent(server, fattr);
//	fattr->attr = aDIR;

	
	fattr->f_ino = 2; /* traditional root inode number */
	fattr->f_mtime = CURRENT_TIME;
//	ftp_finish_dirent(server, fattr);
	fattr->f_mode = server->mnt.dir_mode;
	fattr->f_size = 512;
	fattr->f_blocks = 0;
}

int
ftp_get_name(struct dentry* d, char* name){
	int len = 0;
	struct dentry* p;

	for(p = d; p != p->d_parent; p = p->d_parent)
		len += p->d_name.len + 1;

	if(len>FTP_MAXPATHLEN) return -1;
	if(len == 0) {
		name[0] = '/';
		name[1] = 0;
		return 0;
	}
	name[len] = 0;
	for(p = d; p != p->d_parent; p = p->d_parent){
		len -= p->d_name.len;
		strncpy(&(name[len]), p->d_name.name, p->d_name.len);
		len--;
		name[len] = '/';
	}
	
	return 0;
}

int
ftp_get_substring(char *s, char *d,int n){
	int in_space = 1;
	int len = 0, i;

	for(i = 0;i<strlen(s);i++){
		if(in_space){
			if(s[i] != ' '){
				in_space = 0;
				len = 0;
				d[len++] = s[i];
			}
		}else{
			if(s[i] != ' ') d[len++] = s[i];
			else {
				in_space = 1;
				d[len] = 0;
				if((--n) == 0) return len;
				len = 0;
			}
		}
	}
	if((len>2)&&(n == 1)) {
		d[len - 2] = 0;
		return len -2;
	}
	return -1;
}

int
ftp_get_fname(char* s, char* d){
	int i, cnt;
	int in_space = 1;

	for(i = 0, cnt = 9; (i<strlen(s))&&(cnt>0); i++){
		if(in_space) {
			if(s[i] != ' '){
				in_space = 0;
				cnt--;
			}
		}else{
			if(s[i] == ' ') in_space = 1;
		}
	}
	if(i>=strlen(s)){
		DEBUG(" deep shit here!\n");
		d[0] = 0;
		return -1;
	}
//	DEBUG(" %s found:%u\n", s, i);
	strcpy(d, &(s[i-1]));
	d[strlen(d)-2] = 0;
	return 0;
}


///////////////////////////////////////////////////////////////////////////////
int
ftp_readline(struct socket* sock, char* buf, int len){
	int i = 0, out = 0;
	do{
		if(sock_recv(sock,&(buf[i]),1,0)){
			if(buf[i] == '\n') out = 1;
			i++;
		}else out = 1;
	}while((i<len)&&(out == 0));
	buf[i]=0;
	return i;
}

int 
ftp_get_response(struct ftp_sb_info* info){
	char buf[FTP_MAXLINE];
	int i,res = 0;
	
	do i = ftp_readline(info->ctrl_sock, buf, FTP_MAXLINE);
	while ((i>0)&&(buf[3] == '-'));
	

	if(i == 0){
		DEBUG(" ftp_readline failed!\n");
		return -1;
	}

//	DEBUG(" read: %s\n", buf);
	
	for(i=0;i<3;i++)
		if((buf[i]<'0')||(buf[i]>'9')){
			DEBUG(" bad response!\n");
			return -1;
		}else res = res*10 + buf[i] - '0';

//	DEBUG(" response: %u", res);
	return res;
}

int
ftp_execute(struct ftp_sb_info *info, char *command, int result){
	char buf[FTP_MAXLINE];


	if(info->data_sock)
		ftp_close_data(info);
	
	sprintf(buf, "%s\r\n",command);
	if(sock_send(info->ctrl_sock,buf , strlen(buf))<0){
		DEBUG(" send error!\n");
		return -1;
	}

	if(result){
		int rc = ftp_get_response(info);
		if(rc!=result){
			DEBUG(" command failed (%d)!\n", rc);
			return -rc;
		}
	}
	return 0;
}

int
ftp_connect(struct ftp_sb_info* info){
	char buf[FTP_MAXLINE];
	int rc;
	
	info->address.sin_family = AF_INET;
	
	if(info->ctrl_sock){
		sock_release(info->ctrl_sock);
		info->ctrl_sock = NULL;
	}

	if(sock_create(AF_INET, SOCK_STREAM, 0, &info->ctrl_sock)<0){
		DEBUG(" Create socket error!\n");
		return -1;
	}

	if(info->ctrl_sock->ops->connect(info->ctrl_sock, (struct sockaddr*)&info->address, sizeof(info->address), 0)<0){
		DEBUG(" Connect error!\n");
		return -1;
	}

	if(ftp_get_response(info)!=220){
		DEBUG(" server error!\n");
		return -1;
	}

	sprintf(buf,"USER %s", info->user);
	rc = ftp_execute(info, buf, 331);
	if (rc < 0 && rc != -230) {
		DEBUG(" user failed!\n");
		return -1;
	}

	if (rc != -230) {
		sprintf(buf,"PASS %s", info->pass);
		if(ftp_execute(info, buf, 230)<0){
			DEBUG(" login failed!\n");
			return -1;
		}
	}
	
	return 0;
}

void
ftp_disconnect(struct ftp_sb_info *info){
	ftp_close_data(info);
	
	if(info->ctrl_sock) sock_release(info->ctrl_sock);
//	if(info->data_sock) sock_release(info->data_sock);
}


int 
ftp_execute_open(struct ftp_sb_info* info, char* command, char* type, unsigned long offset, struct socket **data_sock){
	struct sockaddr_in data, ctrl;
	int len;
	char buf[FTP_MAXLINE];
	struct socket* ssock;
	
	memset(&data,0,sizeof(data));
	data.sin_addr.s_addr = INADDR_ANY;
	data.sin_port = 0;

	if((info->data_sock == 0) || (offset != off) || (strcmp(last_exec, command))){
		DEBUG(" :( reopening !\n");
		DEBUG(" off=%d, last_exec=%s\n", off, last_exec);

		if(info->data_sock) ftp_close_data(info);

		if(sock_create(AF_INET, SOCK_STREAM, 0, &ssock)<0){
			DEBUG(" create socket error!\n");
			return -1;
		}

		if(ssock->ops->bind(ssock,(struct sockaddr*)&data, sizeof(data))<0){
			DEBUG(" bind failed!\n");
			return -1;
		}

		if(ssock->ops->listen(ssock, 7)<0){
			DEBUG(" listen failed!\n");
			return -1;
		}

		len = sizeof(ctrl);
		if(info->ctrl_sock->ops->getname(info->ctrl_sock, (struct sockaddr*)&ctrl, &len,0)<0){
			DEBUG(" getsockname failed!\n");
			return -1;
		}

		len = sizeof(data);
		if(ssock->ops->getname(ssock, (struct sockaddr*)&data, &len,0)<0){
			DEBUG(" getsockname failed!\n");
			return -1;
		}

		data.sin_addr.s_addr = ctrl.sin_addr.s_addr;
		sprintf(buf, "PORT %u,%u,%u,%u,%u,%u",
				data.sin_addr.s_addr & 0xff,
				(data.sin_addr.s_addr >> 8) & 0xff,
				(data.sin_addr.s_addr >> 16) & 0xff,
				(data.sin_addr.s_addr >> 24) & 0xff,
				ntohs(data.sin_port) >> 8,
				ntohs(data.sin_port) & 0xff);

		if(ftp_execute(info, buf, 200)<0){
			DEBUG(" PORT command failed!\n");
			return -1;
		}

		sprintf(buf, "TYPE %s", type);
		if(ftp_execute(info, buf, 200)<0){
			DEBUG(" couldn't set transmission type...\n");
			return -1;
		}

		sprintf(buf, "REST %lu", offset);
		if(ftp_execute(info, buf, 350)<0){
			DEBUG(" couldn't set trasmission offset...\n");
			return -1;
		}

		if(ftp_execute(info, command, 150)<0){
			DEBUG(" command failed!\n");
			return -1;
		}

		if(sock_create(AF_INET, SOCK_STREAM, 0, data_sock)<0){
			DEBUG(" socket create error!\n");
			return -1;
		}

		(*data_sock)->type = ssock->type;

		if((*data_sock)->ops->dup((*data_sock), ssock)<0){
			DEBUG(" dup error!\n");
			sock_release((*data_sock));
			return -1;
		}

		if((*data_sock)->ops->accept(ssock, (*data_sock), 0)<0){
			DEBUG(" accept failed!\n");
			sock_release((*data_sock));
			return -1;
		}

		info->data_sock = *data_sock;
		sock_release(ssock);
		strcpy(last_exec, command);

		off = offset;
		

	}else{
//		DEBUG(" :) staying alive!\n");
		(*data_sock) = info->data_sock;		
	}

	DEBUG (" DONE\n");
	return 0;
}


int 
ftp_get_attr(struct dentry *dentry, struct ftp_fattr *fattr, struct ftp_sb_info* info){
	struct ftp_directory *dir;
	char buf[FTP_MAXLINE];
	struct ftp_dirlist_node *file;

	
//	ftp_lock_server(info);	
	
	ftp_get_name(dentry->d_parent, buf);
	dir = ftp_cache_get(info, buf);
	if(!dir){
		DEBUG(" ftp_cache_get failed!\n");
		goto error;
	}

	DEBUG(" got tha dir %s from cache!\n", buf);
	
	for(file = dir->head; file!= NULL; file = file->next)
		if(strcmp(dentry->d_name.name, file->entry.name) == 0) break;
	if(!file){
		DEBUG(" file not found in parent dir cache!\n");
		goto error;
	}
	DEBUG(" file size:%u\n", file->entry.size);

	fattr->f_mode = file->entry.mode;
	fattr->f_size = file->entry.size;
	fattr->f_blksize = file->entry.blocksize;
	fattr->f_blocks = file->entry.blocks;
	fattr->f_nlink= file->entry.nlink;
	fattr->f_mtime= CURRENT_TIME;
	fattr->f_uid = info->mnt.uid;
	fattr->f_gid = info->mnt.gid;
	
//	ftp_unlock_server(info);
	return 0;
error:
//	ftp_unlock_server(info);
	return -1;
}


int
ftp_loaddir(struct ftp_sb_info* info, char *name, struct ftp_directory *dir){
	struct socket *data_sock;
	char buf[FTP_MAXLINE], buf2[FTP_MAXLINE];
	char *b = buf2;
	struct ftp_dirlist_node *p;
	int own = 0;
	
	DEBUG(" reading %s\n", name);
	
	sprintf(buf,"CWD %s", name);
	if(ftp_execute(info, buf, 250) < 0){
		DEBUG(" CWD failed!\n");
		return -1;
	}
	if(ftp_execute_open(info, "LIST", "A", 0, &data_sock)<0){
		DEBUG(" LIST failed!\n");
		return -1;
	}
	while(ftp_readline(data_sock, buf, FTP_MAXLINE)>0){
//		DEBUG(" read:%s\n", buf);
		p = (struct ftp_dirlist_node*)kmalloc(sizeof(struct ftp_dirlist_node), GFP_KERNEL);
		if(!p){
			DEBUG(" malloc error!\n");
			goto out1;
		}
		memset(p, 0, sizeof(struct ftp_dirlist_node));
		ftp_get_fname(buf, buf2);
//		DEBUG(" file fname:%s\n",buf2);
		
//		ftp_get_substring(buf, buf2, 9);

		p->entry.name = (char*)kmalloc(strlen(buf2)+1, GFP_KERNEL);
		if(!p->entry.name){
			DEBUG(" malloc error!\n");
			goto out2;
		}
		strcpy(p->entry.name, buf2);

		ftp_get_substring(buf, buf2, 5);
		b = buf2;
		p->entry.size = simple_strtoul(b, &b, 0);
//		DEBUG(" file size:%s %u\n", buf2, p->entry.size);
		p->entry.blocksize = 1024;
		p->entry.blocks = (p->entry.size + 1023) >> 9;
		ftp_get_substring(buf, buf2, 2);
		b = buf2;
		p->entry.nlink = simple_strtoul(b, &b, 0);


		ftp_get_substring(buf,buf2,3);
		if(strcmp(buf2,info->user) == 0) own = 1;
		ftp_get_substring(buf,buf2,1);
		if(buf2[0] == 'd') p->entry.mode |= S_IFDIR;
		else if(buf2[0] == 'l') p->entry.mode |= S_IFLNK;
				else p->entry.mode |= S_IFREG;

		own = 1; //pt. fucking SERVU
		if(own){
			if(buf2[1] == 'r') p->entry.mode |= S_IRUSR;
			if(buf2[2] == 'w') p->entry.mode |= S_IWUSR;
			if(buf2[3] == 'x') p->entry.mode |= S_IXUSR;
			if(buf2[4] == 'r') p->entry.mode |= S_IRGRP;
			if(buf2[5] == 'w') p->entry.mode |= S_IWGRP;
			if(buf2[6] == 'x') p->entry.mode |= S_IXGRP;
			
		}else{
			if(buf2[7] == 'r') p->entry.mode |= S_IRUSR | S_IRGRP;
			if(buf2[8] == 'w') p->entry.mode |= S_IWUSR | S_IWGRP;
			if(buf2[9] == 'x') p->entry.mode |= S_IXUSR | S_IXGRP;
		}
		if(buf2[7] == 'r') p->entry.mode |= S_IROTH;
		if(buf2[8] == 'w') p->entry.mode |= S_IWOTH;
		if(buf2[9] == 'x') p->entry.mode |= S_IXOTH;

		p->entry.mode |= S_IROTH;								///
		if(p->entry.mode & S_IFDIR) p->entry.mode |= S_IXOTH;   /// Same fucking SERVU
		
		p->prev = NULL;
		p->next = dir->head;
		dir->head = p;
	}
	ftp_close_data(info);
	return 0;
	
out2:
	kfree(p);
out1:
	ftp_close_data(info);
	return -1;
}

int 
ftp_read(struct dentry* dentry, unsigned long offset, unsigned long count, char* buffer){
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct ftp_sb_info *info = (struct ftp_sb_info*)sb->u.generic_sbp;
	char buf[FTP_MAXLINE], buf2[FTP_MAXLINE];
	int res;
	struct socket *data_sock = NULL;
	
	ftp_lock_server(info);
	
	ftp_get_name(dentry, buf);
	DEBUG(" %s, off %u, %u bytes\n", buf, offset, count);

	sprintf(buf2, "RETR %s", buf);
	if(ftp_execute_open(info, buf2, "I", offset, &data_sock)<0){
		DEBUG(" couldn't open data connection for %s!\n", buf2);
		ftp_unlock_server(info);
		return -1;
	}
	off = offset;
	
	res = sock_recv(data_sock, buffer, count, 0);
	DEBUG(" received %u bytes\n", res);
	off += res;

	ftp_unlock_server(info);
	return res;
}

int
ftp_close_data(struct ftp_sb_info* info){
	if(info->data_sock){
		sock_release(info->data_sock);
		info->data_sock = NULL;
		return ftp_get_response(info);
	}
	return 0;
}

inline void
ftp_lock_server(struct ftp_sb_info* info){
	DEBUG(" LOCKING server (pid=%u)...\n", current->pid);
	down(&(info->sem));
	DEBUG(" OK\n");
}

inline void
ftp_unlock_server(struct ftp_sb_info* info){
	DEBUG(" UNLOCKING server (pid=%u)...\n",current->pid);
	up(&(info->sem));
	DEBUG(" OK\n");
}
