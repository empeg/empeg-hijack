// kftpd by Mark Lord
//
// This version can only LIST directories and RETRieve files.  STOR is not implemented yet.
//

extern int hijack_khttpd_port;				// from arch/arm/special/hijack.c
extern int hijack_kftpd_control_port;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_data_port;			// from arch/arm/special/hijack.c
extern struct semaphore hijack_khttpd_startup_sem;	// from arch/arm/special/hijack.c
extern struct semaphore hijack_kftpd_startup_sem;	// from arch/arm/special/hijack.c

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

#define CLIENT_CWD_SIZE		512
typedef struct server_parms_s {
	struct socket		*clientsock;
	struct socket		*servsock;
	struct socket		*datasock;
	int			use_http;
	int			data_port;
	char			clientip[INET_ADDRSTRLEN];
	char			servername[8];
	int			have_portaddr;
	struct sockaddr_in	portaddr;
	unsigned char		cwd[CLIENT_CWD_SIZE];
} server_parms_t;

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
				//printk("kftpd: destination addr = %s:%d\n", first, port);
				return 0;
			}
		}
	}
	//printk("kftpd: bad destination addr\n");
	return -1;
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
		if (rc <= 0)	// fixme?  NO! (len < 0) ??
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
send_response (server_parms_t *parms, const char *response)
{
	char	buf[256];
	int	length, rc;

	strcpy(buf, response);
	strcat(buf, "\r\n");
	length = strlen(buf);
	if ((rc = ksock_rw(0, parms->clientsock, buf, length, -1)) != length) {
		printk("%s: ksock_rw(response) '%s' failed: %d\n", parms->servername, response, rc);
		return -1;
	}
	return 0;
}

static int
make_socket (struct socket **sockp, int port)
{
	int			rc, turn_on = 1;
	struct sockaddr_in	addr;
	struct socket		*sock;

	*sockp = NULL;
	if ((rc = sock_create(AF_INET, SOCK_STREAM, 0, &sock))) {
		printk("kftpd: sock_create() failed, rc=%d\n", rc);
	} else if ((rc = sock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&turn_on, sizeof(turn_on)))) {
		printk("kftpd: setsockopt() failed, rc=%d\n", rc);
		sock_release(sock);
	} else {
		memset(&addr, 0, sizeof(struct sockaddr_in));
		addr.sin_family	     = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port        = htons(port);
		rc = sock->ops->bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
		if (rc) {
			printk("kftpd: bind(port=%d) failed: %d\n", port, rc);
			sock_release(sock);
		} else {
			*sockp = sock;
		}
	}
	return rc;
}

static const char *
open_datasock (server_parms_t *parms)
{
	int		rc;
	const char	*response = NULL;

	if (parms->use_http) {
		parms->datasock = parms->clientsock;
	} else if (!parms->have_portaddr) {
		response = "425 no PORT specified";
	} else {
		parms->have_portaddr = 0;	// for next time
		if ((rc = make_socket(&parms->datasock, hijack_kftpd_data_port))) {
			response = "425 make_socket(datasock) failed";
		} else {
			if (send_response(parms, "150 opening BINARY mode data connection")) {
				response = "451 error";
			} else {
				int flags = 0;
				rc = parms->datasock->ops->connect(parms->datasock,
					(struct sockaddr *)&parms->portaddr, sizeof(struct sockaddr_in), flags);
				if (rc)
					response = "425 data connection failed";
			}
			if (response)
				sock_release(parms->datasock);
		}
	}
	return response;
}

// gmtime - convert time_t into tm_t
//
// Adapted from version found in MINIX source tree

typedef struct tm_s
{
	int	tm_sec;		/* seconds	*/
	int	tm_min;		/* minutes	*/
	int	tm_hour;	/* hours	*/
	int	tm_mday;	/* day of month	*/
	int	tm_mon;		/* month	*/
	int	tm_year;	/* full year	*/
	int	tm_wday;	/* day of week	*/
	int	tm_yday;	/* days in year	*/
} tm_t;

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

