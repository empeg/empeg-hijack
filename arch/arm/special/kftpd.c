// kftpd 0.2 by Mark Lord
//
// This version can only LIST directories (buggy), and RETRieve files.  STOR is not implemented yet.

#if 0
#define SERVER_CONTROL_PORT	21
#define SERVER_DATA_PORT	20
#else
#define SERVER_CONTROL_PORT	91	// Eg.  ftp  10.0.0.24  91
#define SERVER_DATA_PORT	90
#endif

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/socket.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/firewall.h>
#include <linux/wanrouter.h>
#include <linux/init.h>
#include <linux/poll.h>

#if defined(CONFIG_KMOD) && defined(CONFIG_NET)
#include <linux/kmod.h>
#endif

#include <asm/uaccess.h>

#include <linux/inet.h>
#include <net/ip.h>
#include <net/sock.h>
#include <net/rarp.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <net/scm.h>

#define INET_ADDRSTRLEN	16


// This  function  converts  the  character string src
// into a network address structure in the af address family,
// then copies the network address structure to dst.
//
static int
inet_pton (int af, unsigned const char *src, void *dst)
{
	unsigned char	*d = dst;
	int		i;

	if (af != AF_INET)
		return -EAFNOSUPPORT;
	for (i = 3; i >= 0; --i) {
		unsigned int val = 0, digits = 0;
		while (*src >= '0' && *src <= '9') {
			val = (val * 10) + (*src++ - '0');
			++digits;
		}
		if (!digits || val > 255 || (i && *src++ != '.'))
			return 0;
		*d++ = val;
	}
	return 1;
}

// This  function  converts  the  network  address structure src
// in the af address family into a character string, which is copied
// to a character buffer dst, which is cnt bytes long.
//
static const char *
inet_ntop (int af, const void *src, char *dst, size_t cnt)
{
	unsigned const char *s = src;

	if (af != AF_INET || cnt < INET_ADDRSTRLEN)
		return NULL;
	sprintf(dst, "%u.%u.%u.%u", s[0], s[1], s[2], s[3]);
	return dst;
}

static struct sockaddr_in	portaddr;
static int			have_portaddr = 0;
static int			binary_mode = 1;	// fixme

//#define skip_atoi(sp) (strtol((*(sp)),(sp),10))

