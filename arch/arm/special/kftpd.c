// kftpd by Mark Lord
//
// This version can UPLOAD and DOWNLOAD files, directories..
//
// To Do:  fix various buffers to use PATH_MAX (4096) instead of 256 or 512 bytes

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

extern void sys_exit(int);
extern int strxcmp (const char *str, const char *pattern, int partial);	// hijack.c
extern int hijack_do_command(const char *command, unsigned int size);	// notify.c 
extern void show_message (const char *message, unsigned long time);	// hijack.c
extern int hijack_khttpd_port;				// from arch/arm/special/hijack.c
extern int hijack_khttpd_verbose;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_control_port;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_data_port;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_verbose;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_show_dotdir;			// from arch/arm/special/hijack.c
extern int hijack_max_connections;			// from arch/arm/special/hijack.c
extern struct semaphore hijack_khttpd_startup_sem;	// from arch/arm/special/hijack.c
extern struct semaphore hijack_kftpd_startup_sem;	// from arch/arm/special/hijack.c
extern int sys_rmdir(const char *path); // fs/namei.c
extern int sys_readlink(const char *path, char *buf, int bufsiz);
extern int sys_chmod(const char *path, mode_t mode); // fs/open.c
extern int sys_mkdir(const char *path, int mode); // fs/namei.c
extern int sys_unlink(const char *path);
extern int sys_newstat(char *, struct stat *);
extern int sys_newfstat(int, struct stat *);
extern int sys_fsync(int);
extern pid_t kernel_thread(int (*fn)(void *), void *arg, unsigned long flags);
extern int sys_wait4 (pid_t pid,unsigned int * stat_addr, int options, struct rusage * ru);

#define INET_ADDRSTRLEN		16

// This data structure is allocated as a full page to prevent memory fragmentation:
typedef struct server_parms_s {
	struct socket		*clientsock;
	struct socket		*servsock;
	struct socket		*datasock;
	struct sockaddr_in	clientaddr;
	char			verbose;		// bool
	char			generate_playlist;	// bool
	char			use_http;		// bool
	char			have_portaddr;		// bool
	char			icy_metadata;		// bool
	char			filler;		// not used.
	short			data_port;
	off_t			start_offset;		// starting offset for next FTP/HTTP file transfer
	off_t			end_offset;		// for current HTTP file read
	unsigned int		umask;
	char			*servername;
	struct sockaddr_in	portaddr;
	char			clientip[INET_ADDRSTRLEN];
	char			serverip[INET_ADDRSTRLEN];
	unsigned char		cwd[1024];
	unsigned char		buf[1024];
	unsigned char		tmp2[768];
	unsigned char		tmp3[768];
} server_parms_t;

#define INRANGE(c,min,max)	((c) >= (min) && (c) <= (max))
#define TOUPPER(c)		(INRANGE((c),'a','z') ? ((c) - ('a' - 'A')) : (c))