static tm_t *
gmtime (time_t time, tm_t *tm)
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
format_time (tm_t *tm, int current_year, char *buf)
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
	int			use_http;
	int			current_year;
	unsigned long		blockcount;
	struct socket		*datasock;
	struct super_block	*super;
	unsigned long		bufsize;
	unsigned long		bytecount;
	unsigned char		*buf;
} filldir_parms_t;

static char *
append_string (char *b, const char *s, int len, int quoted)
{
	while (len-- && *s) {
		if (quoted && (*s == '\\' || *s == '"'))
			*b++ = '\\';
		*b++ = *s++;
	}
	return b;
}

static int	// callback routine for filp->f_op->readdir()
filldir (void *parms, const char *name, int namelen, off_t offset, ino_t ino)
{
	filldir_parms_t		*p = parms;
	unsigned int		mode;
	struct inode		*i;
	tm_t			tm;
	char			*buf, *b, c, *lname = NULL;
	int			llen = 0;
	struct buffer_head	*bh = NULL;

	i = iget(p->super, ino);
	if (!i) {
		printk("kftpd: iget(%lu) failed\n", ino);
		p->rc = -ENOENT;
		return p->rc;	// non-zero rc causes readdir() to stop, but with no indication of an error!
	}

	// Get target of symbolic link: UGLY HACK, COPIED FROM ext2_readlink(); fixme?: broken for /proc/
	mode = i->i_mode;
	if ((mode & S_IFMT) == S_IFLNK && i->i_sb) {
		llen = i->i_sb->s_blocksize - 1;
		if (i->i_blocks) {
			int err;
			bh = ext2_bread(i, 0, 0, &err);
			lname = bh ? bh->b_data : NULL;
		} else {
			lname = (char *) i->u.ext2_i.i_data;
		}
	}

	b = buf = p->buf + p->bytecount;
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
	if (buf[0] == 'c' || buf[0] == 'b') {
		b += sprintf(b, " %3u, %3u", MAJOR(i->i_rdev), MINOR(i->i_rdev));
	} else {
		b += sprintf(b, " %8lu", i->i_size);
		p->blockcount += i->i_blocks;
	}

	b = format_time(gmtime(i->i_mtime, &tm), p->current_year, b);

	*b++ = ' ';
	if (p->use_http) {
		b = append_string(b, "<A HREF=\"", 9, 0);
		if (lname)
			b = append_string(b, lname, llen, 1);
		else
			b = append_string(b, name, namelen, 1);
		if (buf[0] == 'd')
			*b++ = '/';
		b = append_string(b, "\">", 2, 0);
	}
	b = append_string(b, name, namelen, 0);
	if (p->use_http)
		b = append_string(b, "</A>", 4, 0);

	if (lname) {
		*b++ = ' ';
		*b++ = '-';
		*b++ = '>';
		*b++ = ' ';
		b = append_string(b, lname, llen, 0);
	}
	*b++ = '\r';
	*b++ = '\n';
	*b   = '\0';

	// free up the inode structure
	if (bh) brelse (bh);
	iput(i);
	i = NULL;

	p->bytecount += (b - buf);
	if ((p->bufsize - p->bytecount) < 1024)
		return -EAGAIN;	// time to empty the buffer
	return p->rc;	// non-zero rc causes readdir() to stop, but with no indication of an error!
}

static const char dirlist_header[] =
	"HTTP/1.1 200 OK\n"
	"Connection: close\n"
	"Content-Type: text/html\n\n"
	"<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 3.2 Final//EN\">\n"
	"<HTML>\n"
	"<HEAD><TITLE>Index of %s</TITLE></HEAD>\n"
	"<BODY>\n"
	"<H2>Index of %s</H2>\n"
	"<PRE>\n"
	"<HR>\n";

