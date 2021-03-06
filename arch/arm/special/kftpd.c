// kftpd / khttpd / ktelnetd, by Mark Lord
//
// This version can UPLOAD and DOWNLOAD files, directories, serve playlists, remote commands, ..

#define __KERNEL_SYSCALLS__

#include <linux/config.h>
#include <linux/proc_fs.h>
#include <linux/unistd.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/socket.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/init.h>
#include <linux/poll.h>

#include <asm/uaccess.h>

#include <linux/inet.h>
#include <net/ip.h>
#include <net/sock.h>
#include <net/rarp.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <net/scm.h>

#include <asm/arch/hijack.h>

#define KHTTPD		"khttpd"
#define KFTPD		"kftpd"
#define KTELNETD	"ktelnetd"

extern int hijack_silent;
extern int get_number (unsigned char **src, int *target, unsigned int base, const char *nextchars);	// hijack.c
extern void input_append_code(void *dev, unsigned long button);						// hijack.c
extern int get_button_code (unsigned char **s_p, unsigned int *button, int eol_okay, int raw, const char *nextchars); // hijack.c
extern int hijack_reboot;
extern void hijack_set_voladj_parms(void);
extern int hijack_get_set_option (unsigned char **s_p);
extern int remount_drives (int writeable);
extern void hijack_serial_rx_insert (const char *buf, int size, int port);	// drivers/char/serial_sa1100.c
extern int hijack_glob_match (const char *n, const char *p);
extern tm_t *hijack_convert_time(time_t, tm_t *);	// from arch/arm/special/notify.c
extern void sys_exit(int);
extern char hijack_khttpd_style[];			// from arch/arm/special/hijack.c
extern char hijack_khttpd_root_index[];			// from arch/arm/special/hijack.c
extern const char hijack_vXXX_by_Mark_Lord[];		// from arch/arm/special/hijack.c
extern int strxcmp (const char *str, const char *pattern, int partial);	// hijack.c
extern void show_message (const char *message, unsigned long time);	// hijack.c
extern void printline (const char *msg, char *s);	// from arch/arm/special/hijack.c
extern int hijack_khttpd_port;				// from arch/arm/special/hijack.c
extern int hijack_khttpd_verbose;			// from arch/arm/special/hijack.c
extern int hijack_ktelnetd_port;			// from arch/arm/special/hijack.c
extern int hijack_khttpd_new_fid_dirs;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_control_port;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_data_port;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_verbose;			// from arch/arm/special/hijack.c
extern int hijack_rootdir_dotdot;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_show_dotfiles;			// from arch/arm/special/hijack.c
extern int hijack_khttpd_show_dotfiles;			// from arch/arm/special/hijack.c
extern int hijack_max_connections;			// from arch/arm/special/hijack.c
extern char hijack_kftpd_password[];			// from arch/arm/special/hijack.c
extern char hijack_khttpd_basic[];			// from arch/arm/special/hijack.c
extern char hijack_khttpd_full[];			// from arch/arm/special/hijack.c
extern struct semaphore hijack_kxxxd_startup_sem;	// from arch/arm/special/hijack.c
extern int menuexec_daemon(void *);     // Hijack
extern int sys_rmdir(const char *path); // fs/namei.c
extern int sys_readlink(const char *path, char *buf, int bufsiz);
extern int sys_chmod(const char *path, mode_t mode); // fs/open.c
extern int sys_mkdir(const char *path, int mode); // fs/namei.c
extern int sys_unlink(const char *path);
extern int sys_rename(const char * oldname, const char * newname);
extern int sys_newstat(char *, struct stat *);
extern int sys_newfstat(int, struct stat *);
extern int sys_fsync(int);
extern int sys_sync(void);
extern pid_t kernel_thread(int (*fn)(void *), void *arg, unsigned long flags);
extern int sys_wait4 (pid_t pid,unsigned int * stat_addr, int options, struct rusage * ru);

#define INET_ADDRSTRLEN		16

typedef enum {
	kftpd    = 0,
	khttpd   = 1,
	ktelnetd = 2,
} protocol_t;

// This data structure is allocated as a full page to prevent memory fragmentation:
typedef struct server_parms_s {
	char			*servername;
	struct socket		*clientsock;
	struct socket		*servsock;
	struct socket		*datasock;
	struct sockaddr_in	clientaddr;
	enum {nolist, html, m3u, xml} generate_playlist;
	char			verbose;		// bool
	char			protocol;		// protocol_t
	char			have_portaddr;		// bool
	char			icy_metadata;		// bool
	char			streaming;		// bool
	char			need_password;		// bool, FTP only
	char			rename_pending;		// bool
	char			nocache;		// bool
	char			show_dotfiles;		// bool
	char			nodata;			// bool
	char			method_head;		// bool
	char			running_playlist;	// bool
	char			auth;			// khttpd_auth_t
	char			is_mozilla;		// HTTP only
	unsigned short		data_port;
	off_t			start_offset;		// starting offset for next FTP/HTTP file transfer
	off_t			end_offset;		// for current HTTP file read
	unsigned int		umask;
	unsigned int		offset;			// for HTTP "OFFSET=nnnn" value
	unsigned int		count;			// for HTTP "COUNT=nnnn" value
	struct sockaddr_in	portaddr;
	char			clientip[INET_ADDRSTRLEN];
	char			user_passwd[24];	// khttpd
	char			hostname[48];		// serverip, or "Host:" field from HTTP header
	char			style[128];		// path for stylesheet to embed into xml output
	unsigned char		cwd[1000];
	unsigned char		buf[1024];
	unsigned char		tmp2[768];
	unsigned char		tmp3[768];
} server_parms_t;

static const char *
inet_ntop2 (struct sockaddr_in *addr, char *ipaddr)
{
	char *s;

	if (addr->sin_family != AF_INET)
		return NULL;
	s = (char *)&addr->sin_addr.s_addr;
	sprintf(ipaddr, "%u.%u.%u.%u", s[0], s[1], s[2], s[3]);
	return ipaddr;
}

// This  function  converts  the  character string src
// into a network address structure in the af address family,
// then copies the network address structure to dst.
//
static int
inet_pton (int af, unsigned char **src, void *dst)
{
	unsigned char	*d = dst;
	int		i;

	if (af != AF_INET)
		return -EAFNOSUPPORT;
	for (i = 3; i >= 0; --i) {
		unsigned int val;
		if (!get_number(src, &val, 10, ".,"))
			return 0;
		++*src;	// skip over '.'
		*d++ = val;
	}
	return 1;	// success
}