extern int get_number (char **src, int *val, int base, const char *nextchars);	// hijack.c

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
inet_pton (int af, char **src, void *dst)
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
extract_portaddr (struct sockaddr_in *addr, char *s)
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
ksock_rw (struct socket *sock, char *buf, int buf_size, int minimum)
{
	int		bytecount = 0, sending = 0;

	if (minimum < 0) {
		minimum = buf_size;
		sending = 1;
	}
	do {
		int		rc, len = buf_size - bytecount;
		struct msghdr	msg;
		struct iovec	iov;

		memset(&msg, 0, sizeof(msg));
		iov.iov_base       = &buf[bytecount];
		iov.iov_len        = len;
		msg.msg_iov        = &iov;
		msg.msg_iovlen     = 1;

		lock_kernel();
		if (sending) {
			msg.msg_flags = MSG_DONTWAIT;	// asynchronous send
			while ((rc = sock_sendmsg(sock, &msg, len)) == -EAGAIN)
				msg.msg_flags = 0;	// use synchronous send instead
		} else {
			rc = sock_recvmsg(sock, &msg, len, 0);
		}
		unlock_kernel();
		if (rc < 0 || (!sending && rc == 0)) {
			if (rc && rc != -EPIPE && rc != -ECONNRESET)
				printk("ksock_rw: %s rc=%d\n", sending ? "sock_sendmsg()" : "sock_recvmsg()", rc);
			break;
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
		"   REST\r\n"
		"214 Okay"},
	{216,	"-The following SITE commands are recognized\r\n"
		"   BUTTON  CHMOD   HELP    POPUP   REBOOT  RO      RW\r\n"
		"216 Okay"},
	{215,	" UNIX Type: L8"},
	{220,	" Connected"},
	{221,	" Happy Fishing"},
	{226,	" Okay"},
	{230,	" Okay"},
	{250,	" Okay"},
	{257,	" Okay"},
	{350,	" Restarting next transfer"},	//350 Restarting at 999. Send STORE or RETRIEVE to initiate transfer.
	{425,	" Connection error"},
	{426,	" Connection failed"},
	{431,	" No such directory"},
	{451,	" Internal error"},
	{500,	" Bad command"},
	{501,	" Bad syntax"},
	{502,	" Not implemented"},
	{541,	" Remote command failed"},
	{550,	" Failed"},
	{553,	" Invalid action"},
	{0,	NULL} // End-Of-Table Marker
	};

static int
kftpd_send_response (server_parms_t *parms, int rcode)
{
	char		buf[512];
	int		len, rc;
	response_t	*r = response_table;

	while (r->rcode && r->rcode != rcode)
		++r;
	if (parms->verbose)
		printk("%s: %d%s.\n", parms->servername, rcode, r->text);
	len = sprintf(buf, "%d%s.\r\n", rcode, r->text);
	if ((rc = ksock_rw(parms->clientsock, buf, len, -1)) != len) {
		printk("%s: ksock_rw(response) failed, rc=%d\n", parms->servername, rc);
		return -1;
	}
	return 0;
}

static int
send_dir_response (server_parms_t *parms, int rcode, const char *dir, char *suffix)
{
	char	buf[300];
	int	rc, len;

	if (suffix)
		len = sprintf(buf, "%d \"%s\" %s\r\n", rcode, dir, suffix);
	else
		len = sprintf(buf, "%d \"%s\"\r\n", rcode, dir);
	if (parms->verbose)
		printk("%s: %s", parms->servername, buf);
	if ((rc = ksock_rw(parms->clientsock, buf, len, -1)) != len) {
		printk("%s: ksock_rw(dir_response) failed, rc=%d\n", parms->servername, rc);
		return 1;
	}
	return 0;
}

static int
set_sockopt (server_parms_t *parms, struct socket *sock, int protocol, int option, int off_on)
{
	int	rc;

	rc = sock_setsockopt(sock, protocol, option, (char *)&off_on, sizeof(off_on));
	if (rc)
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
		printk("%s: sock_create() failed, rc=%d\n", parms->servername, rc);
	} else if (set_sockopt(parms, sock, SOL_SOCKET, SO_REUSEADDR, 1)) {
		sock_release(sock);
	} else {
		memset(&addr, 0, sizeof(struct sockaddr_in));
		addr.sin_family	     = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port        = htons(port);
		rc = sock->ops->bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
		if (rc) {
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

	if (parms->use_http) {
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
			if (response)
				sock_release(parms->datasock);
		}
	}
	return response;
}

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
convert_time (time_t time, tm_t *tm)
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


// Pattern matching: returns 1 if n(ame) matches p(attern), 0 otherwise
static int
glob_match (const char *n, const char *p)
{
	while (*n && (*n == *p || *p == '?')) {
		++n;
		++p;
	}
	if (*p == '*') {
		while (*++p == '*');
		while (*n) {
			while (*n && (*n != *p && *p != '?'))
				++n;
			if (*n && glob_match(n++, p))
				return 1;
		}
	}
	return (!(*n | *p));
}

typedef struct filldir_parms_s {
	unsigned short		current_year;	// current calendar year (YYYY), according to the Empeg
	unsigned short		full_listing;	// 0 == names only, 1 == "ls -al"
	unsigned short		use_http;	// 0 == ftp, 1 == http
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
	if (name[0] == '.' && namelen <= 2) {
		if (namelen == 1) {
			if (p->use_http || !hijack_kftpd_show_dotdir)
				return 0;	// skip "." in khttpd listings
		} else if (name[1] == '.' && p->path_len == 1) {
			if (p->path[0] == '/')	// paranoia: path_len==1 ought to be sufficient
				return 0;	// skip ".." when listing "rootdir"
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
	if (!p->pattern || glob_match(zname, p->pattern))
		p->nam_used = len;	// accept this entry; otherwise it gets overwritten next time thru
	return 0;			// continue reading directory entries
}

const char dirlist_html_trailer[] = "</PRE><HR>\r\n</BODY></HTML>\r\n";
#define DIRLIST_TRAILER_MAX	(sizeof(dirlist_html_trailer))

static int
send_dirlist_buf (server_parms_t *parms, filldir_parms_t *p, int send_trailer)
{
	int sent;

	if (p->full_listing && send_trailer) {
		if (parms->use_http)
			p->buf_used += sprintf(p->buf + p->buf_used, dirlist_html_trailer);
		else
			p->buf_used += sprintf(p->buf + p->buf_used, "total %lu\r\n", p->blockcount);
	}
	sent = ksock_rw(parms->datasock, p->buf, p->buf_used, -1);
	if (sent != p->buf_used) {
		if (parms->verbose)
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
                	printk("%s: lnamei(%s) failed, rc=%ld\n", parms->servername, p->path, PTR_ERR(dentry));
                	return -ENOENT;
		}
        } else if (!(inode = iget(p->sb, ino))) {	// iget() is magnitudes faster than lnamei()
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

	b = format_time(convert_time(inode->i_mtime, &tm), p->current_year, b);
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
	if ((p->buf_size - p->buf_used) < ((parms->use_http ? (24+namelen) : 7) + namelen + (linklen ? (2 * linklen) : namelen))) {
		if ((rc = send_dirlist_buf(parms, p, 0)))	// empty the buffer
			goto exit;
		b = p->buf;
	}

	if (parms->use_http) {
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
	if (parms->use_http) {
		if (ftype == 'd')
			*b++ = '/';
		*b++ = '<';
		*b++ = '/';
		*b++ = 'A';
		*b++ = '>';
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
	"<HTML>\r\n"
	"<HEAD><TITLE>Index of %s</TITLE></HEAD>\r\n"
	"<BODY>\r\n"
	"<H2>Index of %s</H2>\r\n"
	"<PRE>\r\n"
	"<HR>\r\n";

static int
send_dirlist (server_parms_t *parms, char *path, int full_listing)
{
	int		rc, pathlen;
	struct file	*filp;
	unsigned int	response = 0;
	filldir_parms_t	p;

	memset(&p, 0, sizeof(p));
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
		}
	}
	lock_kernel();
	filp = filp_open(path,O_RDONLY,0);
	if (IS_ERR(filp) || !filp) {
		printk("%s: filp_open(%s) failed\n", parms->servername, path);
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

			p.current_year	= convert_time(CURRENT_TIME, &tm)->tm_year;
			p.buf_size	= PAGE_SIZE;
			p.nam_size	= PAGE_SIZE;
			p.full_listing	= full_listing;
			strcpy(p.path, path);
			p.path_len	= pathlen;
			if (p.path[pathlen - 1] != '/')
				p.path[pathlen++] = '/';
			p.name		= p.path + pathlen;
			p.use_http	= parms->use_http;
			p.sb		= dentry->d_sb;
			if (parms->use_http)
				p.buf_used = sprintf(p.buf, dirlist_header, path, path);
			do {
				p.nam_used = 0;
				p.filecount = 0;
				schedule(); // give the music player a chance to run
				down(&inode->i_sem);
				rc = readdir(filp, &p, filldir);	// anything "< 0" is an error
				up(&inode->i_sem);
				if (rc < 0) {
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
							if (parms->verbose)
								printk("%s: format_dir('%s') returned %d\n", parms->servername, p.name, rc);
							break;
						}
					}
				}
			} while (!rc && p.filecount);
			if (rc || (rc = send_dirlist_buf(parms, &p, 1)))
				response = 426;
			if (!parms->use_http)
				sock_release(parms->datasock);
			if (p.nam)
				free_page((unsigned long)p.nam);
			if (p.buf)
				free_page((unsigned long)p.buf);
		}
		filp_close(filp,NULL);
	}
	unlock_kernel();
	return response;
}

static void
khttpd_respond (server_parms_t *parms, int rcode, const char *title, const char *text)
{
	static const char kttpd_response[] =
		"HTTP/1.1 %d %s\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html\r\n\r\n"
		"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		"<HTML><HEAD>\r\n"
		"<TITLE>%d %s</TITLE>\r\n"
		"</HEAD><BODY>\r\n"
		"<H1>%s</H1>\r\n"
		"%s<P>\r\n"
		"</BODY></HTML>\r\n";

	char		*buf = parms->cwd;
	unsigned int	len, rc;

	len = sprintf(buf, kttpd_response, rcode, title, rcode, title, title, text ? text : "");
	rc = ksock_rw(parms->clientsock, buf, len, -1);
	if (rc != len && parms->verbose)
		printk("%s: respond(): ksock_rw(%d) returned %d, data='%s'\n", parms->servername, len, rc, buf);
}


static void
khttpd_redirect (server_parms_t *parms, const char *path)
{
	static const char http_redirect[] =
		"HTTP/1.1 302 Found\r\n"
		"Location: %s%s\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html\r\n\r\n"
		"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		"<HTML><HEAD>\r\n"
		"<TITLE>302 Found</TITLE>\r\n"
		"</HEAD><BODY>\r\n"
		"<H1>Found</H1>\r\n"
		"The document has moved <A HREF=\"%s%s\">here</A>.<P>\r\n"
		"</BODY></HTML>\r\n";
	char *buf = parms->tmp3;

	unsigned int	len, rc;
	char *slash = parms->generate_playlist ? "" : "/";

	len = sprintf(buf, http_redirect, path, slash, path, slash);
	rc = ksock_rw(parms->clientsock, buf, len, -1);
	if (rc != len)
		printk("%s: redirect(): ksock_rw(%d) returned %d\n", parms->servername, len, rc);
}

static const char audio_mpeg[]		= "audio/mpeg";
static const char audio_wav[]		= "audio/x-wav";
static const char audio_wma[]		= "audio/x-ms-wma";
static const char audio_m3u[]		= "audio/x-mpegurl";
static const char text_plain[]		= "text/plain";
static const char text_html[]		= "text/html";
static const char application_octet[]	= "application/octet-stream";
static const char application_x_tar[]	= "application/x-tar";

typedef struct mime_type_s {
	const char	*pattern;	// a "glob" expression using * and/or ? wildcards
	const char	*mime;		// the mime-type for matching paths
} mime_type_t;

static const mime_type_t mime_types[] = {
	{"*.tiff",		"image/tiff"		},
	{"*.jpg",		"image/jpeg"		},
	{"*.png",		"image/png"		},
	{"*.htm",		 text_html		},
	{"*.html",		 text_html		},
	{"*.txt",		 text_plain		},
	{"*.text",		 text_plain		},
	{"*.wav",		 audio_wav		},
	{"*.m3u",		 audio_m3u		},
	{"*.mp3",		 audio_mpeg		},
	{"*.wma",		 audio_wma		},
	{"*.gz",		"application/x-gzip"	},
	{"*.tar",		 application_x_tar	},
	{"*.tgz",		 application_x_tar	},
	{"*/config.ini",	 text_plain		},
	{"*/tags",		 text_plain		},
	{"/etc/*",		 text_plain		},
	{"/proc/*",		 text_plain		},
	{"/drive?/fids/*1",	 text_plain		},
	{"*bin/*",		 application_octet	},
	{NULL,			NULL			}};

static char
get_tag (char *s, char *tag, char *buf, int buflen)
{
	*buf = '\0';
	while (*s) {
		if (*s == *tag && !strxcmp(s, tag, 1)) {
			char *val = buf;
			s += strlen(tag);
			while (*s && *s != '\n' && --buflen > 0)
				*buf++ = *s++;
			*buf = '\0';
			return *val;
		}
		while (*s && *s != '\n')
			++s;
		while (*s == '\n')
			++s;
	}
	return '\0';
}

static int
open_fid_file (char *path, const char *tail)
{
	int	fd;
	char	*tailp = NULL, saved = '\0';

	if (tail) {					// use a different tail character?
		tailp = path + strlen(path) - 1;
		saved = *tailp;
		*tailp = *tail;				// substitute tail character
	}
	if (0 > (fd = open(path, O_RDONLY, 0))) {	// try the specified drive first
		path[6] ^= 1;				// failed, now try the other drive
		if (0 > (fd = open(path, O_RDONLY, 0)))
			path[6] ^= 1;			// point back at original drive
	}
	if (tailp)
		*tailp = saved;				// restore tail character
	return fd;
}

static const char *
get_fid_mime_title (char *path, char *buf, int bufsize, char *title, int titlelen, char *ext)
{
	const char	*mimetype = application_octet;
	char		type[12], codec[12];
	int		fd, size;

	if (0 <= (fd = open_fid_file(path, "1"))) {	// open tagfile
		size = read(fd, buf, bufsize-1);
		buf[size] = '\0';
		close(fd);
		get_tag(buf, "type=", type, sizeof(type));
		if (*type == 't' && get_tag(buf, "codec=", codec, sizeof(codec)) && *codec) {
			switch (codec[1]) {
				case 'p': mimetype = audio_mpeg; break;
				case 'm': mimetype = audio_wma;	 break;
				case 'a': mimetype = audio_wav;	 break;
			}
			get_tag(buf, "title=", title, titlelen - 1);
			*ext++ = '.';
			strcpy(ext, codec);
		}

	}
	return mimetype;
}

static int
khttp_send_file_header (server_parms_t *parms, char *path, off_t length, char *buf, int bufsize)
{
	int		len;
	const char	*mimetype, *rcode = "200 OK";
	off_t		clength = length;
	char		title[64], ext[8];

	title[0] = '\0';
	if (glob_match(path, "/drive?/fids/*0")) {
		mimetype = get_fid_mime_title(path, buf, bufsize, title, sizeof(title), ext);
	} else {
		const char		*pattern;
		const mime_type_t	*m = mime_types;
		while ((pattern = m->pattern) && !glob_match(path, pattern))
			++m;
		mimetype = m->mime;
	}
	if (clength && parms->end_offset != -1) {
		clength = parms->end_offset + 1 - parms->start_offset;
		rcode = "206 Partial content";
	}
	len = sprintf(buf, "HTTP/1.1 %s\r\nConnection: close\r\n", rcode);
	if (mimetype)
		len += sprintf(buf+len, "Content-Type: %s\r\n", mimetype);
	if (*title) {	// tune title for WinAmp, XMMS, Save-To-Disk, etc..
		if (parms->icy_metadata)
			len += sprintf(buf+len, "icy-name: %s\r\n", title);
		else
			len += sprintf(buf+len, "Content-Disposition: attachment; filename=%s%s\r\n", title, ext);
	}
	if (clength) {
		len += sprintf(buf+len, "Accept-Ranges: bytes\r\nContent-Length: %lu\r\n", clength);
		if (parms->end_offset != -1)
			len += sprintf(buf+len, "Content-Range: bytes %lu-%lu/%lu\r\n", parms->start_offset, parms->end_offset, length);
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

	parms->datasock = NULL;
	xfer->buf_size = PAGE_SIZE;
	xfer->buf = NULL;
	xfer->writing = writing;
	xfer->redirected = 0;
	if (writing)
		flags = start_offset ? O_RDWR : O_RDWR|O_CREAT|O_TRUNC;
	else
		flags = O_RDONLY;

	fd = open(path, flags, 0666 & ~parms->umask);
	if (fd < 0 && !writing && parms->use_http && glob_match(path, "/drive?/fids/*")) {
		path[6] ^= 1;	// try the other drive; we cannot use httpd_redirect() here
		if (0 > (fd = open(path, flags, 0))) 
			path[6] ^= 1;	// restore original path
	}
	xfer->fd = fd;
	if (fd < 0) {
		printk("%s: open(%s) failed, rc=%d\n", parms->servername, path, fd);
		response = 550;
	} else if (!(xfer->buf = (unsigned char *)__get_free_page(GFP_KERNEL))) {
		response = 451;
	} else if (sys_newfstat(fd, &xfer->st)) {
		printk("%s: fstat(%s) failed\n", parms->servername, path);
		response = 550;
	} else if (S_ISDIR(xfer->st.st_mode)) {
		if (writing || !parms->use_http || parms->generate_playlist) {
			response = 550;
		} else {
			khttpd_redirect(parms, path);
			xfer->redirected = 1;
		}
	} else if (start_offset && (start_offset >= xfer->st.st_size || lseek(fd, start_offset, 0))) {
		printk("%s: lseek(%s,%lu) failed\n", parms->servername, path, start_offset);
		response = 553;
	} else {
		response = open_datasock(parms);
	}
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
	if (!parms->use_http && parms->datasock) {
		sock_release(parms->datasock);
		parms->datasock = NULL;
	}
	if (xfer->buf)
		free_page((unsigned long)xfer->buf);
	parms->start_offset =  0;
	parms->end_offset   = -1;
}

static unsigned int
get_duration (char *buf)
{
	char		duration[16], *d = duration;
	unsigned int	secs = 0;

	if (get_tag(buf, "duration=", duration, sizeof(duration))) {
		if (get_number(&d, &secs, 10, NULL))
			secs /= 1000;	// convert msecs to secs
	}
	return secs;
}

// Mmmm.. probably the wrong approach, but it will do for starters.
// Much better might be to implement an on-the-fly "/proc/playlists" tree,
// using symlinks (with extensions!) to point at the actual audio files.
// At present, this is incredibly ugly code (but it works).  -ml
//

static int
send_playlist (server_parms_t *parms, char *path)
{
	unsigned int	response, secs;
	char		tags_path[] = "/driveX/fids/XXXXXXXXXX", type[12], codec[4], title[64], artist[32], source[32];
	int		playlist_fd, fid = -1, done_header = 0, sent, size;
	file_xfer_t	xfer;

	if ((response = prepare_file_xfer(parms, path, &xfer, 0)))
		goto cleanup;
	size = read(xfer.fd, xfer.buf, xfer.buf_size);
	close(xfer.fd); xfer.fd = -1;
	if (size < 0)
		size = 0;
	xfer.buf[size] = '\0';	// Ensure zero-termination of the data
	if (!get_tag(xfer.buf, "type=", type, sizeof(type))) {
		response = 408;
		goto cleanup;
	}
	(void) get_tag(xfer.buf, "artist=", artist, sizeof(artist));
	(void) get_tag(xfer.buf, "title=",  title,  sizeof(title));
	if (!*title)
		strcpy(title, path);
	if (!strxcmp("tune", type, 0)) {
		path[strlen(path)-1] = '0';
		(void) get_tag(xfer.buf, "codec=", codec, sizeof(codec));
		if (!strxcmp(codec, "mp3", 0)) {
			secs = get_duration(xfer.buf);
			size  = sprintf(xfer.buf, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: %s\r\n\r\n#EXTM3U\r\n"
				"#EXTINF:%u,%s - %s\r\nhttp://%s%s\r\n", audio_m3u, secs, artist, title, parms->serverip, path);
			(void)ksock_rw(parms->datasock, xfer.buf, size, -1);
		} else { // wma, wav
			khttpd_redirect(parms, path);
		}
		goto cleanup;
	}
	if (strxcmp("playlist", type, 0)) {
		response = 408;
		goto cleanup;
	}
	playlist_fd = open_fid_file(path, "0");
	if (playlist_fd < 0) {
		response = 408;
		goto cleanup;
	}
	set_sockopt(parms, parms->datasock, SOL_TCP, TCP_NODELAY, 0);
	while (sizeof(fid) == read(playlist_fd, (char *)&fid, sizeof(fid))) {
		int	tags_fd;
		char	*tags_buf = parms->tmp3;
		fid |= 1;
		sprintf(tags_path, "/drive0/fids/%x", fid);
		tags_fd = open_fid_file(tags_path, NULL);	// get tagfile
		if (tags_fd < 0) {
			printk("open subfid failed: '%s', rc=%d\n", tags_path, tags_fd);
			continue;
		}
		size = read(tags_fd, tags_buf, sizeof(parms->tmp3)-1);
		close(tags_fd);
		if (size <= 0) {
			printk("read subfid failed '%s', rc=%d\n", tags_path, size);
			continue;
		}
		tags_buf[size] = '\0';
		(void) get_tag(tags_buf, "type=", type, sizeof(type));
		if (parms->generate_playlist != 1 && *type != 't')
			continue;
		size = 0;
		if (!done_header) {
			done_header = 1;
			size  = sprintf(xfer.buf, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: %s\r\n\r\n",
				(parms->generate_playlist == 2) ? audio_m3u : text_html);
			if (parms->generate_playlist == 1) {
				const char *hyphen = *artist ? " - " : "";
				size += sprintf(xfer.buf+size, "<HTML><HEAD><TITLE>%s%s%s</TITLE></HEAD>\r\n<BODY>\r\n"
					"<H2>%s%s%s</H2><TABLE BORDER=2><THEAD>\r\n"
					"<HTML><BODY><TABLE BORDER=2><THEAD>\r\n"
					"<TR><TD> <TD> <b>Title</b> <TD> <b>Length</b> <TD> <b>Type</b> "
					"<TD> <b>Artist</b> <TD> <b>Source</b> <TBODY>\r\n",
					artist, hyphen, title, artist, hyphen, title);
			} else {
				size += sprintf(xfer.buf+size, "#EXTM3U\r\n");
			}
		}
		(void) get_tag(tags_buf, "artist=", artist, sizeof(artist));
		(void) get_tag(tags_buf, "source=", source, sizeof(source));
		(void) get_tag(tags_buf, "title=",  title,  sizeof(title));
		if (!*title)
			strcpy(title, tags_path);
		secs = get_duration(tags_buf);
		if (parms->generate_playlist == 1) {
			const char *extra = (*type == 't') ? "0" : "1?.html";
			size += sprintf(xfer.buf+size, "<TR><TD> <A HREF=\"%x?.m3u\"><em>Play</em></A> ", fid);
			size += sprintf(xfer.buf+size, "<TD> <A HREF=\"%x%s\">%s</A> <TD> %u:%02u <TD> %s "
			  "<TD> %s <TD> %s \r\n", fid>>4, extra, title, secs/60, secs%60, type, artist, source);
		} else if (*type == 't') {
			size += sprintf(xfer.buf+size, "#EXTINF:%u,%s - %s\r\nhttp://%s%s\r\n",
				secs, artist, title, parms->serverip, tags_path);
			xfer.buf[size-3] = '0';	// convert tags_path into tune path
		}
		sent = ksock_rw(parms->datasock, xfer.buf, size, -1);
		if (sent != size) {
			printk("Size=%d, sent=%d\n", size, sent);
			break;
		}
	}
	if (done_header) {
		if (parms->generate_playlist == 1) {
			static char trailer[] = "</TABLE></BODY></HTML>\r\n";
			(void) ksock_rw(parms->datasock, trailer, sizeof(trailer)-1, -1);
		}
	} else if (parms->generate_playlist == 1) {
		khttpd_respond(parms, 200, "Empty Playlist", "empty playlist");
	} else {
		path[strlen(path)-1] = '1';
		strcat(path,"?.html");
		khttpd_redirect(parms, path);
	}
cleanup:
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
	if (!response) {
		off_t	filepos, filesize = xfer.st.st_size;
		if (parms->use_http) {
			if (!filesize)
				parms->end_offset = -1;
			else if (parms->start_offset && parms->end_offset == -1)
				parms->end_offset = filesize - 1;
		}
		if (!parms->use_http || !khttp_send_file_header(parms, path, filesize, parms->tmp3, sizeof(parms->tmp3))) {
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
				filepos += size;
				if (size < 0) {
					printk("%s: read() failed; rc=%d\n", parms->servername, size);
					response = 451;
				} else if (size && size != ksock_rw(parms->datasock, xfer.buf, size, -1)) {
					response = 426;
					break;
				}
			} while (size > 0);
		}
	}
	cleanup_file_xfer(parms, &xfer);
	return response;
}

static int
do_rmdir (server_parms_t *parms, const char *path)
{
	int	rc, response = 250;;

	if ((rc = sys_rmdir(path))) {
		printk("%s: rmdir('%s') failed, rc=%d\n", parms->servername, path, rc);
		response = 550;
	}
	return response;
}

static int
do_mkdir (server_parms_t *parms, const char *path)
{
	int	rc, response = 0;

	if ((rc = sys_mkdir(path, 0777 & ~parms->umask))) {
		printk("%s: mkdir('%s') failed, rc=%d\n", parms->servername, path, rc);
		response = 550;
	} else {
		(void) send_dir_response(parms, 257, path, "directory created");
	}
	return response;
}

static int
do_chmod (server_parms_t *parms, unsigned int mode, const char *path)
{
	int	rc, response = 200;

	if ((rc = sys_chmod(path, mode))) {
		if (rc != -ENOENT && parms->verbose)
			printk("%s: chmod('%s',%d) failed, rc=%d\n", parms->servername, path, mode, rc);
		response = 550;
	}
	return response;
}

static int
do_delete (server_parms_t *parms, const char *path)
{
	int	rc, response = 250;

	if ((rc = sys_unlink(path))) {
		printk("%s: unlink('%s') failed, rc=%d\n", parms->servername, path, rc);
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

	response = prepare_file_xfer(parms, path, &xfer, 1);
	if (!response) {
		do {
			schedule(); // give the music player a chance to run
			size = ksock_rw(parms->datasock, xfer.buf, xfer.buf_size, 1);
			if (size < 0) {
				response = 426;
			} else if (size && size != write(xfer.fd, xfer.buf, size)) {
				printk("%s: write(%d) failed\n", parms->servername, size);
				response = 451;
			}
		} while (!response && size > 0);
	}
	cleanup_file_xfer(parms, &xfer);
	return response;
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
	// Now fix-up the path, resolving '..' and removing consecutive '/'
	*p++ = '/';
	while (*tmp) {
		while (*tmp == '/')
			++tmp;
		if (tmp[0] == '.' && tmp[1] == '.' && (tmp[2] == '/' || tmp[2] == '\0')) {
			tmp += 2;
			while (*--p != '/');
			if (p == path)
				++p;
		} else if (*tmp) { // copy simple path element
			if (*(p-1) != '/')
				*p++ = '/';
			while (*tmp && *tmp != '/')
				*p++ = *tmp++;
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
// Apparently Win2K clients still use the (obsolete) RFC775 "XMKD" command as well.
//
// Non-standard UNIX extensions for "SITE" command (wu-ftpd, ncftp):
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
//		  --> output should be captured (pipe) and returned using a set of "200" responses
//
/////////////////////////////////////////////////////////////////////////////////

static response_t simple_response_table[] = {
	{230,	"\1USER "},
	{202,	"\1PASS "},
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
	{214,	"\0HELP"},
	{216,	"\0SITE HELP"},
	{200,	"\0"},
	{0,	NULL}};

static int
kftpd_handle_command (server_parms_t *parms)
{
	char		*path = parms->tmp2, *buf = parms->buf;
	unsigned int	response = 0;
	int		n, quit = 0, bufsize = sizeof(parms->buf) - 1;
	response_t	*r;

	n = ksock_rw(parms->clientsock, buf, bufsize, 0);
	if (n < 0) {
		printk("%s: ksock_rw() failed, rc=%d\n", parms->servername, n);
		return -1;
	} else if (n == 0) {
		if (parms->verbose)
			printk("%s: EOF on client sock\n", parms->servername);
		return -1;
	}
	if (n >= bufsize)
		n = bufsize;
	buf[n] = '\0';
	if (buf[n - 1] == '\n')
		buf[--n] = '\0';
	if (buf[n - 1] == '\r')
		buf[--n] = '\0';
	if (parms->verbose)
		printk("%s: '%s'\n", parms->servername, buf);

	// first look for commands that have hardcoded reponses:
	for (r = simple_response_table; r->text; ++r) {
		if (!strxcmp(buf, &(r->text[1]), r->text[0])) {
			response = r->rcode;
			if (response == 221)
				quit = 1;
			goto got_response;
		}
	}

	// now look for commands involving more complex handling:
	if (!strxcmp(buf, "LIST", 1) || !strxcmp(buf, "NLST", 1)) {
		int j = 4;
		if (buf[j] == ' ' && buf[j+1] == '-')
			while (buf[++j] && buf[j] != ' ');
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
			quit = send_dir_response(parms, 250, parms->cwd, "directory changed");
		}
	} else if (!strxcmp(buf, "CDUP", 0)) {
		append_path(parms->cwd, "..", parms->tmp3);
		quit = send_dir_response(parms, 200, parms->cwd, NULL);
	} else if (!strxcmp(buf, "PWD", 0)) {
		quit = send_dir_response(parms, 257, parms->cwd, NULL);
	} else if (!strxcmp(buf, "SITE CHMOD ", 1)) {
		char *p = &buf[11];
		int mode;
		if (!get_number(&p, &mode, 8, " ") || !*p++ || !*p) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, p, parms->tmp3);
			response = do_chmod(parms, mode, path);
		}
	} else if (!strxcmp(buf, "SITE ", 1)) {
		response = hijack_do_command(&buf[5], n - 5) ? 541 : 200;
	} else if (!strxcmp(buf, "PORT ", 1)) {
		parms->have_portaddr = 0;
		if (extract_portaddr(&parms->portaddr, &buf[5])) {
			response = 501;
		} else {
			parms->have_portaddr = 1;
			response = 200;
		}
	} else if (!strxcmp(buf, "MKD ", 1) || !strxcmp(buf, "XMKD ", 1)) {
		if (!buf[4]) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[4], parms->tmp3);
			response = do_mkdir(parms, path);
		}
	} else if (!strxcmp(buf, "RMD ", 1) || !strxcmp(buf, "XRMD ", 1)) {
		if (!buf[4]) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[4], parms->tmp3);
			response = do_rmdir(parms, path);
		}
	} else if (!strxcmp(buf, "DELE ", 1)) {
		if (!buf[5]) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[5], parms->tmp3);
			response = do_delete(parms, path);
		}
	} else if (!strxcmp(buf, "REST ", 1)) {
		char *p = &buf[5];
		int offset;
		//350 Restarting at 999. Send STORE or RETRIEVE to initiate transfer.
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
	} else {
		response = 500;
	}