static const char *
send_dirlist (server_parms_t *parms, const char *path)
{
	int		rc;
	struct file	*filp;
	const char	*response = NULL, *servername = parms->servername;
	filldir_parms_t	p;

	lock_kernel();
	filp = filp_open(path,O_RDONLY,0);
	if (IS_ERR(filp) || !filp) {
		printk("%s: filp_open(%s) failed\n", servername, path);
		response = "550 directory not found";
	} else {
		if (!filp->f_dentry || !filp->f_dentry->d_inode || !filp->f_op || !filp->f_op->readdir) {
			response = "550 directory read error";
		} else if (!(p.buf = (unsigned char *)__get_free_page(GFP_KERNEL))) {
			response = "426 out of memory";
		} else if (!(response = open_datasock(parms))) {
			unsigned int	len, sent;
			tm_t		tm;
			struct inode	*inode = filp->f_dentry->d_inode;

			p.current_year	= gmtime(CURRENT_TIME, &tm)->tm_year;
			p.rc		= 0;
			p.use_http	= parms->use_http;
			p.blockcount	= 0;
			p.datasock	= parms->datasock;
			p.super		= inode->i_sb;
			p.bufsize	= PAGE_SIZE;
			p.bytecount	= 0;

			if (parms->use_http)
				p.bytecount = sprintf(p.buf, dirlist_header, path, path);
			down(&inode->i_sem);	// This can go inside the loop
			filp->f_pos = 0;
			do {
				rc = filp->f_op->readdir(filp, &p, filldir); // anything "< 0" is an error
				if (!p.bytecount)
					break;
				up(&inode->i_sem);
				sent = ksock_rw(0, parms->datasock, p.buf, p.bytecount, -1);
				down(&inode->i_sem);
				if (sent != p.bytecount) {
					p.rc = sent < 0 ? sent : -EIO;
					printk("%s: ksock_rw(%lu) returned %u\n", servername, p.bytecount, sent);
				}
				p.bytecount = 0;
			} while (rc >= 0 && !p.rc);
			up(&inode->i_sem);

			if (p.rc) {
				response = "426 ksock_rw() error 1";
			} else if (rc < 0) {
				printk("%s: readdir() returned %d\n", servername, rc);
				response = "426 readdir() error";
			} else {
				if (parms->use_http)
					len = sprintf(p.buf, "</PRE><HR>\n</BODY></HTML>\n");
				else
					len = sprintf(p.buf, "total %lu\r\n", p.blockcount);
				sent = ksock_rw(0, parms->datasock, p.buf, len, -1);
				if (sent != len) {
					printk("%s: ksock_rw(%d) returned %d\n", servername, len, sent);
					response = "426 ksock_rw() error 2";
				}
			}
			if (!parms->use_http)
				sock_release(parms->datasock);
			free_page((unsigned long)p.buf);
			p.buf = NULL;	// paranoia
		}
		filp_close(filp,NULL);
	}
	unlock_kernel();
	return response;
}

static const char http_redirect[] =
	"HTTP/1.1 302 Found\n"
	"Location: %s/\n"
	"Connection: close\n"
	"Content-Type: text/html\n\n"
	"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n"
	"<HTML><HEAD>\n"
	"<TITLE>302 Found</TITLE>\n"
	"</HEAD><BODY>\n"
	"<H1>Found</H1>\n"
	"The document has moved <A HREF=\"%s/\">here</A>.<P>\n"
	"</BODY></HTML>\n";

static void
khttpd_dir_redirect (server_parms_t *parms, const char *path, char *buf)
{
	unsigned int	len, rc;

	len = sprintf(buf, http_redirect, path, path);
	rc = ksock_rw(1, parms->clientsock, buf, len, -1);
	if (rc != len)
		printk("%s: bad_request(): ksock_rw(%d) returned %d\n", parms->servername, len, rc);
}

static const char *
khttp_send_file_header (server_parms_t *parms, const char *path, unsigned long i_size, char *buf)
{
	char *hdr = "HTTP/1.1 200 OK\nConnection: close\nAccept-Ranges: bytes\nContent-Length: %lu\nContent-Type: %s\n\n";
	char *type = "text/plain"; // "application/octet-stream"
	int len;
	const char *s;

	// Very crude "tune" recognition scheme:
	if (i_size > 1000) {
		s = path + strlen(path);
		if (*--s == '0') {
			while (s > path && *--s != '/');
			while (s > path && *--s != '/');
			if (!strncmp(s, "/fids", 5))
				type = "application/octet-stream";	// or perhaps "audio/mpeg3" ??
		}
	}
	len = sprintf(buf, hdr, i_size, type);
	if (len != ksock_rw(2, parms->datasock, buf, len, -1))
		return "426 error sending header; transfer aborted";
	return 0;
}