static int
extract_portaddr (struct sockaddr_in *addr, char *s)
{
	extern int	skip_atoi(char **s);	// from lib/vsprintf.c
	char		*first = s;
	int		dots = 0;

	memset(addr, 0, sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	while (*s && dots < 4) {
		if (*s == ',') {
			*s = '.';
			++dots;
		}
		++s;
	}
	*(s - 1) = '\0';
	if (inet_pton(AF_INET, first, &addr->sin_addr) > 0) {
		unsigned short port = (skip_atoi(&s) & 255) << 8;
		if (port && *s++) {
			port += skip_atoi(&s) & 255;
			if (port & 255) {
				addr->sin_port = htons(port);
				printk("kftpd: destination addr = %s:%d\n", first, port);
				return 0;
			}
		}
	}
	printk("kftp: bad destination addr\n");
	return -1;
}

// Adapted from net/socket.c::sys_accept()
struct socket *
ksock_accept (struct socket *sock, struct sockaddr *address, int *len)
{
	struct socket *newsock;

	lock_kernel();
	if (!(newsock = sock_alloc())) {
		printk("kftp: sock_accept: sock_alloc() failed\n");
	} else {
		newsock->type = sock->type;
		if (sock->ops->dup(newsock, sock) < 0) {
			printk("kftp: sock_accept: dup() failed\n");
		} else if (newsock->ops->accept(sock, newsock, sock->file->f_flags) < 0) {
			printk("kftp: sock_accept: accept() failed\n");
		} else if (!address || newsock->ops->getname(newsock, (struct sockaddr *)address, len, 1) >= 0) {
			unlock_kernel();
			return newsock;		// success
		}
		sock_release(newsock);
	}
	unlock_kernel();
	return NULL;	// failure
}

// Adapted from various examples in the kernel
static int
ksock_rw (int have_lock, struct socket *sock, char *buf, int bufsize, int minimum)
{
	struct msghdr	msg;
	struct iovec	iov;
	mm_segment_t	oldfs = 0;
	int		bytecount = 0, sending = 0;

	if (!have_lock) {
		lock_kernel();
		oldfs = get_fs();
		set_fs(KERNEL_DS);
	}
	if (minimum < 0) {
		minimum = bufsize;
		sending = 1;
	}

	do {
		int rc, len = bufsize - bytecount;

		iov.iov_base       = &buf[bytecount];
		iov.iov_len        = len;
		msg.msg_name       = NULL;
		msg.msg_namelen    = 0;
		msg.msg_iov        = &iov;
		msg.msg_iovlen     = 1;
		msg.msg_control    = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags      = 0;

		if (sending)
			rc = sock_sendmsg(sock, &msg, len);
		else
			rc = sock_recvmsg(sock, &msg, len, 0);
		if (rc <= 0)	// fixme?  (len < 0) ??
			break;
		bytecount += rc;
	} while (bytecount < minimum);

	if (!have_lock) {
		set_fs(oldfs);
		unlock_kernel();
	}
	return bytecount;
}

static int
send_response (struct socket *sock, const char *response)
{
	char	buf[256];
	int	length, rc;

	//printk("kftp: sending: %s\n", response);
	strcpy(buf, response);
	strcat(buf, "\r\n");
	length = strlen(buf);
	if ((rc = ksock_rw(0, sock, buf, length, -1)) != length) {
		printk("kftp: ksock_rw(response) '%s' failed: %d\n", response, rc);
		return -1;
	}
	return 0;
}

static int
make_socket (struct socket **sockp, int port)
{
	int			rc, turn_on = 1;
	struct sockaddr_in	servaddr;
	struct socket		*sock;

	*sockp = NULL;
	rc = sock_create(AF_INET, SOCK_STREAM, 0, &sock);
	if (rc) {
		printk("kftp: sock_create() failed, rc=%d\n", rc);
	} else {
		rc = sock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&turn_on, sizeof(turn_on));
		if (rc) {
			printk("kftp: setsockopt() failed, rc=%d\n", rc);
			sock_release(sock);
		} else {
			memset(&servaddr, 0, sizeof(servaddr));
			servaddr.sin_family	 = AF_INET;
			servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
			servaddr.sin_port        = htons(port);
			rc = sock->ops->bind(sock, (struct sockaddr *)&servaddr, sizeof(servaddr));
			if (rc) {
				printk("kftp: bind(port=%d) failed: %d\n", port, rc);
				sock_release(sock);
			} else {
				*sockp = sock;
			}
		}
	}
	return rc;
}

static const char *
open_datasock (struct socket *sock, struct socket **newsock)
{
	int		rc;
	struct socket	*datasock = NULL;
	const char	*response = NULL;

	if (!have_portaddr) {
		response = "425 no PORT specified";
	} else {
		have_portaddr = 0;	// for next time
		if ((rc = make_socket(&datasock, SERVER_DATA_PORT))) {
			response = "425 make_socket(datasock) failed";
		} else {
			if (send_response(sock, "150 opening BINARY mode data connection")) {
				response = "451 error";
			} else {
				int flags = 0;
				rc = datasock->ops->connect(datasock, (struct sockaddr *)&portaddr,
								sizeof(struct sockaddr_in), flags);
				if (rc)
					response = "425 data connection failed";
			}
			if (response)
				sock_release(datasock);
			else
				*newsock = datasock;
		}
	}
	return response;
}

static const char *
send_file (struct socket *sock, const char *path)
{
	unsigned int	size;
	struct file	*filp;
	unsigned char	buf[PAGE_SIZE];
	mm_segment_t	old_fs;
	struct socket	*datasock;
	const char	*response = NULL;

	lock_kernel();
	filp = filp_open(path,O_RDONLY,0);
	if (IS_ERR(filp) || !filp) {
		printk("kftp: filp_open(%s) failed\n", path);
		response = "550 file not found";
	} else {
		if (!filp->f_dentry || !filp->f_dentry->d_inode || !filp->f_op || !filp->f_op->read) {
			response = "550 file read error";
		} else if (!(response = open_datasock(sock, &datasock))) {
			filp->f_pos = 0;
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			do {
				size = filp->f_op->read(filp, buf, sizeof(buf), &(filp->f_pos));
				if (size < 0) {
					printk("kftp: filp->f_op->read() failed; rc=%d\n", size);
					response = "451 error reading file";
				} else if (size && size != ksock_rw(1, datasock, buf, size, -1)) {
					response = "426 error sending data; transfer aborted";
					break;
				}
			} while (size > 0);
			set_fs(old_fs);
			sock_release(datasock);
		}
		filp_close(filp,NULL);
	}
	unlock_kernel();
	return response;
}