got_response:
	if (response)
		kftpd_send_response(parms, response);
	return quit;
}

static unsigned char
fromhex (unsigned char x)
{
	if (INRANGE(x,'0','9'))
		return x - '0';
	x = TOUPPER(x);
	if (INRANGE(x, 'A', 'F'))
		return x - 'A';
	return 16;
}

static char *
khttpd_fix_hexcodes (char *buf)
{
	char *s = buf;
	unsigned char x1, x2;

	while (*s) {
		while (*s && *s++ != '%');
		if ((x1 = *s) && (x2 = *++s)) {
			if ((x1 = fromhex(x1)) < 16 && (x2 = fromhex(x2)) < 16) {
				int len = s - buf;
				*s = (x1 << 4) | x2;
				buf = s;
				while (len-- > 2) {
					--buf;
					*buf = *(buf - 2);
				}
				++s;
			}
		}
	}
	return buf;
}

static void
khttpd_handle_connection (server_parms_t *parms)
{
	char	*buf = parms->buf, *cmds = NULL, c, *path, *p, *x;
	int	response = 0, buflen = sizeof(parms->buf) - 1, n;
	int	use_index = 1;	// look for index.html?

	n = ksock_rw(parms->clientsock, buf, buflen, 0);
	if (n <= 0) {
		if (parms->verbose)
			printk("%s: receive failed: %d\n", parms->servername, n);
		return;
	}
	buf[n] = '\0';
	if (n >= buflen) {
		khttpd_respond(parms, 400, "Bad command", "request too long");
		return;
	}
	if (strxcmp(buf, "GET ", 1) || n < 4) {
		khttpd_respond(parms, 400, "Bad command", "server only supports GET");
		return;
	}
	p = path = &buf[4];
	// find path delimiter
	while ((c = *p) && c != ' ' && c != '\r' && c != '\n')
		++p;
	// HTTP "restart/resume" support:
	for (x = p; *x; ++x) {
		static const char Range[] = "\nRange: bytes=";
		static const char Icy[] = "\nIcy-MetaData:1";
		int start = 0, end = -1;
		if (*x == '\n') {
			if (!strxcmp(x, Icy, 1)) {
				parms->icy_metadata = 1;
			} else if (!strxcmp(x, Range, 1) && *(x += sizeof(Range) - 1)) {
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
	}
	*p = '\0'; // zero-terminate the GET line
	if (parms->verbose)
		printk("%s: GET '%s'\n", parms->servername, path);
	path = khttpd_fix_hexcodes(path);
	for (p = path; *p; ++p) {
		if (*p == '?') {
			cmds = p + 1;
			*p = '\0';	// zero-terminate the path portion
			break;
		}
	}
	if (!*path) {
		response = 404;
	} else {
		if (cmds && *cmds) {
			// translate '='/'+' into spaces, and '&' into ';'
			while ((c = *++p)) {
				if (c == '=' || c == '+')
					*p = ' ';
				else if (c == '&')
					*p = ';';
			}
			if (glob_match(path, "/drive?/fids/*")) {
				if (!strxcmp(cmds, ".html", 1))
					parms->generate_playlist = 1;
				else if (!strxcmp(cmds, ".m3u", 1))
					parms->generate_playlist = 2;
			}
			if (parms->generate_playlist) {
				response = send_playlist(parms, path);
				goto quit;
			}
			(void) hijack_do_command(cmds, p - cmds); // ignore errors
			use_index = 0;
		}
		n = strlen(path);
		if (path[n-1] == '/' && (!use_index || !strcpy(path+n, "index.html") || 1 != classify_path(path))) {
			path[n] = '\0';
			response = send_dirlist(parms, path, 1);
		} else
			response = send_file(parms, path);
	}
quit:
	if (response) {
		sprintf(buf, "(%d)", response);
		khttpd_respond(parms, 404, "Error retrieving file", buf);
	}
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

	lock_kernel();
	if ((parms->clientsock = sock_alloc())) {
		parms->clientsock->type = parms->servsock->type;
		if (parms->servsock->ops->dup(parms->clientsock, parms->servsock) < 0) {
			printk("%s: sock_accept: dup() failed\n", parms->servername);
		} else if ((rc = parms->clientsock->ops->accept(parms->servsock, parms->clientsock, parms->servsock->file->f_flags)) < 0) {
			printk("%s: sock_accept: accept() failed, rc=%d\n", parms->servername, rc);
		} else if (get_ipaddr(parms->clientsock, parms->clientip, 1) || get_ipaddr(parms->clientsock, parms->serverip, 0)) {
			printk("%s: sock_accept: get_ipaddr() failed\n", parms->servername);
		} else {
			unlock_kernel();
			return 0;	// success
		}
		sock_release(parms->clientsock);
	}
	unlock_kernel();
	return 1;	// failure
}

//static int zombies = 0;

static int
child_thread (void *arg)
{
	server_parms_t	*parms = arg;

	if (parms->verbose)
		printk("%s: %s connection from %s\n", parms->servername, parms->serverip, parms->clientip);
	(void) set_sockopt(parms, parms->servsock, SOL_TCP, TCP_NODELAY, 1); // don't care
	if (parms->use_http) {
		khttpd_handle_connection(parms);
	} else if (!kftpd_send_response(parms, 220)) {
		strcpy(parms->cwd, "/");
		parms->umask = 0022;
		while (!kftpd_handle_command(parms));
		sync();	// useful for flash upgrades
	}
	sock_release(parms->clientsock);
	free_pages((unsigned long)parms, 1);
	//++zombies;
	sys_exit(0);	// never returns
	return 0;
}

int
kftpd_daemon (unsigned long use_http)	// invoked twice from init/main.c
{
	server_parms_t		parms, *clientparms;
	int			server_port;
	struct semaphore	*sema;
	extern unsigned long	sys_signal(int, void *);

	if (sizeof(server_parms_t) > PAGE_SIZE)	// we allocate client parms as single pages
		return 0;
	memset(&parms, 0, sizeof(parms));

	if (use_http) {
		parms.servername = "khttpd";
		sema = &hijack_khttpd_startup_sem;
	} else {
		parms.servername = "kftpd";
		sema = &hijack_kftpd_startup_sem;
	}

	// kthread setup
	set_fs(KERNEL_DS);
	current->session = 1;
	current->pgrp = 1;
	strcpy(current->comm, parms.servername);
	sigfillset(&current->blocked);

	down(sema);	// wait for Hijack to get our port number from config.ini

	parms.end_offset = -1;
	if (use_http) {
		server_port	= hijack_khttpd_port;
		parms.verbose	= hijack_khttpd_verbose;
		parms.use_http	= 1;
	} else {
		server_port	= hijack_kftpd_control_port;
		parms.data_port	= hijack_kftpd_data_port;
		parms.verbose	= hijack_kftpd_verbose;
	}

	if (server_port && hijack_max_connections > 0) {
		//struct wait_queue *sleepq = NULL;
		//sleep_on_timeout(&sleepq, 3*HZ);	// snooze long enough for DHCP negotiations to finish
		if (make_socket(&parms, &parms.servsock, server_port)) {
			printk("%s: make_socket(port=%d) failed\n", parms.servername, server_port);
		} else if (parms.servsock->ops->listen(parms.servsock, 10) < 0) {	// queued=10
			printk("%s: listen(port=%d) failed\n", parms.servername, server_port);
		} else {
			int childcount = 0;
			printk("%s: listening on port %d\n", parms.servername, server_port);
			while (1) {
				int child;
				do {
					int status, flags = WUNTRACED | __WCLONE;
					//if (!zombies && childcount < hijack_max_connections)
					if (childcount < hijack_max_connections)
						flags |= WNOHANG;
					//else printk("%s: too many offspring, waiting\n", parms.servername);
					child = sys_wait4(-1, &status, flags, NULL);
					if (child > 0) {
						--childcount;
						//if (zombies > 0)	// paranoia
						//	--zombies;
					}
				//} while (child > 0 || zombies > 0);
				} while (child > 0);
				if (!ksock_accept(&parms)) {
					if (!(clientparms = (server_parms_t *)__get_free_pages(GFP_KERNEL,1))) {
						printk("%s: no memory for client parms\n", parms.servername);
						sock_release(parms.clientsock);
					} else {
						memcpy(clientparms, &parms, sizeof(parms));
						if (0 < kernel_thread(child_thread, clientparms, CLONE_FS|CLONE_FILES|CLONE_SIGHAND))
							++childcount;
					}
				}
			}
		}
	}
	return 0;
}