static int
extract_portaddr (struct sockaddr_in *addr, unsigned char *s)
{
	memset(addr, 0, sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	if (inet_pton(AF_INET, &s, &addr->sin_addr) > 0) {
		unsigned int port1, port2;
		if (get_number(&s, &port1, 10, ",") && *s++ && get_number(&s, &port2, 10, NULL)) {
			if (port1 <= 255 && port2 <= 255) {
				addr->sin_port = htons((port1 << 8) | port2);
				return 0;	// success
			}
		}
	}
	return -1;	// failure
}

// Adapted from various examples in the kernel
static int
ksock_rw (struct socket *sock, const char *buf, int buf_size, int minimum)
{
	int	flags = 0, bytecount = 0, sending = 0, retries = 9;

	if (minimum < 0) {
		minimum = buf_size;
		sending = 1;
		flags = MSG_DONTWAIT;	// asynchronous send
	}
	do {
		int		rc, len = buf_size - bytecount;
		struct msghdr	msg;
		struct iovec	iov;

		memset(&msg, 0, sizeof(msg));
		iov.iov_base	= (char *)&buf[bytecount];
		iov.iov_len	= len;
		msg.msg_iov	= &iov;
		msg.msg_iovlen	= 1;
		msg.msg_flags	= flags;	// asynchronous send
		if (sending)
			rc = sock_sendmsg(sock, &msg, len);
		else
			rc = sock_recvmsg(sock, &msg, len, 0);
		switch (rc) {
			case 0:
				if (!sending)
					return bytecount;
				break;
			case -EAGAIN:
				if (sending) {
					flags = 0;	// use synchronous send instead
					rc = 0;
				}
				break;
			case -ENOMEM:
			case -ENOBUFS:
				if (!hijack_silent)
					printk("ksock_rw(): low memory\n");
				if (retries--) {
					current->state = TASK_INTERRUPTIBLE;
					schedule_timeout(HZ);
					rc = 0;
				}
				break;
			case -EPIPE:
				if (bytecount)
					return bytecount;
			case -ECONNRESET:
				return rc;
			default:
				if (rc < 0) {
					if (!hijack_silent)
						printk("ksock_rw(%s): error: %d\n", sending ? "send" : "recv", rc);
					return rc;
				}
		}
		bytecount += rc;
	} while (bytecount < minimum);
	return bytecount;
}

typedef struct response_s {
	int		rcode;
	const char	*text;
} response_t;

static response_t response_table[] = {
	{150,	" Opening data connection"},
	{200,	" Okay"},
	{202,	" Okay"},
	{214,	"-The following commands are recognized (* =>'s unimplemented)\r\n"
		"   USER    PORT    STOR    NLST    MKD     CDUP    PASS    ABOR*\r\n"
		"   SITE    TYPE*   DELE    SYST*   RMD     STRU*   CWD     MODE*\r\n"
		"   HELP    PWD     QUIT    RETR    LIST    NOOP    XMKD    XRMD\r\n"
		"   REST    SIZE\r\n"
		"214 Okay"},
	{216,	"-The following SITE commands are recognized\r\n"
		"   BUTTON  CHMOD   EXEC    HELP    POPUP   REBOOT  RO      RW\r\n"
		"216 Okay"},
	{215,	" UNIX Type: L8"},
	{220,	" Connected"},
	{221,	" Happy Fishing"},
	{226,	" Okay"},
	{230,	" Login okay"},
	{250,	" Okay"},
	{257,	" Okay"},
	{331,	" Password required"},
	{350,	" Next command required"},
	{425,	" Connection error"},
	{426,	" Connection failed"},
	{431,	" No such directory"},
	{451,	" Internal error"},
	{500,	" Bad command"},
	{501,	" Bad syntax"},
	{502,	" Not implemented"},
	{503,	" Bad command sequence"},
	{530,	" Login incorrect"},
	{541,	" Remote command failed"},
	{550,	" Failed"},
	{553,	" Invalid action"},
	{0,	NULL} // End-Of-Table Marker
	};

static int
kftpd_send_response2 (server_parms_t *parms, int rcode, const char *text, const char *extra)
{
	char		buf[512];
	int		len, rc;

	if (parms->verbose && !hijack_silent)
		printk(KFTPD": %d%s%s\n", rcode, text, extra);
	len = sprintf(buf, "%d%s%s\r\n", rcode, text, extra);
	if ((rc = ksock_rw(parms->clientsock, buf, len, -1)) != len) {
		if (!hijack_silent)
			printk(KFTPD": ksock_rw(response) failed, rc=%d\n", rc);
		return -1;
	}
	return 0;
}

static int
kftpd_send_response (server_parms_t *parms, int rcode)
{
	response_t	*r = response_table;

	while (r->rcode && r->rcode != rcode)
		++r;
	return kftpd_send_response2(parms, rcode, r->text, ".");
}

static int
kftpd_dir_response (server_parms_t *parms, int rcode, const char *dir, char *suffix)
{
	char	buf[300];
	int	rc, len;

	if (suffix)
		len = sprintf(buf, "%d \"%s\" %s\r\n", rcode, dir, suffix);
	else
		len = sprintf(buf, "%d \"%s\"\r\n", rcode, dir);
	if (parms->verbose && !hijack_silent)
		printk(KFTPD": %s", buf);
	if ((rc = ksock_rw(parms->clientsock, buf, len, -1)) != len) {
		if (!hijack_silent)
			printk(KFTPD": ksock_rw(dir_response) failed, rc=%d\n", rc);
		return 1;
	}
	return 0;
}

static int
set_sockopt (server_parms_t *parms, struct socket *sock, int protocol, int option, int off_on)
{
	int	rc;

	rc = sock_setsockopt(sock, protocol, option, (char *)&off_on, sizeof(off_on));
	if (rc && !hijack_silent)
		printk("%s: setsockopt(%d,%d) failed, rc=%d\n", parms->servername, protocol, option, rc);
	return rc;
}

static int
make_socket (server_parms_t *parms, struct socket **sockp, int port)
{
	int			rc;
	struct sockaddr_in	addr;
	struct socket		*sock;

	*sockp = NULL;
	if ((rc = sock_create(AF_INET, SOCK_STREAM, 0, &sock))) {
		if (!hijack_silent)
			printk("%s: sock_create() failed, rc=%d\n", parms->servername, rc);
	} else if ((rc = set_sockopt(parms, sock, SOL_SOCKET, SO_REUSEADDR, 1))) {
		sock_release(sock);
	} else {
		memset(&addr, 0, sizeof(struct sockaddr_in));
		addr.sin_family	     = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port        = htons(port);
		rc = sock->ops->bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
		if (rc) {
			if (!hijack_silent)
				printk("%s: bind(port=%d) failed: %d\n", parms->servername, port, rc);
			sock_release(sock);
		} else {
			*sockp = sock;
		}
	}
	return rc;
}

static int
open_datasock (server_parms_t *parms)
{
	int		flags = 0;
	unsigned int	response = 0;

	if (parms->protocol == khttpd) {
		parms->datasock = parms->clientsock;
	} else if (!parms->have_portaddr) {
		response = 425;
	} else {
		parms->have_portaddr = 0;	// for next time
		if (make_socket(parms, &parms->datasock, hijack_kftpd_data_port)) {
			response = 425;
		} else {
			if (kftpd_send_response(parms, 150)) {
				response = 451;	// this obviously will never get sent..
			} else if (parms->datasock->ops->connect(parms->datasock,
					(struct sockaddr *)&parms->portaddr, sizeof(struct sockaddr_in), flags)) {
				response = 426;
			} else {
				(void) set_sockopt(parms, parms->datasock, SOL_TCP, TCP_NODELAY, 1); // don't care
			}
			if (response) {
				sock_release(parms->datasock);
				parms->datasock = NULL;
			}
		}
	}
	return response;
}

static char *
format_time (tm_t *tm, int current_year, char *buf)
{
	int t, y;
	extern const char *hijack_months[12];
	const char *s = hijack_months[tm->tm_mon];

	*buf++ = ' ';
	*buf++ = s[0];
	*buf++ = s[1];
	*buf++ = s[2];
	*buf++ = ' ';
	t = tm->tm_mday;
	*buf++ = (t / 10) ? '0' + (t / 10) : ' ';
	*buf++ = '0' + (t % 10);
	*buf++ = ' ';
	y = tm->tm_year;
	if (y == current_year) {
		t = tm->tm_hour;
		*buf++ = '0' + (t / 10);
		*buf++ = '0' + (t % 10);
		*buf++ = ':';
		t = tm->tm_min;
		*buf++ = '0' + (t / 10);
		*buf++ = '0' + (t % 10);
	} else {
		*buf++ = ' ';
		*buf++ = '0' + y / 1000; y = y % 1000;
		*buf++ = '0' + y /  100; y = y %  100;
		*buf++ = '0' + y /   10; y = y %   10;
		*buf++ = '0' + y;
	}
	*buf = '\0';
	return buf;
}

#define MODE_XBIT(c,x,s)	((x) ? ((s) ? ((c)|0x20) : 'x') : ((s) ? (c) : '-'));

static char *
append_string (char *b, const char *s, int quoted)
{
	while (*s) {
		if (quoted && (*s == '\\' || *s == '"'))
			*b++ = '\\';
		*b++ = *s++;
	}
	return b;
}

typedef struct filldir_parms_s {
	unsigned short		current_year;	// current calendar year (YYYY), according to the Empeg
	unsigned short		full_listing;	// 0 == names only, 1 == "ls -l"
	unsigned char		use_http;	// bool
	unsigned char		show_dotfiles;	// bool
	unsigned short		filecount;	// 0 == end of directory
	unsigned long		blockcount;	// for "total xxxx" line at end of kftpd dir listing
	struct super_block	*sb;		// superblock of filesystem, needed for inode lookups
	char			*pattern;	// for filename globbing (pattern matching for mget/mput/list)
	char			*name;		// points into path[]
	unsigned int		buf_size;	// size (bytes) of buf[]
	unsigned int		buf_used;	// number of bytes used buf[]
	char			*buf;		// allocated buffer for formatting partial dir listings
	unsigned int		nam_size;	// size (bytes) of nam[]
	unsigned int		nam_used;	// number of bytes used in nam[]
	char			*nam;		// allocated buffer for names from filldir()
	int			path_len;	// length of (non-zero terminated) base path in path[]
	char			path[768];	// full dir prefix, plus current name appended for dentry lookups
} filldir_parms_t;

// Callback routine for readdir().
// This gets called repeatedly until we return non-zero (nam[] buffer full).
//
static int
filldir (void *data, const char *name, int namelen, off_t offset, ino_t ino)
{
	filldir_parms_t *p = data;
	char		*n, *zname;
	unsigned int	len;

	++p->filecount;
	if (name[0] == '.' && (!p->pattern || p->pattern[0] != '.')) {
		if (!p->show_dotfiles)
			return 0;
		if (namelen == 2 && name[1] == '.') {
			if (!hijack_rootdir_dotdot && p->path_len == 1 && p->path[0] == '/')
				return 0;	// hide '..' in rootdir
		}
	}
	// determine next aligned location in p->nam buffer:
	len = (p->nam_used + 3) & ~3;
	if ((len + namelen + (sizeof(ino) + 1)) > p->nam_size)
		return -EAGAIN;			// buffer full; take a breather
	n = p->nam + len;
	len += (sizeof(ino) + 1) + namelen;
	// copy over the data first, to create a zero-terminated copy of "name"
	*((ino_t *)n)++ = ino;
	zname = n;
	while (namelen--)
		*n++ = *name++;
	*n = '\0';
	// now we can do pattern matching on the copied name:
	if (!p->pattern || hijack_glob_match(zname, p->pattern))
		p->nam_used = len;	// accept this entry; otherwise it gets overwritten next time thru
	return 0;			// continue reading directory entries
}

static int
classify_path (char *path)
{
	struct stat st;

	if (0 > sys_newstat(path, &st))	// in theory, this "follows" symlinks for us
		return 0;		// does not exist
	if ((st.st_mode & S_IFMT) != S_IFDIR)
		return 1;		// non-directory
	return 2;			// directory
}

static const char dirlist_html_trailer[] = "</pre><hr>\r\n<a href=\"/?FID=101&EXT=.%s\"><font size=-1>[Click here for playlists]</font></a><br>\r\n<font size=-2>%s</font></body></html>\r\n";
#define DIRLIST_TRAILER_MAX (sizeof(dirlist_html_trailer) + 1 + 25) // 25 is for version string

static int
send_dirlist_buf (server_parms_t *parms, filldir_parms_t *p, int send_trailer)
{
	int sent;

	if (p->full_listing && send_trailer) {
		if (parms->protocol) {
			const char *ext = "htm";
			if (1 == classify_path(hijack_khttpd_style))
				ext = "xml";
			p->buf_used += sprintf(p->buf + p->buf_used, dirlist_html_trailer, ext, hijack_vXXX_by_Mark_Lord);
		} else {
			p->buf_used += sprintf(p->buf + p->buf_used, "total %lu\r\n", p->blockcount);
		}
	}
	sent = ksock_rw(parms->datasock, p->buf, p->buf_used, -1);
	if (sent != p->buf_used) {
		if (parms->verbose && !hijack_silent)
			printk("%s: send_dirlist_buf(): ksock_rw(%u) returned %d\n", parms->servername, p->buf_used, sent);
		if (sent >= 0)
			sent = -ECOMM;
		return sent;
	}
	p->buf_used = 0;
	return 0;
}

static int	// callback routine for filp->f_op->readdir()
format_dir (server_parms_t *parms, filldir_parms_t *p, ino_t ino, char *name, int namelen)
{
	struct dentry	*dentry = NULL;
	struct inode	*inode;
	unsigned long	mode;
	char		*b;
	tm_t		tm;
	char		ftype, *lname = parms->tmp3;
	int		rc, linklen = 0;

	if ((p->buf_size - p->buf_used) < (58 + namelen)) { 
		if ((rc = send_dirlist_buf(parms, p, 0)))	// empty the buffer
			return rc;
	}

	strcpy(p->name, name);		// fill in "tail" of p->path[]
	if (p->sb->s_magic == PROC_SUPER_MAGIC) {	// iget() doesn't work properly for /proc fs
		dentry = lnamei(p->path);
		if (IS_ERR(dentry) || !(inode = dentry->d_inode)) {
			if (!hijack_silent)
                		printk("%s: lnamei(%s) failed, rc=%ld\n", parms->servername, p->path, PTR_ERR(dentry));
                	return -ENOENT;
		}
        } else if (!(inode = iget(p->sb, ino))) {	// iget() is magnitudes faster than lnamei()
		if (!hijack_silent)
			printk("%s: iget(%lu) failed\n", parms->servername, ino);
		return -ENOENT;
	}
	mode = inode->i_mode;
	switch (mode & S_IFMT) {
		case S_IFLNK:	ftype = 'l'; break;
		case S_IFDIR:	ftype = 'd'; break;
		case S_IFCHR:	ftype = 'c'; break;
		case S_IFBLK:	ftype = 'b'; break;
		case S_IFIFO:	ftype = 'p'; break;
		case S_IFSOCK:	ftype = 's'; break;
		case S_IFREG:	ftype = '-'; break;
		default:	ftype = '-'; break;
	}
	if (!p->full_listing) {
		if (ftype == '-' || ftype == 'l') {
			b = append_string((p->buf + p->buf_used), name, 0);
			goto done;
		}
		rc = 0;
		goto exit;	// not a file or symlink:  skip it
	}

	b = p->buf + p->buf_used;
	*b++ = ftype;
	*b++ = (mode & S_IRUSR) ? 'r' : '-';
	*b++ = (mode & S_IWUSR) ? 'w' : '-';
	*b++ = MODE_XBIT('S', mode & S_IXUSR, mode & S_ISUID);
	*b++ = (mode & S_IRGRP) ? 'r' : '-';
	*b++ = (mode & S_IWGRP) ? 'w' : '-';
	*b++ = MODE_XBIT('S', mode & S_IXGRP, mode & S_ISGID);
	*b++ = (mode & S_IROTH) ? 'r' : '-';
	*b++ = (mode & S_IWOTH) ? 'w' : '-';
	*b++ = MODE_XBIT('T', mode & S_IXOTH, mode & S_ISVTX);

	b += sprintf(b, "%5u %-8u %-8u", inode->i_nlink, inode->i_uid, inode->i_gid);
	if (ftype == 'c' || ftype == 'b') {
		b += sprintf(b, " %3u, %3u", MAJOR(inode->i_rdev), MINOR(inode->i_rdev));
	} else {
		b += sprintf(b, " %8lu", inode->i_size);
		p->blockcount += inode->i_blocks;
	}

	b = format_time(hijack_convert_time(inode->i_mtime, &tm), p->current_year, b);
	*b++ = ' ';

	if (ftype == 'l') {
		linklen = sys_readlink(p->path, lname, 512);
		if (linklen <= 0) {
			//printk("%s: readlink(%s) (dentry=%p) failed, rc=%d\n", parms->servername, p->path, dentry, linklen);
			linklen = 0;
		}
	}
	lname[linklen] = '\0';

	p->buf_used = b - p->buf;
	if ((p->buf_size - p->buf_used) < ((parms->protocol ? (24+namelen) : 7) + namelen + (linklen ? (2 * linklen) : namelen))) {
		if ((rc = send_dirlist_buf(parms, p, 0)))	// empty the buffer
			goto exit;
		b = p->buf;
	}

	if (parms->protocol) {
		*b++ = '<';
		*b++ = 'A';
		*b++ = ' ';
		*b++ = 'H';
		*b++ = 'R';
		*b++ = 'E';
		*b++ = 'F';
		*b++ = '=';
		*b++ = '"';
		b = append_string(b, linklen ? lname : name, 1);
		if (ftype == 'd')
			*b++ = '/';
		*b++ = '"';
		*b++ = '>';
	}

	b = append_string(b, name, 0);
	if (parms->protocol) {
		*b++ = '<';
		*b++ = '/';
		*b++ = 'A';
		*b++ = '>';
		if (ftype == 'd')
			*b++ = '/';
	}

	if (linklen) {
		*b++ = ' ';
		*b++ = '-';
		*b++ = '>';
		*b++ = ' ';
		b = append_string(b, lname, 0);
	}
done:
	*b++ = '\r';
	*b++ = '\n';
	rc = 0;
	p->buf_used = b - p->buf;
	if ((p->buf_size - p->buf_used) < DIRLIST_TRAILER_MAX)
		rc = send_dirlist_buf(parms, p, 0);	// empty the buffer
exit:
	if (dentry)
		dput(dentry);
	else if (inode)
		iput(inode);
	return rc;
}

static const char dirlist_header[] =
	"HTTP/1.1 200 OK\r\n"
	"Connection: close\r\n"
	"Content-Type: text/html\r\n\r\n"
	"<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 3.2 Final//EN\">\r\n"
	"<html>"
	"<head><title>Index of %s</title></head>"
	"<body>"
	"<font size=+1><b>Index of %s</b></font>"
	"<hr><pre style=\"font-family: monospace, Courier\">\r\n";

static int
send_dirlist (server_parms_t *parms, char *path, int full_listing)
{
	int		rc, pathlen;
	struct file	*filp;
	unsigned int	response = 0;
	filldir_parms_t	p;

	current->policy = SCHED_OTHER;
	memset(&p, 0, sizeof(p));
	p.show_dotfiles	= parms->show_dotfiles;
	pathlen = strlen(path);
	if (path[pathlen-1] == '/') {
		if (pathlen > 1)
			path[--pathlen] = '\0';
	} else {	// check for globbing in final path element:
		int globbing = 0;
		char *d = &path[pathlen];
		while (*--d != '/') {
			if (*d == '*' || *d == '?')
				++globbing;
		}
		if (globbing) {
			pathlen = d - path;
			if (pathlen == 0) {
				path = "/";
				pathlen = 1;
			}
			*d++ = '\0';
			p.pattern = d;
			if (*d == '.')
				p.show_dotfiles = 1;
		}
	}
	filp = filp_open(path,O_RDONLY,0);
	if (IS_ERR(filp) || !filp) {
		if (parms->verbose || parms->protocol || (int)filp != -ENOENT) {
			if (!hijack_silent)
				printk("%s: filp_open(\"%s\") failed (%d)\n", parms->servername, path, (int)filp);
		}
		response = 550;
	} else {
		int (*readdir) (struct file *, void *, filldir_t);
		struct file_operations *fops;
		struct dentry *dentry = filp->f_dentry;
		struct inode  *inode;
		if (!dentry || !(inode = dentry->d_inode) || !(fops = filp->f_op)) {
			response = 550;
		} else if (!(readdir = fops->readdir)) {
			response = 553;
		} else if (!(p.buf = (char *)__get_free_page(GFP_KERNEL))) {
			response = 451;
		} else if (!(p.nam = (char *)__get_free_page(GFP_KERNEL))) {
			response = 451;
		} else if (!(response = open_datasock(parms))) {
			tm_t		tm;

			p.current_year	= hijack_convert_time(CURRENT_TIME, &tm)->tm_year;
			p.buf_size	= PAGE_SIZE;
			p.nam_size	= PAGE_SIZE;
			p.full_listing	= full_listing;
			strcpy(p.path, path);
			p.path_len	= pathlen;
			if (p.path[pathlen - 1] != '/')
				p.path[pathlen++] = '/';
			p.name		= p.path + pathlen;
			p.sb		= dentry->d_sb;
			p.use_http	= (parms->protocol == khttpd);
			if (p.use_http)
				p.buf_used = sprintf(p.buf, dirlist_header, path, path);
			do {
				p.nam_used = 0;
				p.filecount = 0;
				schedule(); // give the music player a chance to run
				down(&inode->i_sem);
				rc = readdir(filp, &p, filldir);	// anything "< 0" is an error
				up(&inode->i_sem);
				if (rc < 0) {
					if (!hijack_silent)
						printk("%s: readdir() returned %d\n", parms->servername, rc);
				} else {
					unsigned int pos = 0;
					rc = 0;
					while (pos < p.nam_used) {
						ino_t	ino     = *(ino_t *)(p.nam + pos);
						char	*name   = p.nam + (pos += sizeof(ino));
						int	namelen = strlen(name);
						pos = (pos + namelen + (1 + 3)) & ~3;
						rc = format_dir(parms, &p, ino, name, namelen);
						if (rc < 0) {
							if (parms->verbose && !hijack_silent)
								printk("%s: format_dir(\"%s\") returned %d\n", parms->servername, p.name, rc);
							break;
						}
					}
				}
			} while (!rc && p.filecount);
			if (rc || (rc = send_dirlist_buf(parms, &p, 1)))
				response = 426;
			if (!p.use_http)
				sock_release(parms->datasock);
			if (p.nam)
				free_page((unsigned long)p.nam);
			if (p.buf)
				free_page((unsigned long)p.buf);
		}
		filp_close(filp, NULL);
	}
	current->policy = SCHED_RR;
	return response;
}

typedef enum {auth_none, auth_basic, auth_full} khttpd_auth_t;

static int
khttpd_check_auth (server_parms_t *parms, khttpd_auth_t authtype)
{
	static const char khttpd_response[] =
		"HTTP/1.1 401 Unauthorized\r\n"
		"Connection: close\r\n"
		"WWW-Authenticate: Basic realm=\"Empeg-%s\"\r\n"
		"Content-Type: text/html\r\n\r\n"
		"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		"<html><head>"
		"<title>401 Unauthorized</title>"
		"</head><body>"
		"<h1>401 Unauthorized</h1>"
		"%s Authorization Required<p>"
		"</body></html>\r\n";

	if (!*hijack_khttpd_basic && !*hijack_khttpd_full)
		parms->auth = auth_full;
	if (parms->auth >= authtype) {
		return 0;
	} else {
		char		*buf = parms->cwd, *auths = (authtype == auth_full) ? "Full" : "Basic";
		unsigned int	len, rc;

		len = sprintf(buf, khttpd_response, auths, auths);
		rc = ksock_rw(parms->clientsock, buf, len, -1);
		if (rc != len && parms->verbose && !hijack_silent)
			printk(KHTTPD": respond(): ksock_rw(%d) returned %d, data=\"%s\"\n", len, rc, buf);
		return 1;
	}
}

static void
khttpd_respond (server_parms_t *parms, int rcode, const char *title, const char *text)
{
	static const char khttpd_response[] =
		"HTTP/1.1 %d %s\r\n"
		"Connection: close\r\n"
		"Allow: GET, HEAD\r\n"
		"Content-Type: text/html\r\n\r\n"
		"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		"<html><head>"
		"<title>%d %s</title>"
		"</head><body>"
		"<h1>%d %s</h1>"
		"%s<p>"
		"</body></html>\r\n";

	char		*buf = parms->cwd;
	unsigned int	len, rc;

	len = sprintf(buf, khttpd_response, rcode, title, rcode, title, rcode, title, text ? text : "");
	rc = ksock_rw(parms->clientsock, buf, len, -1);
	if (rc != len && parms->verbose && !hijack_silent)
		printk(KHTTPD": respond(): ksock_rw(%d) returned %d, data=\"%s\"\n", len, rc, buf);
}

static void
khttpd_redirect (server_parms_t *parms, const char *path, char *slash)
{
	static const char http_redirect[] =
		"HTTP/1.1 302 Found\r\n"
		"Location: %s%s\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html\r\n\r\n"
		"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		"<html><head>"
		"<title>301 Moved</title>"
		"</head><body>"
		"<h1>Moved</h1>"
		"The document has moved <a href=\"%s%s\">here</a>.<p>"
		"</body></html>\r\n";
	char *buf = parms->tmp3;

	unsigned int	len, rc;

	len = sprintf(buf, http_redirect, path, slash, path, slash);
	rc = ksock_rw(parms->clientsock, buf, len, -1);
	if (rc != len && !hijack_silent)
		printk(KHTTPD": redirect(): ksock_rw(%d) returned %d\n", len, rc);
}

static const char audio_mpeg[]		= "audio/mpeg";
static const char audio_wav[]		= "audio/x-wav";
static const char application_ogg[]	= "application/ogg";
static const char audio_flac[]		= "audio/x-flac";	// or audio/flac, or application/x-flac ??
static const char audio_wma[]		= "audio/x-ms-wma";
static const char audio_m3u[]		= "audio/x-mpegurl";
static const char text_plain[]		= "text/plain";
static const char text_html[]		= "text/html";
static const char text_css[]		= "text/css";
static const char text_xml[]		= "text/xml";
static const char application_octet[]	= "application/octet-stream";
static const char application_x_tar[]	= "application/x-tar";
static const char video_xmpegurl[]	= "video/x-mpegurl";

typedef struct mime_type_s {
	const char	*pattern;	// a "glob" expression using * and/or ? wildcards
	const char	*mime;		// the mime-type for matching paths
	const int	is_webfile;	// "1" == allow access even when "khttpd_files=0"
} mime_type_t;

static const mime_type_t mime_types[] = {
	{"*.tiff",		"image/tiff",		1},
	{"*.jpg",		"image/jpeg",		1},
	{"*.gif",		"image/gif",		1},
	{"*.png",		"image/png",		1},
	{"*.htm",		 text_html,		1},
	{"*.html",		 text_html,		1},
	{"*.xml",		 text_xml,		1},
	{"*.xsl",		 text_xml,		1},
	{"*.css",		 text_css,		1},
	{"*.js",		 NULL,			1},
	{"*.jar",		 NULL,			1},
	{"*.html",		 text_html,		1},
	{"*.txt",		 text_plain,		0},
	{"*.text",		 text_plain,		0},
	{"*.wav",		 audio_wav,		0},
	{"*.m3u",		 audio_m3u,		1},
	{"*.mp3",		 audio_mpeg,		0},
	{"*.wma",		 audio_wma,		0},
	{"*.ogg",		 application_ogg,	0},
	{"*.flac",		 audio_flac,		0},
	{"*.gz",		"application/x-gzip",	0},
	{"*.tar",		 application_x_tar,	0},
	{"*.tgz",		 application_x_tar,	0},
	{"*/config.ini",	 text_plain,		0},
	{"*/tags",		 text_plain,		0},
	{"/etc/*",		 text_plain,		0},
	{"/proc/*",		 text_plain,		0},
	{"/drive?/fids/*1",	 text_plain,		0},
	{"/empeg/fids?/*1",	 text_plain,		0},
	{"*bin/*",		 application_octet,	0},
	{"/proc/version",	 text_plain,		1},
	{"*.m1u",		 video_xmpegurl,	1},
	{"*.mpg",		 video_xmpegurl,	1},
	{NULL,			NULL,			0}};

static int
get_mime_type (const char *path, const char **mimetype)
{
	const char		*pattern;
	const mime_type_t	*m = mime_types;
	while ((pattern = m->pattern) && !hijack_glob_match(path, pattern))
		++m;
	if (mimetype)
		*mimetype = m->mime;
	return m->is_webfile;
}

static void
find_tags (char *buf, int buflen, char *labels[], char *values[])
{
	static char	*null_label = "";
	char		*start = buf;
	int		n, remaining;

	for (n = 0; labels[n] != NULL; ++n)
		values[n] = null_label;
	remaining = n;
	while (remaining && (buf - start) < buflen) { // allows handling of embedded '\0' characters
		int	i;
		char	c;
		for (i = 0; i < n; ++i) {
			if (!*values[i] && !strxcmp(buf, labels[i], 1)) {
				buf += strlen(labels[i]);
				if ((c = *buf) && c != '\n' && c != '\r') {
					--remaining;
					if (!strcmp(buf,"vorbis") && !strcmp(labels[i],"codec="))
						values[i] = "ogg";
					else
						values[i] = buf;
				}
				break;
			}
		}
		while ((c = *buf) && c != '\n' && c != '\r')
			++buf;
		*buf++ = '\0';
	}
	schedule(); // give the music player a chance to run
}

static void
combine_artist_title (char *artist, char *title, char *combined, int maxlen)
{
	int	len;

	--maxlen;	// reserve room for '\0' at end
	if (*artist) {
		int amax = (maxlen / 2) - 3;
		len = strlen(artist);
		if (len > amax)
			len = amax;
		memcpy(combined, artist, len);
		memcpy(combined + len, " - ", 3);
		combined += len + 3;
		maxlen   -= len + 3;
	}
	len = strlen(title);
	if (len) {
		if (len > maxlen)
			len = maxlen;
		memcpy(combined, title, len);
	}
	combined[len] = '\0';
}

static int
open_fid_file (char *path)
{
	int		fd;

	if ((fd = open(path, O_RDONLY, 0)) < 0) {	// try the specified drive first
		path[11] ^= 1;				// failed, now try the other drive
		if ((fd = open(path, O_RDONLY, 0)) < 0)
			path[11] ^= 1;			// point back at original drive
	}
	return fd;
}

static int
khttp_send_file_header (server_parms_t *parms, char *path, off_t length, char *buf, int bufsize)
{
	static char	*labels[] = {"type=", "artist=", "title=", "codec=", NULL};
	struct 		{char *type, *artist, *title, *codec;} tags;
	int		len;
	const char	*mimetype = application_octet, *rcode = "200 OK";
	off_t		clength = length;
	char		artist_title[128];

	artist_title[0] = '\0';
	if (hijack_glob_match(path, "/empeg/fids?/*0")) {
		char c, *lastc = path + strlen(path) - 1;
		int		fd, size;

		*lastc = '1';
		fd = open_fid_file(path);	// open tagfile
		*lastc = '0';
		if (fd > 0) {
			size = read(fd, buf, bufsize/2);
			buf[size] = '\0';
			close(fd);
			schedule(); // give the music player a chance to run
			find_tags(buf, size, labels, (char **)&tags);
			buf += size + 1;
			c = tags.type[0];
			if (TOUPPER(c) != 'P' && tags.codec[0]) {
				switch (TOUPPER(tags.codec[1])) {
					case 'P': mimetype = audio_mpeg;	break;
					case 'M': mimetype = audio_wma;		break;
					case 'A': mimetype = audio_wav;		break;
					case 'G': mimetype = application_ogg;	break;
					case 'L': mimetype = audio_flac;	break;
				}
				combine_artist_title(tags.artist, tags.title, artist_title, sizeof(artist_title));
			}
		}
	} else {
		(void) get_mime_type(path, &mimetype);
	}
	if (clength && parms->end_offset != -1) {
		clength = parms->end_offset + 1 - parms->start_offset;
		rcode = "206 Partial content";
	}
	len = sprintf(buf, "HTTP/1.1 %s\r\nConnection: close\r\n", rcode);
	if (clength) {
		len += sprintf(buf+len, "Accept-Ranges: bytes\r\nContent-Length: %lu\r\n", clength);
		if (parms->end_offset != -1)
			len += sprintf(buf+len, "Content-Range: bytes %lu-%lu/%lu\r\n", parms->start_offset, parms->end_offset, length);
	}
	if (parms->nocache || !strxcmp(path, "/proc/", 1) || !strxcmp(path, "/dev/", 1)) {
		len += sprintf(buf+len,	"Cache-Control: no-cache,must-revalidate,max-age=0,no-store\r\n"
					"Pragma: no-cache,no-store\r\n"
					"Expires: -1\r\n" );
	}
	if (mimetype)
		len += sprintf(buf+len, "Content-Type: %s\r\n", mimetype);
	if (artist_title[0]) {	// tune title for WinAmp, XMMS, Save-To-Disk, etc..
		if (parms->icy_metadata) {
			len += sprintf(buf+len, "icy-name:%s\r\n", artist_title);
		} else if (parms->is_mozilla) { // Mmm.. I wonder if we could we just do \" here for all agents?
			len += sprintf(buf+len, "Content-Disposition: attachment; filename=\"%s.%s\"\r\n", artist_title, tags.codec);
		} else {
			len += sprintf(buf+len, "Content-Disposition: attachment; filename=%s.%s\r\n", artist_title, tags.codec);
		}
	}
	buf[len++] = '\r';
	buf[len++] = '\n';
	if (len != ksock_rw(parms->datasock, buf, len, -1))
		return 426;
	return 0;
}

typedef struct file_xfer_s {
	int		fd;
	unsigned int	buf_size;
	char *		buf;
	int		writing;
	int		redirected;
	struct stat	st;
} file_xfer_t;

static int
prepare_file_xfer (server_parms_t *parms, char *path, file_xfer_t *xfer, int writing)
{
	int	fd, response = 0, flags;
	off_t	start_offset = parms->start_offset;
	off_t	end_offset = parms->end_offset;

	parms->datasock = NULL;
	xfer->buf_size = PAGE_SIZE;
	xfer->buf = NULL;
	xfer->writing = writing;
	xfer->redirected = 0;
	if (writing)
		flags = start_offset ? O_RDWR : O_RDWR|O_CREAT|O_TRUNC;
	else
		flags = O_RDONLY;
	if (!writing && parms->protocol && hijack_glob_match(path, "/empeg/fids?/*"))
		fd = open_fid_file(path);
	else
		fd = open(path, flags, 0666 & ~parms->umask);
	xfer->fd = fd;
	if (fd < 0) {
		if (!hijack_silent)
			printk("%s: open(\"%s\") failed, rc=%d\n", parms->servername, path, fd);
		response = 550;
	} else if (!(xfer->buf = (unsigned char *)__get_free_page(GFP_KERNEL))) {
		response = 451;
	} else if (!parms->running_playlist) {
		if (sys_newfstat(fd, &xfer->st)) {
			if (!hijack_silent)
				printk("%s: fstat(%s) failed\n", parms->servername, path);
			response = 550;
		} else if (S_ISDIR(xfer->st.st_mode)) {
			if (writing || !parms->protocol || parms->generate_playlist) {
				response = 550;
			} else {
				khttpd_redirect(parms, path, "/");
				xfer->redirected = 1;
			}
		} else if (end_offset != -1 && (xfer->st.st_size && end_offset >= xfer->st.st_size)) {
			response = parms->protocol ? 416 : 553;
		} else if (start_offset && ((xfer->st.st_size && start_offset > xfer->st.st_size) || start_offset != lseek(fd, start_offset, 0))) {
			if (!hijack_silent)
				printk("%s: lseek(%s,%lu/%lu) failed\n", parms->servername, path, start_offset, xfer->st.st_size);
			response = parms->protocol ? 416 : 553;
		}
	}
	if (!response)
		response = open_datasock(parms);
	return response;
}

static void
cleanup_file_xfer (server_parms_t *parms, file_xfer_t *xfer)
{
	int	fd = xfer->fd;
	if (fd >= 0) {
		if (xfer->writing)
			sys_fsync(fd);
		close(fd);
	}
	if (!parms->protocol && parms->datasock) {
		sock_release(parms->datasock);
		parms->datasock = NULL;
	}
	if (xfer->buf)
		free_page((unsigned long)xfer->buf);
	parms->start_offset =  0;
	parms->end_offset   = -1;
}

static unsigned int
str_val (unsigned char *str)
{
	unsigned int val = 0;
	(void)get_number(&str, &val, 10, NULL);
	return val;
}

typedef struct http_response_s {
	int		 rcode;
	const char	*rtext;
} http_response_t;

static const http_response_t access_not_permitted  = {403, "Access Not Permitted"};
static const http_response_t invalid_playlist_path = {404, "Invalid playlist path"};

static http_response_t *
convert_rcode (int rcode)
{
	http_response_t	*response;

	if (!rcode)
		response = NULL;
	else if (rcode == 416)
		response = &(http_response_t){416, "Requested Range Not Satisfiable"};
	else
		response = &(http_response_t){404, "Error Retrieving File"};
	return response;
}

static unsigned int
encode_url (unsigned char *out, unsigned char *s, int partial_encode)
{
	unsigned char c, *start = out;

	while ((c = *s++)) {
		if (INRANGE(c,'a','z') || INRANGE(c,'A','Z') || c == '_' || c == '-' || INRANGE(c, '0','9')) {
			*out++ = c;
		} else if (partial_encode == 0) {
			extern const unsigned char hexchars[];
			*out++ = '%';
			*out++ = hexchars[c >> 4];
			*out++ = hexchars[c & 0xf];
		} else if (partial_encode == 1) { // for netscape, and stupid iTunes playlist display
			*out++ = (c == ' ' || c == '?' || c == '%' || c == '#' || c == '!' || c == '@' || c == '"') ? '_' : c;
		} else {  // (partial_encode == 2) // for xml
			if (c == '<' || c == '>' || c == '&') {
				*out++ = '&';
				if (c == '&') {
					*out++ = 'a';
					*out++ = 'm';
					*out++ = 'p';
				} else {
					*out++ = (c == '>') ? 'g' : 'l';
					*out++ = 't';
				}
				c = ';';
			} else if (c == '"') {
				*out++ = '&';
				*out++ = 'q';
				*out++ = 'u';
				*out++ = 'o';
				*out++ = 't';
				c = ';';
			}
			*out++ = c;
		}
	}
	*out = '\0';	// paranoia
	return out - start;
}

static unsigned int
encode_tag1 (unsigned char *out, const char *tagname, unsigned char *s)
{
	unsigned char *start = out;

	out += sprintf(out, " %s=\"", tagname);
	out += encode_url(out, s, 2);
	out += sprintf(out, "\"");
	return out - start;
}

static unsigned int
encode_tag2 (unsigned char *out, const char *tagname, unsigned char *s)
{
	unsigned char *start = out;

	out += sprintf(out, "\t\t\t<%s>", tagname);
	out += encode_url(out, s, 2);
	out += sprintf(out, "</%s>\r\n", tagname);
	return out - start;
}

static const http_response_t *
send_playlist (server_parms_t *parms, char *path)
{
	http_response_t	*response = NULL;
	unsigned int	secs = 0, start = 0, count = 0, limit = 0x7fffffff, playlist_len = 0, running_len = 0;
	unsigned char	*p, subpath[] = "/empeg/fids0/XXXXXXXXXX", artist_title[128], fidtype;
	int		pfid, fid, size, used = 0, xmit_threshold, fidfiles[16], fidx = -1;	// up to 16 levels of nesting
	static const char *playlist_format[3] = {text_html, audio_m3u, text_xml};
	static char	*tagtypes[2] = {"playlist", "tune"};
	static char	*labels[] = {"type=", "artist=", "title=", "codec=", "duration=", "source=", "length=", "genre=", "year=", "comment=", "tracknr=", "offset=", "options=", "bitrate=", "samplerate=", NULL};
	struct 		{char *type, *artist, *title, *codec, *duration, *source, *length, *genre, *year, *comment, *tracknr, *offset, *options, *bitrate, *samplerate;} tags;
	const char	*tagtype, *encoding;
	file_xfer_t	xfer;

	// extract fid from path[]:
	p = &path[13];
	if (!get_number(&p, &pfid, 16, ""))
		return &invalid_playlist_path;
	fid = pfid |= 1;	// we know the fid ends in "1", but make sure anyway..

	start = parms->offset;
	if (parms->count)
		limit = parms->count;

	memset(&tags, 0, sizeof(tags));
	if (fid == 1) {
		path = "/dev/hda3";
		parms->running_playlist = 1;
		fidtype = 'P';
		tagtype = tagtypes[0];
		if ((response = convert_rcode(prepare_file_xfer(parms, path, &xfer, 0))))
			goto cleanup;
		/*
		 * "FID=001" for Mark Cushman: dump part of current running order.
		 * FIXME: check for errors here and set response=451?
		 */
		lseek(xfer.fd, 0x10, 0);
		read(xfer.fd, (void *)&playlist_len, sizeof(playlist_len));
		lseek(xfer.fd, 0x18, 0);
		read(xfer.fd, (void *)&running_len, sizeof(running_len));
		if (limit > running_len)
			limit = running_len;

		fidfiles[++fidx] = xfer.fd; xfer.fd = -1;
		strcpy(artist_title, "Current Running Order");
		tags.length = "";
		tags.year = "";
		tags.options = "";
	} else {

		// read the tagfile:
		if ((response = convert_rcode(prepare_file_xfer(parms, path, &xfer, 0))))
			goto cleanup;
		size = read(xfer.fd, parms->tmp3, sizeof(parms->tmp3));
		close(xfer.fd); xfer.fd = -1;
		if (size < 0)
			size = 0;
		parms->tmp3[size] = '\0';	// Ensure zero-termination of the data

		// parse the tagfile for the tags we are interested in:
		find_tags(parms->tmp3, size, labels, (char **)&tags);
		fidtype = TOUPPER(tags.type[0]);
		if (fidtype != 'T' && fidtype != 'P') {
			response = &(http_response_t){408, "Invalid tag file"};
			goto cleanup;
		}
		tagtype = tagtypes[fidtype == 'T'];
		if (!tags.title[0])
			tags.title = path;
		combine_artist_title(tags.artist, tags.title, artist_title, sizeof(artist_title));

		// If tagfile is for a "tune", then send a .m3u playlist for it, and quit
		if (fidtype == 'T') {
			if (parms->generate_playlist != xml) {
				secs = str_val(tags.duration) / 1000;
				used  = sprintf(xfer.buf, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: %s\r\n\r\n"
					"#EXTM3U\r\n#EXTINF:%u,%s\r\nhttp://%s%s/", audio_m3u, secs, artist_title, parms->user_passwd, parms->hostname);
				used += encode_url(xfer.buf+used, artist_title, 0);
				used += sprintf(xfer.buf+used, ".%s?FID=%x&EXT=.%s\r\n", tags.codec, pfid^1, tags.codec);
				(void)ksock_rw(parms->datasock, xfer.buf, used, -1);
				goto cleanup;
			}
		}
	}

	// Send the playlist header, in either html, m3u, or xml format:
	encoding = (player_version >= MK2_PLAYER_v3a1) ? "UTF-8" : "ISO-8859-1";
	used += sprintf(xfer.buf+used, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: %s; charset=%s\r\n\r\n",
					playlist_format[parms->generate_playlist - 1], encoding);

	switch (parms->generate_playlist) {
		case html:
			used += sprintf(xfer.buf+used,
				"<html><head><title>%s playlists: %s</title></head>\r\n"
				"<body><table bgcolor=\"WHITE\" border=\"2\"><thead>\r\n"
				"<tr><td> <a href=\"/", parms->hostname, artist_title);
			used += encode_url(xfer.buf+used, artist_title, 1);
			used += sprintf(xfer.buf+used,".m3u?FID=%x&EXT=.m3u\"><b>Stream</b></a> ", pfid);
			if (parms->auth == auth_full) {
				used += sprintf(xfer.buf+used, "<td> <a href=\"/?NODATA&SERIAL=%%23%x\"><b>Play</b></a> ", pfid^1);
				used += sprintf(xfer.buf+used, "<td> <a href=\"/?NODATA&SERIAL=%%23%x-\"><b>Insert</b></a> ", pfid^1);
				used += sprintf(xfer.buf+used, "<td> <a href=\"/?NODATA&SERIAL=%%23%x%%2B\"><b>Append</b></a> ", pfid^1);
				used += sprintf(xfer.buf+used, "<td> <a href=\"/?FID=%x\"><b>Tags</b></a> ", pfid);
			}
			used += sprintf(xfer.buf+used, "<td align=center> <font size=+2><b><em>%s</em></b></font> <td> <b>Length</b> "
				"<td> <b>Type</b> <td> <b>Artist</b> <td> <b>Source</b><tbody>\r\n", tags.title);
			break;
		case m3u:
			used += sprintf(xfer.buf+used, "#EXTM3U\r\n");
			break;
		case xml: // xml
		{	int full_access = parms->auth == auth_full;
			used += sprintf(xfer.buf+used,
				"<?xml version=\"1.0\" encoding=\"%s\"?>\r\n"
				"<?xml-stylesheet type=\"text/xsl\" href=\"%s\"?>\r\n"
				"<%s stylesheet=\"%s\" host=\"%s\" allow_files=\"%d\" allow_commands=\"%d\" "
				"type=\"%s\" tagfid=\"%x\" fid=\"%x\" length=\"%s\" "
				"year=\"%s\" options=\"%s\"", encoding,
				parms->style, (fidtype == 'T' ? "item" : "playlist"), parms->style, parms->hostname, full_access,
				full_access, tagtype, pfid, pfid^1, tags.length, tags.year, tags.options);
			used += encode_tag1(xfer.buf+used, "genre",   tags.genre);
			used += encode_tag1(xfer.buf+used, "title",   tags.title);
			used += encode_tag1(xfer.buf+used, "artist",  tags.artist);
			used += encode_tag1(xfer.buf+used, "source",  tags.source);
			used += encode_tag1(xfer.buf+used, "comment", tags.comment);
			if (parms->running_playlist)
				used += sprintf(xfer.buf+used, " runOrderLen=\"%u\" playListLen=\"%u\"", running_len, playlist_len);
			if (fidtype == 'T') {
				char dbuf[32];
				used += encode_tag1(xfer.buf+used, "tracknr", tags.tracknr);
				used += encode_tag1(xfer.buf+used, "bitrate", tags.bitrate);
				used += encode_tag1(xfer.buf+used, "samplerate", tags.samplerate);
				used += encode_tag1(xfer.buf+used, "codec", tags.codec);
				secs = str_val(tags.duration) / 1000;
				sprintf(dbuf, "%u:%02u", secs/60, secs%60);
				used += encode_tag1(xfer.buf+used, "duration", dbuf);
				used += encode_tag1(xfer.buf+used, "offset", tags.offset);
				used += sprintf(xfer.buf+used, "/>\r\n");
				goto sendit;
			}
			used += sprintf(xfer.buf+used, ">\r\n\t<items>\r\n");
			break;
		}
		default:
	}

	if (!parms->running_playlist) {
open_fidfile:
		// Read the playlist's fidfile, and process each fid in turn:
		if (++fidx >= (sizeof(fidfiles) / sizeof(fidfiles[0]))) {
			--fidx;
			used += sprintf(xfer.buf+used, "<font color=red>playlists nested too deep</font>\n");
			goto aborted;
		}
		sprintf(subpath+13, "%x", fid^1);
		fidfiles[fidx] = open_fid_file(subpath);
		if (fidfiles[fidx] < 0) {
			int rc = fidfiles[fidx--];	/* FIXME? why -- here ??? */
			if (rc != -ENOENT) {
				used += sprintf(xfer.buf+used, "<font color=red>open(\"%s\") failed, rc=%d</font>\n", subpath, rc);
				goto aborted;
			} // else: empty playlist; just continue..
		}
		path[11] = subpath[11];	// update the drive number '0'|'1'
	}
	xmit_threshold = (parms->generate_playlist == xml) ? 1536 : 512;
	while (fidx >= 0) {
		while (count < limit) {
			int sublen, fd, entries = 0;
			if (parms->running_playlist && fidx == 0) {
				unsigned int fidTableIndex, rc;
		 		/*
		 		 * "FID=001" for Mark Cushman: dump part of current running order.
		 		 * FIXME: check for errors here and set response=451?
		 		 *
		 		 * Here we do some fancy seeking to find/position the next fid within
		 		 * the current running order.  Then we let the mainline code just read it
		 		 * as if it was from an ordinary playlist file.
		 		 *
		 		 * FIXME: M.Cushman probably expects us to automatically skip ahead
		 		 * here to the "currently playing FID", possibly adjusted with a
		 		 * negative OFFSET=nn.
		 		 *
		 		 * FIXME: Allow negative OFFSET=nn (?)
		 		 *
		 		 * FIXME: Number the returned "<item>" lines, like this:
		 		 *
		 		 * 	<item number="2">
		 		 *
		 		 * FIXME: When dumping XML for the currently playing FID,
		 		 * change the "<item>" line to this:
		 		 *
		 		 * 	<item number="3" playing="yes">
		 		 */
				lseek(fidfiles[0], 0x200 + 8 * playlist_len + (start + count) * 4, 0);
				rc = read(fidfiles[0], (void *)&fidTableIndex, sizeof(fidTableIndex));
				if (rc != sizeof(fidTableIndex)) {
					if (!hijack_silent)
						printk("read(fidTableIndex(%d) failed, rc=%d\n", start + count, rc);
					break;
				}
				lseek(fidfiles[0], 0x200 + 8 * fidTableIndex, 0);
			}
			if (sizeof(fid) != read(fidfiles[fidx], (char *)&fid, sizeof(fid)))
				break;
			if ((!parms->running_playlist || fidx != 0) && start) {
				--start;
				continue;
			}
			++count;

			if (used >= xmit_threshold && (used -= ksock_rw(parms->datasock, xfer.buf, used, -1)))
				goto cleanup;

			schedule(); // give the music player a chance to run
			// read in the tagfile for this fid
			fid |= 1;
			sublen = 13+sprintf(subpath+13, "%x", fid);
			fd = open_fid_file(subpath);
			if (fd < 0) {
				// Hmmm.. missing tags file.  This IS a database error, and should never happen.  But it does..
				// But we'll just ignore it here.
				if (parms->generate_playlist == html && !hijack_silent)
					printk(KHTTPD": open(\"%s\") failed, rc=%d\n", subpath, fd);
				continue;
			}
			size = read(fd, parms->tmp3, sizeof(parms->tmp3)-1);
			close(fd);
			path[11] = subpath[11];	// update the drive number '0'|'1'
			if (size <= 0) {
				// Hmmm.. empty tags file.  This IS a database error, and should never happen.
				// But we'll just ignore it here.
				if (parms->generate_playlist == html && !hijack_silent)
					printk(KHTTPD": read(\"%s\") failed, rc=%d\n", subpath, size);
				continue;
			}
			parms->tmp3[size] = '\0';	// Ensure zero-termination of the data

			// parse the tagfile for the tags we are interested in:
			find_tags(parms->tmp3, size, labels, (char **)&tags);
			fidtype = TOUPPER(tags.type[0]);
			if (fidtype == 'P') {
				if (parms->generate_playlist == m3u) {
					if (!tags.length[0] || str_val(tags.length))
						goto open_fidfile;	// nest one level deeper for this playlist
				}
				tagtype = tagtypes[0];
				entries = str_val(tags.length) / 4;
			} else if (fidtype == 'T') {
				tagtype = tagtypes[1];
				secs = str_val(tags.duration) / 1000;
			} else {
				if (parms->generate_playlist == html)
					used += sprintf(xfer.buf+used, "<tr><td colspan=7><font color=red>%s: invalid 'type=%s'</font>\n",
						subpath, tags.type);
				continue;
			}
			if (!tags.title[0])
				tags.title = subpath;
			combine_artist_title(tags.artist, tags.title, artist_title, sizeof(artist_title));

			// spit out an appropriately formed representation for this fid:
			switch (parms->generate_playlist) {
				case html:
					used += sprintf(xfer.buf+used, "<tr><td> <a href=\"/");
					used += encode_url(xfer.buf+used, artist_title, 1);
					used += sprintf(xfer.buf+used, ".m3u?FID=%x&EXT=.m3u\"><em>Stream</em></a> ", fid);
					if (parms->auth == auth_full) {
						used += sprintf(xfer.buf+used, "<td> <a href=\"/?NODATA&SERIAL=%%23%x\"><em>Play</em></a> ", fid^1);
						used += sprintf(xfer.buf+used, "<td> <a href=\"/?NODATA&SERIAL=%%23%x-\"><em>Insert</em></a> ", fid^1);
						used += sprintf(xfer.buf+used, "<td> <a href=\"/?NODATA&SERIAL=%%23%x%%2B\"><em>Append</em></a> ", fid^1);
						used += sprintf(xfer.buf+used, "<td> <a href=\"/?FID=%x\"><em>Tags</em></a> ", fid);
					}
					if (fidtype == 'T') {
						if (parms->auth == auth_full) {
							used += sprintf(xfer.buf+used, "<td> <a href=\"/");
							used += encode_url(xfer.buf+used, artist_title, 1);
							used += sprintf(xfer.buf+used, ".%s?FID=%x&EXT=.%s\">%s</a> ", tags.codec, fid^1, tags.codec, tags.title);
						} else {
							used += sprintf(xfer.buf+used, "<td> %s ", tags.title);
						}
						used += sprintf(xfer.buf+used, "<td align=center> %u:%02u <td> %s <td> %s&nbsp <td> %s&nbsp \r\n",
							secs/60, secs%60, tags.type, tags.artist, tags.source);
					} else {
						used += sprintf(xfer.buf+used, "<td> <a href=\"/?FID=%x&EXT=.htm\">%s</a> "
							"<td align=center> %u <td> %s <td> %s&nbsp <td> %s&nbsp \r\n",
							fid, tags.title, entries, tags.type, tags.artist, tags.source);
					}
					break;
				case m3u:
					if (fidtype == 'T') {
						combine_artist_title(tags.artist, tags.title, artist_title, sizeof(artist_title));
						subpath[sublen - 1] = '0';
						used += sprintf(xfer.buf+used, "#EXTINF:%u,%s\r\nhttp://%s%s/", secs, artist_title, parms->user_passwd, parms->hostname);
						used += encode_url(xfer.buf+used, artist_title, 0);
						used += sprintf(xfer.buf+used, ".%s?FID=%x&EXT=.%s\r\n", tags.codec, fid^1, tags.codec);
					}
					break;
				case xml:
					used += sprintf(xfer.buf+used,
						"\t\t<item>\r\n"
						"\t\t\t<type>%s</type>\r\n"
						"\t\t\t<tagfid>%x</tagfid>\r\n"
						"\t\t\t<fid>%x</fid>\r\n"
						"\t\t\t<year>%s</year>\r\n"
						"\t\t\t<options>%s</options>\r\n",
						tagtype, fid, fid^1, tags.year, tags.options);
					used += encode_tag2(xfer.buf+used, "genre",   tags.genre);
					used += encode_tag2(xfer.buf+used, "title",   tags.title);
					used += encode_tag2(xfer.buf+used, "artist",  tags.artist);
					used += encode_tag2(xfer.buf+used, "source",  tags.source);
					used += encode_tag2(xfer.buf+used, "comment", tags.comment);
					if (fidtype == 'T') {
						used += sprintf(xfer.buf+used,
							"\t\t\t<length>%s</length>\r\n"
							"\t\t\t<tracknr>%s</tracknr>\r\n"
							"\t\t\t<bitrate>%s</bitrate>\r\n"
							"\t\t\t<samplerate>%s</samplerate>\r\n"
							"\t\t\t<codec>%s</codec>\r\n"
							"\t\t\t<duration>%u:%02u</duration>\r\n"
							"\t\t\t<offset>%s</offset>\r\n"
							"\t\t</item>\r\n",
							tags.length, tags.tracknr, tags.bitrate, tags.samplerate, tags.codec, secs/60, secs%60, tags.offset);
					} else { // (fidtype == 'P') {
						used += sprintf(xfer.buf+used,
							"\t\t\t<length>%u</length>\r\n"
							"\t\t</item>\r\n",
							entries);
					}
					break;
				default:
			}
		}
		close(fidfiles[fidx--]);
	}
aborted:
	switch (parms->generate_playlist) {
		case html:
			used += sprintf(xfer.buf+used, "</table><font size=-2>%s</font></body></html>\r\n", hijack_vXXX_by_Mark_Lord);
			break;
		case xml:
			used += sprintf(xfer.buf+used, "\t</items>\r\n</playlist>\r\n");
			break;
		default:
	}
sendit:
	if (used)
		(void) ksock_rw(parms->datasock, xfer.buf, used, -1);
cleanup:
	while (fidx >= 0)
		close(fidfiles[fidx--]);
	cleanup_file_xfer(parms, &xfer);
	return response;
}

static int
send_file (server_parms_t *parms, char *path)
{
	int		size;
	unsigned int	response = 0;
	file_xfer_t	xfer;

	response = prepare_file_xfer(parms, path, &xfer, 0);
	if (!response && !xfer.redirected) {
		off_t	filepos, filesize = xfer.st.st_size;
		if (parms->protocol) {
			if (!filesize)
				parms->end_offset = -1;
			else if (parms->start_offset && parms->end_offset == -1)
				parms->end_offset = filesize - 1;
		}
		if (0 != strxcmp(path, "/proc/", 1) && (!(parms->protocol) || filesize > 0x10000))
			current->policy = SCHED_OTHER;
		if (!parms->protocol || !khttp_send_file_header(parms, path, filesize, xfer.buf, xfer.buf_size)) {
			if (!parms->method_head) {
				filepos = parms->start_offset;
				do {
					int read_size = xfer.buf_size;
					if (parms->end_offset != -1) {
						size = parms->end_offset + 1 - filepos;
						if (size > 0 && size < read_size)
							read_size = size;
					}
					schedule(); // give the music player a chance to run
					size = read(xfer.fd, xfer.buf, read_size);
					if (parms->protocol)
						schedule(); // give the music player a chance to run
					filepos += size;
					if (size < 0) {
						if (!hijack_silent)
							printk("%s: read() failed; rc=%d\n", parms->servername, size);
						if (!parms->protocol)
							response = 451;
					} else if (size && size != ksock_rw(parms->datasock, xfer.buf, size, -1)) {
						if (!parms->protocol)
							response = 426;
						break;
					}
				} while (size > 0);
			}
		}
	}
	cleanup_file_xfer(parms, &xfer);
	current->policy = SCHED_RR;
	return response;
}

static int
kftpd_do_rmdir (server_parms_t *parms, const char *path)
{
	int	rc, response = 250;;

	if ((rc = sys_rmdir(path))) {
		if (!hijack_silent)
			printk(KFTPD": rmdir(\"%s\") failed, rc=%d\n", path, rc);
		response = 550;
	}
	return response;
}

static int
kftpd_do_mkdir (server_parms_t *parms, const char *path)
{
	int	rc, response = 0;

	if ((rc = sys_mkdir(path, 0777 & ~parms->umask))) {
		if (!hijack_silent)
			printk(KFTPD": mkdir(\"%s\") failed, rc=%d\n", path, rc);
		response = 550;
	} else {
		(void) kftpd_dir_response(parms, 257, path, "directory created");
	}
	return response;
}

static int
kftpd_do_chmod (server_parms_t *parms, unsigned int mode, const char *path)
{
	int	rc, response = 200;

	if ((rc = sys_chmod(path, mode))) {
		if (rc != -ENOENT && parms->verbose && !hijack_silent)
			printk(KFTPD": chmod(\"%s\",%d) failed, rc=%d\n", path, mode, rc);
		response = 550;
	}
	return response;
}

static int
kftpd_do_delete (server_parms_t *parms, const char *path)
{
	int	rc, response = 250;

	if ((rc = sys_unlink(path))) {
		if (!hijack_silent)
			printk(KFTPD": unlink(\"%s\") failed, rc=%d\n", path, rc);
		response = 550;
	}
	return response;
}

static int
receive_file (server_parms_t *parms, char *path)
{
	int		size;
	unsigned int	response = 0;
	file_xfer_t	xfer;

	current->policy = SCHED_OTHER;
	response = prepare_file_xfer(parms, path, &xfer, 1);
	if (!response) {
		do {
			schedule(); // give the music player a chance to run
			size = ksock_rw(parms->datasock, xfer.buf, xfer.buf_size, 1);
			if (size < 0) {
				if (!hijack_silent)
					printk(KERN_ERR "receive_file: ksock_rw returned %d\n", size);
				response = 426;
			} else if (size && size != write(xfer.fd, xfer.buf, size)) {
				if (!hijack_silent)
					printk(KFTPD": write(%d) failed\n", size);
				response = 451;
			}
		} while (!response && size > 0);
	}
	cleanup_file_xfer(parms, &xfer);
	current->policy = SCHED_RR;
	return response;
}

static void
append_path (char *path, char *new, char *tmp)
{
	char *p = path;

	if (*new == '/') {
		strcpy(tmp, new);
	} else {
		strcpy(tmp, path);
		strcat(tmp, "/");
		strcat(tmp, new);
	}
	// Now fix-up the path, resolving '.' and '..' and removing consecutive '/'
	*p++ = '/';
	while (*tmp) {
		while (*tmp == '/')
			++tmp;
		if (tmp[0] == '.' && tmp[1] == '.' && (tmp[2] == '/' || tmp[2] == '\0')) {
			tmp += 2;
			while (*--p != '/');
			if (p == path)
				++p;
		} else if (tmp[0] == '.' && (tmp[1] == '/' || tmp[1] == '\0')) {
			if (*++tmp)
				++tmp;
		} else if (*tmp) { // copy simple path element
			if (*(p-1) != '/')
				*p++ = '/';
			while (*tmp && *tmp != '/')
				*p++ = *tmp++;
		}
	}
	*p = '\0';
}

static unsigned char
fromhex (unsigned char x)
{
	if (INRANGE(x,'0','9'))
		return x - '0';
	x = TOUPPER(x);
	if (INRANGE(x, 'A', 'F'))
		return x - 'A' + 10;
	return 16;
}

static void
khttpd_fix_hexcodes (char *buf)
{
	char *s = buf;
	unsigned char *x, x0, x1;

	for (s = buf; *s; ++s) {
		if (*s == '%') {
			x = s + 1;
			if ((x0 = *x++) && (x1 = *x++)) {
				if ((x0 = fromhex(x0)) < 16 && (x1 = fromhex(x1)) < 16) {
					*s = (x0 << 4) | x1;
					do {
						*(x - 2) = *x;
					} while (*x++);
				}
			}
		}
	}
}

#define RELEASECODE(b)	(((b) > 0xf) ? (b) | 0x80000000 : (b) | 1)

static unsigned char *
do_button (unsigned char *s, int raw)
{
	unsigned int button;

	if (*s && get_button_code(&s, &button, 1, raw, ".\r")) {
		if (raw) {
			input_append_code(IR_INTERNAL, button);
		} else {
			struct wait_queue *wq = NULL;
			unsigned int hold_time = 5;
			button &= ~0xc0000000;
			if (button <= 0xf && button != IR_KNOB_LEFT)
				button &= ~1;
			if (s[0] == '.' && s[1] == 'L')
				hold_time = HZ+(HZ/5);
			sleep_on_timeout(&wq, HZ/25);
			input_append_code (IR_INTERNAL, button);
			if (button != IR_KNOB_LEFT && button != IR_KNOB_RIGHT) {
				sleep_on_timeout(&wq, hold_time);
				input_append_code (IR_INTERNAL, RELEASECODE(button));
			}
		}
	}
	schedule(); // give the music player a chance to run
	return s;
}

int
hijack_do_command (void *sparms, char *buf)
{
	int		rc = 0;
	char		*nextline = buf;
	server_parms_t	*parms = sparms;	// kftpd passes us NULL

	while (!rc && *nextline) {
		int nocache = 1;
		unsigned char c, *s;
		for (s = nextline; (c = *nextline); ++nextline) {
			if (c == '+') {
				*nextline = ' ';
			} else if (c == '\n' || c == ';' || c == '&') {
				*nextline++ = '\0';
				break;
			}
		}
		khttpd_fix_hexcodes(s);		// process %xx escapes
		if (!strxcmp(s, "IGNORE=", 1)) {
			parms->nocache = 1;
			break;
		}
		if (!parms || parms->auth == auth_full) {
			if (!strxcmp(s, "BUTTON=", 1)) {
				s = do_button(s+7, 0);
				goto next;
			} else if (!strxcmp(s, "BUTTONRAW=", 1)) {
				s = do_button(s+10, 1);
				goto next;
			} else if (!strxcmp(s, "RW", 0)) {
				remount_drives(1);
				goto next;
			} else if (!strxcmp(s, "RO", 0)) {
				remount_drives(0);
				sys_sync();
				goto next;
			} else if (!strxcmp(s, "REBOOT", 0)) {
				remount_drives(0);
				sys_sync();
				hijack_reboot = 1;
				goto next;
			} else if (!strxcmp(s, "POPUP ", 1) && *(s += 6)) {
				int secs;
				if (get_number(&s, &secs, 10, NULL) && *s++ == ' ' && *s)
					show_message(s, secs * HZ);
				else
					rc = -EINVAL;
				goto next;
			} else if (!strxcmp(s, "VOLADJLOW=", 1) || !strxcmp(s, "VOLADJMED=", 1) || !strxcmp(s, "VOLADJHIGH=", 1)) {
				rc = hijack_get_set_option(&s);
				if (!rc)
					hijack_set_voladj_parms();
				goto next;
			} else if (!strxcmp(s, "SERIAL=", 1) && *(s += 7)) {
				hijack_serial_rx_insert(s, strlen(s), 1);
				hijack_serial_rx_insert("\n", 1, 1);
				goto next;
			}
		}
		if (!parms) {
			rc = -EINVAL;
		} else {
			nocache = 0;
			if (!strxcmp(s, "NODATA", 0)) {
				parms->nodata = 1;
			} else if (!strxcmp(s, "FID=", 1)) {
				sprintf(parms->cwd, "/empeg/fids0/%s", s+4);
			} else if (!strxcmp(s, "OFFSET=", 1)) {
				unsigned char *t = s + 7;
				get_number(&t, &parms->offset, 10, NULL);
			} else if (!strxcmp(s, "COUNT=", 1)) {
				unsigned char *t = s + 6;
				get_number(&t, &parms->count, 10, NULL);
			} else if (!strxcmp(s, "STYLE=", 1) && *(s += 6) && strlen(s) < sizeof(parms->style)) {
				strcpy(parms->style, s);
			} else if (strxcmp(s, "EXT=", 1) || !*(s += 4)) {
				rc = -EINVAL;
			} else {
				if (!strxcmp(s, ".html", 0) || !strxcmp(s, ".htm", 0)) {
					parms->generate_playlist = html;
				} else if (!strxcmp(s, ".m3u", 0)) {
					parms->generate_playlist = m3u;
				} else if (!strxcmp(s, ".xml", 0)) {
					parms->generate_playlist = xml;
				} else {
					// Just ignore it:  .mp3, .wma, .wav, .ogg, .flac, ...
				}
			}
		}
	next:
		if (nocache && parms)
			parms->nocache = 1;
	}
	return rc;
}

/////////////////////////////////////////////////////////////////////////////////
//
// In order to make FTP workable without needless error messages, the
// following minimum implementation is required for all servers:
//
//	TYPE - ASCII Non-print
//	MODE - Stream
//	STRUCTURE - File, Record
//	COMMANDS - USER, QUIT, PORT, RETR, STOR, NOOP.
//	    and  - TYPE, MODE, STRU (for at least the default values)
//
// The default values for transfer parameters are:
//
//         TYPE - ASCII Non-print
//         MODE - Stream
//         STRU - File
//
// All hosts must accept the above as the standard defaults.
//
// In addition, we also need:  NLST, PWD, CWD, CDUP, MKD, RMD, DELE, SYST, and maybe PASV.
// The ABOR command Would Be Nice.  Plus, the UMASK, CHMOD, and EXEC extensions (below).
// Apparently Win2K clients still use the (obsolete) RFC775 "XMKD" command as well.
//
// Would-Be-Nice non-standard UNIX extensions for "SITE" command (wu-ftpd, ncftp):
//
//	Request	Description
//	UMASK	change umask. Eg. SITE UMASK 002
//	CHMOD	change mode of a file. Eg. SITE CHMOD 755 filename
//	UTIME	set timestamps. Eg. SITE UTIME file 20011104031719 20020113190634 20020113190634 UTC
//		  --> timestamps are: mtime/atime/ctime (I think), formatted as: YYYYMMDDHHMMSS
//	HELP	give help information. Eg. SITE HELP
//	NEWER	list files newer than a particular date
//	MINFO	like SITE NEWER, but gives extra information
//	GPASS	give special group access password. Eg. SITE GPASS bar
//	EXEC	execute a program.	Eg. SITE EXEC program params
//		  --> output should be captured (pipe) and returned using a set of "200-" responses
//	SIZE	returns status 213 and bytecount of the specified file
//
// FIXME: add "MDTM 20030130102437 <pathname>"   (Modify Date TiMe command, used by ncftp)
//
/////////////////////////////////////////////////////////////////////////////////

static response_t simple_response_table[] = {
	{200,	"\1TYPE "},
	{502,	"\0PASV"},
	{215,	"\0SYST"},
	{200,	"\0MODE S"},
	{200,	"\0STRU F"},
	{200,	"\0NOOP"},
	{0,	"\0\xff\xf4"},
	{226,	"\0\xf2" "ABOR"},
	{226,	"\0ABOR"},
	{221,	"\0QUIT"},
	{216,	"\0HELP SITE"},
	{216,	"\0SITE HELP"},
	{214,	"\0HELP"},
	{200,	"\0"},
	{0,	NULL}};

static int
kftpd_handle_command (server_parms_t *parms)
{
	response_t	*r;
	unsigned int	response = 0;
	int		n, quit = 0, bufsize = (sizeof(parms->buf) / 2) - 1;
	char		*path = parms->tmp2, *buf = parms->buf, *rnfr_name = parms->buf + bufsize + 1;

	if (bufsize > 511)
		bufsize = 511;	// limit path lengths, so we can use the other half for other stuff
	n = ksock_rw(parms->clientsock, buf, bufsize, 0);
	if (n < 0) {
		if (parms->verbose && !hijack_silent)
			printk(KFTPD": ksock_rw() failed, rc=%d\n", n);
		return -1;
	} else if (n == 0) {
		if (parms->verbose && !hijack_silent)
			printk(KFTPD": EOF on client sock\n");
		return -1;
	}
	if (n >= bufsize)
		n = bufsize;
	buf[n] = '\0';
	if (buf[n - 1] == '\n')
		buf[--n] = '\0';
	if (buf[n - 1] == '\r')
		buf[--n] = '\0';
	if (parms->verbose && !hijack_silent)
		printk(KFTPD": \"%s\"\n", buf);

	// first look for commands that have hardcoded reponses:
	for (r = simple_response_table; r->text; ++r) {
		if (!strxcmp(buf, &(r->text[1]), r->text[0])) {
			response = r->rcode;
			if (response == 221)
				quit = 1;
			goto got_response;
		}
	}
	schedule(); // give the music player a chance to run

	// now look for commands involving more complex handling:
	if (!strxcmp(buf, "USER ", 1)) {
		response = parms->need_password ? 331 : 230;
	} else if (!strxcmp(buf, "PASS ", 1) && buf[5]) {
		if (!parms->need_password || !strcmp(&buf[5], hijack_kftpd_password)) {
			parms->need_password = 0;
			response = 230;	// password okay, ACCT info not required
		} else {
			response = 530; // login incorrect
		}
	} else if (parms->need_password) {
		response = 530;	// login incorrect
	} else if (!strxcmp(buf, "LIST", 1) || !strxcmp(buf, "NLST", 1)) {
		int j = 4;
		parms->show_dotfiles = hijack_kftpd_show_dotfiles;
		if (buf[j] == ' ' && buf[j+1] == '-') {
			while (buf[++j] && buf[j] != ' ') {
				if (buf[j] == 'a')
					parms->show_dotfiles = 1;
			}
		}
		if (buf[j] && buf[j] != ' ') {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			if (buf[j]) {
				buf[j] = '\0';
				append_path(path, &buf[j+1], parms->tmp3);
			}
			response = send_dirlist(parms, path, buf[0] == 'L');
			if (!response)
				response = 226;
		}
	} else if (!strxcmp(buf, "CWD ", 1) && buf[4]) {
		strcpy(path, parms->cwd);
		append_path(path, &buf[4], parms->tmp3);
		if (2 != classify_path(path)) {
			response = 431;
		} else {
			strcpy(parms->cwd, path);
			quit = kftpd_dir_response(parms, 250, parms->cwd, "directory changed");
		}
	} else if (!strxcmp(buf, "CDUP", 0)) {
		append_path(parms->cwd, "..", parms->tmp3);
		quit = kftpd_dir_response(parms, 200, parms->cwd, NULL);
	} else if (!strxcmp(buf, "PWD", 0)) {
		quit = kftpd_dir_response(parms, 257, parms->cwd, NULL);
	} else if (!strxcmp(buf, "SITE CHMOD ", 1)) {
		unsigned char *p = &buf[11];
		int mode;
		if (!get_number(&p, &mode, 8, " ") || !*p++ || !*p) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, p, parms->tmp3);
			response = kftpd_do_chmod(parms, mode, path);
		}
	} else if (!strxcmp(buf, "SITE EXEC ", 1)) {
		unsigned char *p = &buf[10];
		while (*p == ' ')
			++p;
		if (!*p) {
			response = 501;
		} else {
			extern int hijack_exec(char *cwd, char *cmdline);
			int rc;
			// prefix the user's command with "exec":
			p = &buf[5];
			p[0] = 'e';
			p[1] = 'x';
			p[2] = 'e';
			p[3] = 'c';
			if (0 == (rc = hijack_exec(parms->cwd, p))) {
				response = 200;
			} else {
				char text[32];
				sprintf(text, " ERROR (rc=%d)", rc);
				kftpd_send_response2(parms, 541, text, "");
				response = 0;
			}
		}
	} else if (!strxcmp(buf, "SITE ", 1)) {
		response = hijack_do_command(NULL, &buf[5]) ? 541 : 200;
	} else if (!strxcmp(buf, "PORT ", 1)) {
		parms->have_portaddr = 0;
		if (extract_portaddr(&parms->portaddr, &buf[5])) {
			response = 501;
		} else {
			parms->have_portaddr = 1;
			response = 200;
		}
	} else if (!strxcmp(buf, "MKD ", 1) || !strxcmp(buf, "XMKD ", 1)) {
		unsigned char *p = &buf[4 + (buf[3] != ' ')];
		if (!*p) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, p, parms->tmp3);
			response = kftpd_do_mkdir(parms, path);
		}
	} else if (!strxcmp(buf, "RMD ", 1) || !strxcmp(buf, "XRMD ", 1)) {
		unsigned char *p = &buf[4 + (buf[3] != ' ')];
		if (!*p) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, p, parms->tmp3);
			response = kftpd_do_rmdir(parms, path);
		}
	} else if (!strxcmp(buf, "DELE ", 1)) {
		if (!buf[5]) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[5], parms->tmp3);
			response = kftpd_do_delete(parms, path);
		}
	} else if (!strxcmp(buf, "RNFR ", 1)) {
		if (!buf[5]) {
			response = 501;
		} else {
			parms->rename_pending = 2;
			strcpy(rnfr_name, parms->cwd);
			append_path(rnfr_name, &buf[5], parms->tmp3);
			response = 350;
		}
	} else if (!strxcmp(buf, "RNTO ", 1)) {
		if (!buf[5]) {
			response = 501;
		} else if (!parms->rename_pending) {
			response = 503;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[5], parms->tmp3);
			response = sys_rename(rnfr_name, path) ? 451 : 250;
		}
	} else if (!strxcmp(buf, "REST ", 1)) {
		unsigned char *p = &buf[5];
		int offset;
		if (!get_number(&p, &offset, 10, "\r\n") || *p) {
			response = 501;
		} else {
			parms->start_offset = offset;
			response = 350;
		}
	} else if (!strxcmp(buf, "RETR ", 1) || !strxcmp(buf, "STOR ", 1)) {
		if (!buf[5]) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[5], parms->tmp3);
			if (buf[0] == 'R')
				response = send_file(parms, path);
			else
				response = receive_file(parms, path);
			if (!response)
				response = 226;
		}
		parms->start_offset = 0;
	} else if (!strxcmp(buf, "SIZE ", 1)) {
		struct stat st;
		if (!buf[5]) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[5], parms->tmp3);
			if (0 > sys_newstat(path, &st)) {
				response = 550;
			} else {
				char text[32];
				sprintf(text, " %lu", (unsigned long)(st.st_size));
				kftpd_send_response2(parms, 213, text, "");
				response = 0;
			}
		}
	} else {
		response = 500;
	}