static const char *
send_file (server_parms_t *parms, const char *path)
{
	unsigned int	size;
	struct file	*filp;
	mm_segment_t	old_fs;
	const char	*response = NULL, *servername = parms->servername;
	unsigned char	*buf;

	lock_kernel();
	if (!(buf = (unsigned char *)__get_free_page(GFP_KERNEL))) {
		response = "426 out of memory";
	} else {
		filp = filp_open(path,O_RDONLY,0);
		if (IS_ERR(filp) || !filp) {
			printk("%s: filp_open(%s) failed\n", servername, path);
			response = "550 file not found";
		} else {
			old_fs = get_fs();
			set_fs(KERNEL_DS);
			if (!filp->f_dentry || !filp->f_dentry->d_inode || !filp->f_op) {
				response = "550 file read error";
			} else if (filp->f_op->readdir) {
				if (parms->use_http)
					khttpd_dir_redirect(parms, path, buf);
				else
					response = "550 not a file";
			} else if (!filp->f_op->read) {
				response = "550 file read error";
			} else if (!(response = open_datasock(parms))) {
				if (!parms->use_http || !(response = khttp_send_file_header(parms, path, filp->f_dentry->d_inode->i_size, buf))) {
					filp->f_pos = 0;
					do {
						size = filp->f_op->read(filp, buf, PAGE_SIZE, &(filp->f_pos));
						if (size < 0) {
							printk("%s: filp->f_op->read() failed; rc=%d\n", servername, size);
							response = "451 error reading file";
							break;
						} else if (size && size != ksock_rw(2, parms->datasock, buf, size, -1)) {
							response = "426 error sending data; transfer aborted";
							break;
						}
					} while (size > 0);
				}
				if (!parms->use_http)
					sock_release(parms->datasock);
			}
			set_fs(old_fs);
			filp_close(filp,NULL);
		}
		free_page((unsigned long)buf);
	}
	unlock_kernel();
	return response;
}

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
	char	buf[CLIENT_CWD_SIZE], *b = buf, *p = path;

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


static void
make_keyword_uppercase (char *keyword)
{
	char c = *keyword;

	while (c && c != ' ') {
		if (c >= 'a' && c <= 'z')
			*keyword -= ('a' - 'A');
		c = *++keyword;
	}
}