// gmtime - convert time_t into struct tm
//
// Adapted from version found in MINIX source tree

struct tm
{
	int	tm_sec;		/* seconds	*/
	int	tm_min;		/* minutes	*/
	int	tm_hour;	/* hours	*/
	int	tm_mday;	/* day of month	*/
	int	tm_mon;		/* month	*/
	int	tm_year;	/* full year	*/
	int	tm_wday;	/* day of week	*/
	int	tm_yday;	/* days in year	*/
};

#define	EPOCH_YR		(1970)		/* Unix EPOCH = Jan 1 1970 00:00:00 */
#define	SECS_PER_HOUR		(60L * 60L)
#define	SECS_PER_DAY		(24L * SECS_PER_HOUR)
#define	IS_LEAPYEAR(year)	(!((year) % 4) && (((year) % 100) || !((year) % 400)))
#define	DAYS_PER_YEAR(year)	(IS_LEAPYEAR(year) ? 366 : 365)

const int DAYS_PER_MONTH[2][12] = {
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },	// normal year
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 } };	// leap year

static char *MONTHS[12] =
	{ "Jan", "Feb", "Mar", "Apr", "May", "Jun",
	  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static struct tm *
gmtime (time_t time, struct tm *tm)
{
	unsigned long clock, day;
	const int *days_per_month;
	int month = 0, year = EPOCH_YR;

	clock   = (unsigned long)time % SECS_PER_DAY;
	day = (unsigned long)time / SECS_PER_DAY;
	tm->tm_sec   =  clock % 60;
	tm->tm_min   = (clock % SECS_PER_HOUR) / 60;
	tm->tm_hour  =  clock / SECS_PER_HOUR;
	tm->tm_wday  = (day + 4) % 7;	/* day 0 was a thursday */
	while ( day >= DAYS_PER_YEAR(year) ) {
		day -= DAYS_PER_YEAR(year);
		year++;
	}
	tm->tm_year  = year;
	tm->tm_yday  = day;
	days_per_month = DAYS_PER_MONTH[IS_LEAPYEAR(year)];
	month          = 0;
	while ( day >= days_per_month[month] ) {
		day -= days_per_month[month];
		month++;
	}
	tm->tm_mon   = month;
	tm->tm_mday  = day + 1;
	return tm;
}

static char *
format_time (struct tm *tm, int current_year, char *buf)
{
	int		t, y;
	const char	*s;

	s = MONTHS[tm->tm_mon];
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
		*buf++ = '0' +  (y / 1000);
		*buf++ = '0' + ((y /  100) % 10);
		*buf++ = '0' + ((y /   10) % 10);
		*buf++ = '0' +  (y %   10);
	}
	*buf = '\0';
	return buf;
}

#define MODE_XBIT(c,x,s)	((x) ? ((s) ? ((c)|0x20) : 'x') : ((s) ? (c) : '-'));

typedef struct filldir_parms_s {
	int			rc;
	int			current_year;
	unsigned long		blockcount;
	struct socket		*datasock;
	struct super_block	*super;
} filldir_parms_t;