got_response:
	if (parms->rename_pending)
		--parms->rename_pending;
	if (response)
		kftpd_send_response(parms, response);
	return quit;
}

static const unsigned char base64[64] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void
decode_base64 (const char *s, char *result, int maxlen)
{
	char c, tmp = 0, *end = result + maxlen - 1;
	unsigned int state = 0;

	while ((c = *s++) != '\0' && c != '\r' && c != '\n') {
		unsigned b64 = 64;

		while (c != base64[--b64] && b64);
		switch (state) {
			case 0:
				tmp = b64;
				state = 1;
				break;
			case 1:
				*result++ = (tmp << 2) | (b64 >> 4);
				tmp = b64 & 0xf;
				state = 2;
				break;
			case 2:
				*result++ = (tmp << 4) | (b64 >> 2);
				tmp = b64 & 0x3;
				state = 3;
				break;
			case 3:
				*result++ = (tmp << 6) | (b64);
				state = 0;
				break;
		}
		if (result >= end)
			break;
	}
	*result = '\0';
}

static void
khttpd_handle_connection (server_parms_t *parms)
{
	unsigned char	*buf = parms->buf, *cmds = NULL, c, *path, *p, *x;
	int		buflen = sizeof(parms->buf) - 1, pathlen;
	int		size = 0, use_index = 1;	// look for index.html
	const http_response_t *response = NULL;

	parms->show_dotfiles = hijack_khttpd_show_dotfiles;
	strcpy(parms->style, hijack_khttpd_style);

	do {
		int rc = ksock_rw(parms->clientsock, buf+size, buflen-size, 0);
		if (rc <= 0) {
			if (parms->verbose && !hijack_silent)
				printk(KHTTPD": receive failed: %d\n", rc);
			return;
		}
		size += rc;
		if (size >= buflen) {
			khttpd_respond(parms, 414, "Request-URI Too Long", "POST not allowed");
			return;
		}
	} while (size < 5 || buf[size-1] != '\n' || (buf[size-2] != '\n' && buf[size-3] != '\n'));
	buf[size] = '\0';
	if (parms->verbose > 1 && !hijack_silent)
		printk(KHTTPD": request_header = \"%s\"\n", buf);
	if (!strxcmp(buf, "GET ", 1) && buf[4]) {
		path = buf + 4;
	} else if (!strxcmp(buf, "HEAD ", 1) && buf[5]) {
		parms->method_head = 1;
		path = buf + 5;
	} else {
		khttpd_respond(parms, 405, "Method Not Allowed", "Server only supports GET");
		return;
	}
	p = path;
	// find path delimiter
	while ((c = *p) && c != ' ' && c != '\r' && c != '\n')
		++p;
	// Look for some HTTP header options
	for (x = p; *x; ++x) {
		static const char Range[] = "\nRange: bytes=";
		static const char Host[] = "\nHost: ";
		static const char Auth[] = "\nAuthorization: Basic ";
		if (*x == '\n') {
			if (!strxcmp(x, "\nUser-Agent: Mozilla", 1)) {
				parms->is_mozilla = 1;
			} else if (!strxcmp(x, "\nUser-Agent: NSPlayer", 1) || !strxcmp(x, "\nUser-Agent: Windows-Media-Player", 1) || !strxcmp(x, "\nUser-Agent: iTunes", 1)) {
				parms->streaming = 1;
			} else if (!strxcmp(x, "\nIcy-MetaData:1", 1)) {
				parms->streaming = 1;
				parms->icy_metadata = 1;
			} else if (!strxcmp(x, Auth, 1)) {
				char *user_passwd = x + sizeof(Auth) - 1;	// "user:passwd" in base64 encoding
				x = user_passwd;
				while ((c = *x) && c != ' ' && c != '\r' && c != '\n')
					++x;
				*x = '\0';
				decode_base64(user_passwd, parms->user_passwd, sizeof(parms->user_passwd) - 1);	// (-1 to allow for '@' append)
				if (parms->user_passwd[0]) {
					if (*hijack_khttpd_full && 0 == strcmp(parms->user_passwd, hijack_khttpd_full))
						parms->auth = auth_full;
					else if (*hijack_khttpd_basic && 0 == strcmp(parms->user_passwd, hijack_khttpd_basic))
						parms->auth = auth_basic;
					else
						parms->user_passwd[0] = '\0';
					strcat(parms->user_passwd, "@");
				}
				*x = c;
			} else if (!strxcmp(x, Host, 1) && *(x += sizeof(Host) - 1)) {
				unsigned char *h = x;
				while ((c = *x) && c != '\n' && c != '\r')
					++x;
				if ((x - h) < sizeof(parms->hostname)) {
					c = *x;
					*x = '\0';
					strcpy(parms->hostname, h);
					*x-- = c;
				}
			} else if (!strxcmp(x, Range, 1) && *(x += sizeof(Range) - 1)) {
				int start = 0, end = -1;
				if (*x != '-' && (!get_number(&x, &start, 10, "-") || start < 0))
					break;
				if (*x == '-') {
					if ((c = *++x) == '\0' || c == '\r' || c == '\n' || (get_number(&x, &end, 10, "\r\n") && end >= start)) {
						parms->start_offset = start;
						parms->end_offset   = end;
					}
				}
				break;
			}
		}
		schedule(); // give the music player a chance to run
	}
	*p = '\0'; // zero-terminate the GET line
	if (parms->verbose && !hijack_silent)
		printk(KHTTPD": GET \"%s\"\n", path);
	if (khttpd_check_auth(parms, auth_basic))
		return;
	if ((parms->auth == auth_basic && (!*path || !strcmp(path, "/"))) || !strxcmp(path, "/?playlists", 1)) {
		khttpd_redirect(parms, "/?FID=101&EXT=.htm", "");
		return;
	}
	for (p = path; *p; ++p) {
		if (*p == '?') {
			cmds = p + 1;
			*p = '\0';	// zero-terminate the path portion
			break;
		}
	}
	// move path to an alternate buffer so we can edit/replace it later, if needed
	khttpd_fix_hexcodes(strcpy(parms->cwd, path));
	path = parms->cwd;
	if (cmds && *cmds) {
		if (hijack_do_command(parms, cmds)) {
			response = &access_not_permitted;
			goto quit;
		}
		if (parms->generate_playlist) {
			if (!hijack_glob_match(path, "/empeg/fids?/??*1"))
				response = &invalid_playlist_path;
			else if (!*path)
				response = &(http_response_t){400, "Missing Pathname"};
			else 
				response = send_playlist(parms, path);
			goto quit;
		}
		if (parms->nodata) {
			const char r204[] = "HTTP/1.1 204 No Data\r\nConnection: close\r\n\r\n";
			ksock_rw(parms->clientsock, r204, sizeof(r204)-1, -1);
			return;
		}
	}
	if (!(pathlen = strlen(path))) {
		response = &(http_response_t){400, "Missing Pathname"};
	} else if (path[pathlen-1] == '/' && (!use_index || !strcpy(path+pathlen, hijack_khttpd_root_index) || 1 != classify_path(path))) {
		path[pathlen] = '\0';	// remove the index.html path suffix
		if (khttpd_check_auth(parms, auth_full))
			return;
		response = convert_rcode(send_dirlist(parms, path, 1));
	//} else if (!get_mime_type(path, NULL) && !parms->streaming && khttpd_check_auth(parms, auth_full)) {
	} else if (!parms->streaming && khttpd_check_auth(parms, auth_full)) {
		return;
	} else {
		response = convert_rcode(send_file(parms, path));
	}
quit:
	if (response)
		khttpd_respond(parms, response->rcode, response->rtext, buf);
}