static int
kftpd_handle_command (server_parms_t *parms)
{
	char		path[CLIENT_CWD_SIZE], buf[300];
	const char	*response = NULL;
	int		n, rc, quit = 0;
	const char	ABOR[] = {0xf2,'A','B','O','R','\0'};

	n = ksock_rw(0, parms->clientsock, buf, sizeof(buf), 0);
	if (n < 0) {
		printk("%s: client request too short, ksock_rw() failed: %d\n", parms->servername, n);
		return -1;
	} else if (n == 0) {
		printk("%s: EOF on client sock\n", parms->servername);
		return -1;
	}
	if (n < 5 || n >= sizeof(buf)) {
		response = "501 bad command length";
	} else if (buf[--n] != '\n' || buf[--n] != '\r') {
		response = "500 bad end of line";
	} else {
		buf[n] = '\0';	// overwrite '\r'
		printk("%s: '%s'\n", parms->servername, buf);
		make_keyword_uppercase(buf);
		if (!strcmp(buf, "QUIT")) {
			quit = 1;
			response = "221 happy fishing";
		} else if (!strncmp(buf, "USER ", 5)) {
			response = "230 user logged in";
		} else if (!strncmp(buf, "PASS ", 5)) {
			response = "202 password okay";
		} else if (!strncmp(buf, "SYST", 4)) {
			response = "215 UNIX Type: L8";
		} else if (!strcmp(buf, "MODE S")) {
			response = "200 mode-s okay";
		} else if (!strcmp(buf, "STRU F")) {
			response = "200 stru-f okay";
		} else if (!strncmp(buf, "TYPE ", 5)) {
			if (buf[5] != 'I') {
				//response = "502 unsupported xfer TYPE";
				response = "200 type-A okay"; // fixme?
			} else {
				response = "200 type-I okay";
			}
		} else if (!strncmp(buf, "CWD ",4)) {
			append_path(parms->cwd, &buf[4]);
			response = dir_response(257, parms->cwd, "directory changed");
		} else if (!strcmp(buf, "CDUP")) {
			append_path(parms->cwd, "..");
			response = dir_response(257, parms->cwd, NULL);
		} else if (!strcmp(buf, "NOOP")) {
			response = "200 noop okay";
		} else if (!strcmp(buf, "PWD")) {
			response = dir_response(257, parms->cwd, NULL);
		} else if (!strcmp(buf, "HELP SITE")) {
			response = "214-The following SITE commands are recognized.\r\n   CHMOD   EXEC\r\n214 done";
		} else if (!strncmp(buf, "SITE CHMOD ", 11)) {
			response = "500 Not implemented yet";
		} else if (!strcmp(buf, ABOR) || !strcmp(buf, "ABOR")) {
			response = "200 Aborted";
		} else if (!strncmp(buf, "SITE EXEC ", 10)) {
			response = "500 Not implemented yet";
		} else if (!strncmp(buf, "PORT ", 5)) {
			parms->have_portaddr = 0;
			if (extract_portaddr(&parms->portaddr, &buf[5])) {
				response = "501 bad port address";
			} else {
				parms->have_portaddr = 1;
				response = "200 port accepted";
			}
		} else if (!strncmp(buf, "STOR ", 5)) {
			response = "502 upload not supported yet";
		} else if (!strncmp(buf, "LIST", 4)) {
			int j = 4;
			if (buf[j] == ' ' && buf[j+1] == '-')
				while (buf[++j] && buf[j] != ' ');
			strcpy(path, parms->cwd);
			if (buf[j] == ' ') {
				buf[j] = '\0';
				append_path(path, &buf[j+1]);
			}
			if (buf[j]) {
				response = "500 bad command";
			} else {
				response = send_dirlist(parms, path);
				if (!response)
					response = "226 transmission completed";
			}
		} else if (!strncmp(buf, "RETR ", 5)) {
			strcpy(path, parms->cwd);
			if (!buf[5]) {
				response = "501 missing path";
			} else {
				append_path(path, &buf[5]);
				response = send_file(parms, path);
				if (!response)
					response = "226 transmission completed";
			}
		} else {
			response = "500 bad command";
		}
	}
	if (response)
		printk("%s: %s\n", parms->servername, response);
	rc = response ? send_response(parms, response) : -1;
	return quit ? -1 : rc;
}

static void
khttpd_bad_request (server_parms_t *parms, int codenum, const char *title, const char *text)
{
	static const char kttpd_response[] =
		"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n"
		"<HTML><HEAD>\n"
		"<TITLE>%d %s</TITLE>\n"
		"</HEAD><BODY>\n"
		"<H1>%s</H1>\n"
		"%s<P>\n"
		"</BODY></HTML>\n";
	char		buf[256];
	unsigned int	len, rc;

	len = sprintf(buf, kttpd_response, codenum, title, title, text);
	rc = ksock_rw(0, parms->clientsock, buf, len, -1);
	if (rc != len)
		printk("%s: bad_request(): ksock_rw(%d) returned %d\n", parms->servername, len, rc);
}

static int
khttpd_handle_connection (server_parms_t *parms)
{
	const char	*response = NULL, GET[4] = "GET ";
	char		*buf = parms->cwd;
	int		buflen = CLIENT_CWD_SIZE, n;

	n = ksock_rw(0, parms->clientsock, buf, buflen, 0);
	if (n < 0) {
		printk("%s: client request too short, ksock_rw() failed: %d\n", parms->servername, n);
		return -1;
	} else if (n == 0) {
		printk("%s: EOF on client sock\n", parms->servername);
		return -1;
	}
	while (--n && (buf[n] == '\n' || buf[n] == '\r'))
		buf[n] = '\0';
	if (n < sizeof(GET) || n >= buflen || strncmp(buf, GET, sizeof(GET)) || !buf[sizeof(GET)]) {
		khttpd_bad_request(parms, 400, "Bad command", "server only supports GET");
	} else {
		unsigned char *path = &buf[sizeof(GET)];
		for (n = 0; path[n] && path[n] != ' '; ++n); // ignore and strip off all the other http parameters
		path[n--] = '\0';
		printk("%s: '%s'\n", parms->servername, buf);
		// fixme? (maybe):  need to translate incoming char sequences of "%2F" to slashes
		if (path[n] == '/') {
			while (n > 0 && path[n] == '/')
				path[n--] = '\0';
			response = send_dirlist(parms, path);
		} else {
			response = send_file(parms, path);
		}
		if (response)
			khttpd_bad_request(parms, 404, "Error retrieving file", response);
	}
	return 0;
}