static int	// callback routine for filp->f_op->readdir()
filldir (void *parms, const char *name, int namelen, off_t offset, ino_t ino)
{
	filldir_parms_t	*p = parms;
	unsigned int	len, mode, sent;
	struct inode	*i;
	struct tm	tm;
	char		buf[512], *b, c;

	if (p->rc)
		return -EIO;
	i = iget(p->super, ino);
	if (!i) {
		printk("kftp: iget(%lu) failed\n", ino);
		return -ENOENT;
	}

	b = buf;
	mode = i->i_mode;
	switch (mode & S_IFMT) {
		case S_IFLNK:	c = 'l'; break;
		case S_IFDIR:	c = 'd'; break;
		case S_IFCHR:	c = 'c'; break;
		case S_IFBLK:	c = 'b'; break;
		case S_IFIFO:	c = 'p'; break;
		case S_IFSOCK:	c = 's'; break;
		case S_IFREG:	c = '-'; break;
		default:	c = '-'; break;
	}
	*b++ = c;
	*b++ = (mode & S_IRUSR) ? 'r' : '-';
	*b++ = (mode & S_IWUSR) ? 'w' : '-';
	*b++ = MODE_XBIT('S', mode & S_IXUSR, mode & S_ISUID);
	*b++ = (mode & S_IRGRP) ? 'r' : '-';
	*b++ = (mode & S_IWGRP) ? 'w' : '-';
	*b++ = MODE_XBIT('S', mode & S_IXGRP, mode & S_ISGID);
	*b++ = (mode & S_IROTH) ? 'r' : '-';
	*b++ = (mode & S_IWOTH) ? 'w' : '-';
	*b++ = MODE_XBIT('T', mode & S_IXOTH, mode & S_ISVTX);

	b += sprintf(b, "%5u %-8u %-8u", i->i_nlink, i->i_uid, i->i_gid);
	if (buf[0] == 'c' || buf[0] == 'b')
		b += sprintf(b, " %3u, %3u", MAJOR(i->i_rdev), MINOR(i->i_rdev));
	else
		b += sprintf(b, " %8lu", i->i_size);

	b = format_time(gmtime(i->i_mtime, &tm), p->current_year, b);

	*b++ = ' ';
	while (namelen--)
		*b++ = *name++;

	// Get target of symbolic link: UGLY HACK, COPIED FROM ext2_readlink()
	if (buf[0] == 'l' && i->i_sb) {
		len = i->i_sb->s_blocksize - 1;
		if (i->i_blocks) {
			int err;
			struct buffer_head * bh;
			bh = ext2_bread(i, 0, 0, &err);
			name = bh ? bh->b_data : NULL;
		} else {
			name = (char *) i->u.ext2_i.i_data;
		}
		if (name) {
			*b++ = ' ';
			*b++ = '-';
			*b++ = '>';
			*b++ = ' ';
			while (len-- && *name)
				*b++ = *name++;
		}
	}

	*b++ = '\r';
	*b++ = '\n';
	*b   = '\0';

	// free up the inode structure
	iput(i);
	i = NULL;

	//printk("kftp: %s", buf);
	len  = b - buf;
	sent = ksock_rw(0, p->datasock, buf, len, -1);
	if (sent != len) {
		p->rc = -EIO;
		printk("kftp: ksock_rw(%d) returned %d\n", len, sent);
	}
	return p->rc;
}

static const char *
send_dirlist (struct socket *sock, const char *path)
{
	int		rc;
	struct file	*filp;
	struct socket	*datasock;
	const char	*response = NULL;

	lock_kernel();
	filp = filp_open(path,O_RDONLY,0);
	if (IS_ERR(filp) || !filp) {
		printk("kftp: filp_open(%s) failed\n", path);
		response = "550 directory not found";
	} else {
		if (!filp->f_dentry || !filp->f_dentry->d_inode || !filp->f_op || !filp->f_op->readdir) {
			response = "550 directory read error";
		} else if (!(response = open_datasock(sock, &datasock))) {
			filldir_parms_t	p;
			struct inode	*inode = filp->f_dentry->d_inode;
			unsigned char	buf[64];
			unsigned int	len, sent;
			struct tm	tm;

			p.current_year	= gmtime(CURRENT_TIME, &tm)->tm_year;
			p.rc		= 0;
			p.blockcount	= 0;
			p.super		= inode->i_sb;
			p.datasock	= datasock;

			down(&inode->i_sem);
			filp->f_pos = 0;
			do {
				rc = filp->f_op->readdir(filp, &p, filldir);
			} while (rc >= 0 && !p.rc && filp->f_pos < inode->i_size);
			up(&inode->i_sem);

			if (p.rc) {
				printk("kftp: ksock_rw() error %d\n", p.rc);
				response = "426 ksock_rw() error";
			} else if (rc) {
				printk("kftp: readdir() returned %d\n", rc);
				response = "426 readdir() error";
			} else {
				len = sprintf(buf, "total %lu\r\n", p.blockcount);
				sent = ksock_rw(0, datasock, buf, --len, -1);
				if (sent != len) {
					printk("kftp: ksock_rw(%d) returned %d\n", len, sent);
					response = "426 ksock_rw() error 2";
				}
			}
			sock_release(datasock);
		}
		filp_close(filp,NULL);
	}
	unlock_kernel();
	return response;
}