static int
get_ipaddr (struct socket *sock, char *ipaddr, int peer)	// peer: 0=local, 1=remote
{
	int rc, len;
	struct sockaddr_in addr;

	if ((rc = sock->ops->getname(sock, (struct sockaddr *)&addr, &len, peer)))
		return rc;
	if (!inet_ntop2(&addr, ipaddr))
		return -EINVAL;
	return 0;
}

static int
ksock_accept (server_parms_t *parms)
{
	int rc;

	if ((parms->clientsock = sock_alloc())) {
		parms->clientsock->type = parms->servsock->type;
		if (parms->servsock->ops->dup(parms->clientsock, parms->servsock) < 0) {
			if (!hijack_silent)
				printk("%s: sock_accept: dup() failed\n", parms->servername);
		} else if ((rc = parms->clientsock->ops->accept(parms->servsock, parms->clientsock, parms->servsock->file->f_flags)) < 0) {
			if (!hijack_silent)
				printk("%s: sock_accept: accept() failed, rc=%d\n", parms->servername, rc);
		} else if (get_ipaddr(parms->clientsock, parms->clientip, 1) || get_ipaddr(parms->clientsock, parms->hostname, 0)) {
			if (!hijack_silent)
				printk("%s: sock_accept: get_ipaddr() failed\n", parms->servername);
		} else {
			return 0;	// success
		}
		sock_release(parms->clientsock);
	}
	return 1;	// failure
}