static int
get_clientip (server_parms_t *parms)
{
	struct sockaddr_in	addr;
	int			addrlen = sizeof(struct sockaddr_in);

	parms->clientip[0] = '\0';
	return (parms->clientsock->ops->getname(parms->clientsock, (struct sockaddr *)&addr, &addrlen, 1) >= 0)
	 	&& (addr.sin_family == AF_INET)
	 	&& !inet_ntop(AF_INET, &addr.sin_addr.s_addr, parms->clientip, INET_ADDRSTRLEN);
}

static int
ksock_accept (server_parms_t *parms)
{
	lock_kernel();
	if ((parms->clientsock = sock_alloc())) {
		parms->clientsock->type = parms->servsock->type;
		if (parms->servsock->ops->dup(parms->clientsock, parms->servsock) < 0) {
			printk("%s: sock_accept: dup() failed\n", parms->servername);
		} else if (parms->clientsock->ops->accept(parms->servsock, parms->clientsock, parms->servsock->file->f_flags) < 0) {
			printk("%s: sock_accept: accept() failed\n", parms->servername);
		} else {
			unlock_kernel();
			return 0;	// success
		}
		sock_release(parms->clientsock);
	}
	unlock_kernel();
	return 1;	// failure
}

static void
run_server (int use_http, struct semaphore *sem_p, int control_port, int data_port)
{
	struct task_struct	*tsk = current;
	server_parms_t		*parms = kmalloc(sizeof(server_parms_t), GFP_KERNEL);

	if (!parms)
		return;
	memset(parms, 0, sizeof(parms));
	parms->use_http  = use_http;
	parms->data_port = data_port;
	strcpy(parms->servername, use_http ? "khttpd" : "kftpd");

	// kthread setup
	tsk->session = 1;
	tsk->pgrp = 1;
	strcpy(tsk->comm, parms->servername);
	sigfillset(&tsk->blocked);

	if (sem_p) {
		*sem_p = MUTEX_LOCKED;
		down(sem_p);	// wait for hijack.c to get our port numbers from config.ini
	}

	if (control_port && data_port) {
		if (make_socket(&parms->servsock, control_port)) {
			printk("%s: make_socket(port=%d) failed\n", parms->servername, control_port);
		} else if (parms->servsock->ops->listen(parms->servsock, use_http ? 5 : 1) < 0) {
			printk("%s: listen(port=%d) failed\n", parms->servername, control_port);
		} else {
			printk("%s: listening on port %d\n", parms->servername, control_port);
			while (1) {
				if (ksock_accept(parms)) {
					printk("%s: accept() failed\n", parms->servername);
				} else {
					if (get_clientip(parms)) {
						printk("%s: get_clientip failed\n", parms->servername);
					} else {
						printk("%s: connection from %s\n", parms->servername, parms->clientip);
						if (parms->use_http) {
							khttpd_handle_connection(parms);
						} else {
							if (!send_response(parms, "220 connected")) {
								strcpy(parms->cwd, "/");
								while (!kftpd_handle_command(parms));
							}
						}
					}
					sock_release(parms->clientsock);
				}
			}
		}
	}
}

int kftpd (void *unused)	// invoked from init/main.c
{
	run_server(0, &hijack_kftpd_startup_sem, hijack_kftpd_control_port, hijack_kftpd_data_port);
	return 0;
}

int khttpd (void *unused)	// invoked from init/main.c
{
	run_server(1, &hijack_khttpd_startup_sem, hijack_khttpd_port, hijack_khttpd_port);
	return 0;
}