static char cwd[256] = {'/', '\0', };

static char *
dir_response (int code, char *dir, char *suffix)
{
	static char response[256];

	if (suffix)
		sprintf(response, "%03d \"%s\" %s", code, dir, suffix);
	else
		sprintf(response, "%03d \"%s\"", code, dir);
	return response;
}

static void
append_path (char *path, char *new)
{
	char	buf[256], *b = buf, *p = path;

	if (*new == '/') {
		strcpy(buf, new);
	} else {
		strcpy(buf, path);
		strcat(buf, "/");
		strcat(buf, new);
	}
	// Now fix-up the path, resolving '..' and removing consecutive '/'
	*p++ = '/';
	while (*b) {
		while (*b == '/')
			++b;
		if (b[0] == '.' && b[1] == '.' && (b[2] == '/' || b[2] == '\0')) {
			b += 2;
			while (*--p != '/');
			if (p == path)
				++p;
		} else if (*b) { // copy simple path element
			if (*(p-1) != '/')
				*p++ = '/';
			while (*b && *b != '/')
				*p++ = *b++;
		}
	}
	*p = '\0';
}

// FIXME:  allow upper/lower/mixed case for commands
//
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
// In addition, we also need:  NLST, PWD, CWD, CDUP, MKD, RMD, DELE, and maybe SYST
// The ABOR command Would Be Nice.  Plus, the UMASK, CHMOD, and EXEC extensions (below).
//
// Non-standard UNIX extensions from wu-ftpd: "SITE EXEC", "SITE CHMOD", "SITE HELP", maybe "MINFO".
//
//	Request	Description
//	UMASK	change umask. E.g. SITE UMASK 002
//	CHMOD	change mode of a file. E.g. SITE CHMOD 755 filename
//	HELP	give help information. E.g. SITE HELP
//	NEWER	list files newer than a particular date
//	MINFO	like SITE NEWER, but gives extra information
//	GPASS	give special group access password. E.g. SITE GPASS bar
//	EXEC	execute a program.	E.g. SITE EXEC program params
//
/////////////////////////////////////////////////////////////////////////////////


// FIXME: Dunno if we have these in the kernel
#define strcasecmp	strcmp
#define strncasecmp	strncmp