asmlinkage int sys_chdir(const char *);
int get_fd(struct inode *inode);	// net/socket.c

static int
ktelnetd_handle_connection (server_parms_t *parms)
{
	static char shell[] = "/bin/bash";
	static char *envp[] = { "HOME=/", "TERM=ansi", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL };
	char *argv[] = { shell, "--login", "-i", NULL };
	int sockfd, errno;

	// we don't need the original server socket here
	//close(parms->servsock);  /* not a file descriptor */

	// Allow syscall args to come from kernel space
	set_fs(KERNEL_DS);

	// cd to home directory
	(void)sys_chdir("/");

	// set up stdin,stdout,stderr to all point at the socket
	sockfd = get_fd(parms->clientsock->inode);
	dup(sockfd);
	dup(sockfd);

	// toss garbage (unsupported protocol leftovers) from client side
	read(sockfd, parms->buf, sizeof(parms->buf));

	// free the client parms structure now that we're done with it
	free_pages((unsigned long)parms, 1);

	// launch the shell.  FIXME: do this on a pty someday, to get job control goodies working
	errno = execve(shell, argv, envp);	// never returns
	if (!hijack_silent)
		printk(KERN_ERR "ktelnetd_handle_connection: failed, errno = %d\n", errno);
	return -errno;
}