static int
handle_command (struct socket *sock)
{
	char		path[256], buf[256];
	const char	*response = NULL;
	int		n, rc, quit = 0;
	const char	ABOR[] = {0xf2,'A','B','O','R','\0'};

	n = ksock_rw(0, sock, buf, sizeof(buf), 0);
	if (n < 0) {
		printk("kftp: client request too short, ksock_rw() failed: %d\n", n);
		return -1;
	} else if (n == 0) {
		printk("kftp: EOF on client sock\n");
		return -1;
	}
	if (n < 5 || n >= sizeof(buf)) {
		response = "501 bad command length";
	} else if (buf[--n] != '\n' || buf[--n] != '\r') {
		printk("kftp: EOL not found\n");
		response = "500 bad end of line";
	} else {
		buf[n] = '\0';	// overwrite '\r'
		printk("kftp: '%s'\n", buf);
		if (!strcasecmp(buf, "QUIT")) {
			quit = 1;
			response = "221 happy fishing";
		} else if (!strncasecmp(buf, "USER ", 5)) {
			response = "230 user logged in";
		} else if (!strncasecmp(buf, "PASS ", 5)) {
			response = "202 password okay";
		} else if (!strncasecmp(buf, "SYST", 4)) {
			response = "215 UNIX Type: L8";
		} else if (!strcasecmp(buf, "STRU F")) {
			response = "200 stru-f okay";
		} else if (!strncasecmp(buf, "TYPE ", 5)) {
			if (buf[5] != 'A' && buf[5] != 'I') {
				response = "501 unsupported xfer TYPE";
			} else {
				binary_mode = (buf[5] == 'I');
				response = "200 type okay";
			}
		} else if (!strncasecmp(buf, "CWD ",4)) {
			append_path(cwd, &buf[4]);
			response = dir_response(257, cwd, "directory changed");
		} else if (!strcasecmp(buf, "CDUP")) {
			append_path(cwd, "..");
			response = dir_response(257, cwd, NULL);
		} else if (!strcasecmp(buf, "PWD")) {
			response = dir_response(257, cwd, NULL);
		} else if (!strcasecmp(buf, "HELP SITE")) {
			response = "214-The following SITE commands are recognized.\r\n   CHMOD   EXEC\r\n214 done";
		} else if (!strncasecmp(buf, "SITE CHMOD ", 11)) {
			response = "500 Not implemented yet";
		} else if (!strcasecmp(buf, ABOR) || !strcasecmp(buf, "ABOR")) {
			response = "200 Aborted";
		} else if (!strncasecmp(buf, "SITE EXEC ", 10)) {
			response = "500 Not implemented yet";
		} else if (!strncasecmp(buf, "PORT ", 5)) {
			have_portaddr = 0;
			if (extract_portaddr(&portaddr, &buf[5])) {
				response = "501 bad port address";
			} else {
				have_portaddr = 1;
				response = "200 port accepted";
			}
		} else if (!strncasecmp(buf, "STOR ", 5)) {
			response = "502 upload not supported yet";
		} else if (!strncasecmp(buf, "LIST", 4)) {
			strcpy(path, cwd);
			if (buf[4] == ' ') {
				buf[4] = '\0';
				append_path(path, &buf[5]);
			}
			if (buf[4]) {
				response = "500 bad command";
			} else {
				response = send_dirlist(sock, path);
				if (!response)
					response = "226 transmission completed";
			}
		} else if (!strncasecmp(buf, "RETR ", 5)) {
			strcpy(path, cwd);
			if (!buf[5]) {
				response = "501 missing path";
			} else {
				append_path(path, &buf[5]);
				response = send_file(sock, path);
				if (!response)
					response = "226 transmission completed";
			}
		} else {
			response = "500 bad command";
		}
	}
	rc = response ? send_response(sock, response) : -1;
	return quit ? -1 : rc;
}

static void
handle_connection (struct socket *sock, struct sockaddr_in *clientaddr)
{
	char	clientip[INET_ADDRSTRLEN] = {0,};

	clientip[0] = '\0';
	if (clientaddr->sin_family != AF_INET) {
		printk("kftpd: not AF_INET: %d\n", (int)clientaddr->sin_family);
	} else if (inet_ntop(AF_INET, &clientaddr->sin_addr.s_addr, clientip, sizeof(clientip)) == NULL) {
		printk("kftpd: inet_ntop(%08x) failed\n", (uint32_t)clientaddr->sin_addr.s_addr);
	} else {
		printk("kftpd: connection from %s\n", clientip);
		if (!send_response(sock, "220 connected")) {
			strcpy(cwd, "/");
			while (handle_command(sock) == 0);
		}
	}
	sock_release(sock);
}

int kftpd (void *unused)
{
	int			rc;
	struct socket		*servsock = NULL;
	struct task_struct	*tsk = current;

	// kthread setup
	tsk->session = 1;
	tsk->pgrp = 1;
	strcpy(tsk->comm, "kftpd");
	sigfillset(&tsk->blocked);

	rc = make_socket(&servsock, SERVER_CONTROL_PORT);
	if (rc) {
		printk("kftpd: make_socket(port=%d) failed, rc=%d\n", SERVER_CONTROL_PORT, rc);
		return 0;
	}
	rc = servsock->ops->listen(servsock, 1); /* allow only one client at a time */
	if (rc < 0) {
		printk("kftpd: listen(port=%d) failed, rc=%d\n", SERVER_CONTROL_PORT, rc);
		return 0;
	}
	while (1) {
		struct socket		*clientsock;
		struct sockaddr_in	clientaddr;
		int			clientaddr_len = sizeof(clientaddr);

		clientsock = ksock_accept(servsock, (struct sockaddr *)&clientaddr, &clientaddr_len);
		if (!clientsock) {
			printk("kftpd: accept() failed\n");
		} else {
			handle_connection(clientsock, &clientaddr);
		}
	}
	return 0;
}