static int
child_thread (void *arg)
{
	server_parms_t	*parms = arg;

	if (parms->verbose && !hijack_silent)
		printk("%s: %s connection from %s\n", parms->servername, parms->hostname, parms->clientip);
	(void) set_sockopt(parms, parms->servsock, SOL_TCP, TCP_NODELAY, 1); // don't care
	switch (parms->protocol) {
		case kftpd:
			if (!kftpd_send_response(parms, 220)) {
				strcpy(parms->cwd, "/");
				parms->umask = 0022;
				while (!kftpd_handle_command(parms));
				sync();	// useful for flash upgrades
			}
			break;
		case khttpd:
			khttpd_handle_connection(parms);
			break;
		case ktelnetd:
			ktelnetd_handle_connection(parms);
			break;
	}
#if 1
	sock_release(parms->clientsock);
#else
	parms->clientsock->ops->shutdown(parms->clientsock, 2); // SHUT_RDWR
#endif
	free_pages((unsigned long)parms, 1);
	sys_exit(0);	// never returns
	return 0;
}

static int
kxxxd_daemon (void *protocolp)	// invoked thrice on startup
{
	server_parms_t		parms, *clientparms = NULL;
	int			server_port, protocol = (int)(long)protocolp;
	extern unsigned long	sys_signal(int, void *);
	unsigned int		flags = CLONE_FS | CLONE_FILES | CLONE_SIGHAND;

	if (sizeof(server_parms_t) > PAGE_SIZE) {	// we allocate client parms as single pages
		if (!hijack_silent)
			printk("%s: ERROR: parms too large (%u)\n", __FUNCTION__, sizeof(server_parms_t));
		return 0;
	}
	memset(&parms, 0, sizeof(parms));

	parms.end_offset = -1;
	parms.protocol = protocol;
	if (*hijack_kftpd_password)
		parms.need_password = 1;

	switch (protocol) {
		case kftpd:
			parms.servername = KFTPD;
			server_port	= hijack_kftpd_control_port;
			parms.data_port	= hijack_kftpd_data_port;
			parms.verbose	= hijack_kftpd_verbose;
			break;
		case khttpd:
			parms.servername = KHTTPD;
			server_port	= hijack_khttpd_port;
			parms.verbose	= hijack_khttpd_verbose;
			break;
		case ktelnetd:
			parms.servername = KTELNETD;
			server_port	= hijack_ktelnetd_port;
			parms.verbose	= 1;
			flags		= CLONE_FS | CLONE_SIGHAND;;
			break;
		default:
			return 0;
	}

	// kthread setup
	set_fs(KERNEL_DS);
	current->session = 1;
	current->pgrp = 1;

	strcpy(current->comm, parms.servername);
	sigfillset(&current->blocked);

	// Prevent starvation due to player disk I/O
	current->rt_priority = 50;
	current->policy = SCHED_RR;

	if (server_port && hijack_max_connections > 0) {
		if (make_socket(&parms, &parms.servsock, server_port)) {
			if (!hijack_silent)
				printk("%s: make_socket(port=%d) failed\n", parms.servername, server_port);
		} else if (parms.servsock->ops->listen(parms.servsock, 10) < 0) {	// queued=10
			if (!hijack_silent)
				printk("%s: listen(port=%d) failed\n", parms.servername, server_port);
		} else {
			int childcount = 0;
			if (!hijack_silent)
				printk("%s: listening on port %d\n", parms.servername, server_port);
			while (1) {
				int child;
				do {
					int status, flags = WUNTRACED | __WCLONE;
					if (childcount < hijack_max_connections)
						flags |= WNOHANG;
					child = sys_wait4(-1, &status, flags, NULL);
					if (child > 0)
						--childcount;
				} while (child > 0);
				if (!ksock_accept(&parms)) {
					if (!(clientparms = (server_parms_t *)__get_free_pages(GFP_KERNEL,1))) {
						if (!hijack_silent)
							printk("%s: no memory for client parms\n", parms.servername);
						sock_release(parms.clientsock);
					} else {
						memcpy(clientparms, &parms, sizeof(parms));
						if (0 < kernel_thread(child_thread, clientparms, flags))
							++childcount;
					}
				}
			}
		}
	}
	return 0;
}

int
kxxxd_starter (unsigned long arg)	// invoked once on startup
{
	down(&hijack_kxxxd_startup_sem);	// wait for Hijack to get our port numbers from config.ini
	if (hijack_kftpd_control_port)
		kernel_thread(kxxxd_daemon, (void *)0, CLONE_FS | CLONE_FILES | CLONE_SIGHAND); // kftpd
	if (hijack_khttpd_port)
		kernel_thread(kxxxd_daemon, (void *)1, CLONE_FS | CLONE_FILES | CLONE_SIGHAND); // khttpd
	if (hijack_ktelnetd_port)
		kernel_thread(kxxxd_daemon, (void *)2, CLONE_FS | CLONE_FILES | CLONE_SIGHAND); // ktelnetd
	return menuexec_daemon(NULL);
}
