// kftpd 0.1 by Mark Lord
//
// This version can only RETRieve files.  LIST and STOR are not implemented yet.

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
				printk("destination addr = %s:%d\n", first, port);
				return 0;
			}
		}
	}
	printk("bad destination addr\n");
	return -1;
}

// Adapted from net/socket.c::sys_accept()
struct socket *
ksock_accept (struct socket *sock, struct sockaddr *address, int *len)
{
	struct socket *newsock;

	lock_kernel();
	if (!(newsock = sock_alloc())) {
		printk("sock_accept: sock_alloc() failed\n");
	} else {
		newsock->type = sock->type;
		if (sock->ops->dup(newsock, sock) < 0) {
			printk("sock_accept: dup() failed\n");
		} else if (newsock->ops->accept(sock, newsock, sock->file->f_flags) < 0) {
			printk("sock_accept: accept() failed\n");
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

	//printk("sending: %s\n", response);
	strcpy(buf, response);
	strcat(buf, "\r\n");
	length = strlen(buf);
	if ((rc = ksock_rw(0, sock, buf, length, -1)) != length) {
		printk("ksock_rw(response) '%s' failed: %d\n", response, rc);
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
		printk("sock_create() failed, rc=%d\n", rc);
	} else {
		rc = sock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&turn_on, sizeof(turn_on));
		if (rc) {
			printk("setsockopt() failed, rc=%d\n", rc);
			sock_release(sock);
		} else {
			memset(&servaddr, 0, sizeof(servaddr));
			servaddr.sin_family	 = AF_INET;
			servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
			servaddr.sin_port        = htons(port);
			rc = sock->ops->bind(sock, (struct sockaddr *)&servaddr, sizeof(servaddr));
			if (rc) {
				printk("bind(port=%d) failed: %d\n", port, rc);
				sock_release(sock);
			} else {
				*sockp = sock;
			}
		}
	}
	return rc;
}

static struct socket *
open_datasock (struct socket *sock)
{
	int		rc;
	struct socket	*datasock = NULL;

	have_portaddr = 0;	// for next time
	if ((rc = make_socket(&datasock, SERVER_DATA_PORT))) {
		printk("make_socket(datasock) failed: %d\n", rc);
	} else {
		if (!send_response(sock, "150 opening data connection")) {
			int flags = 0;
			rc = datasock->ops->connect(datasock, (struct sockaddr *)&portaddr,
							sizeof(struct sockaddr_in), flags);
			if (!rc)
				return datasock;
			printk("connect(datasock) failed: %d\n", rc);
		}
		sock_release(datasock);
	}
	return NULL;
}

static const char *
send_file (struct socket *sock, const char *path)
{
	unsigned int	size;
	int		rc = 0;
	struct file	*filp;
	unsigned char	buf[PAGE_SIZE];
	mm_segment_t	old_fs;
	struct socket	*datasock;
	const char	*response = NULL;

	lock_kernel();
	filp = filp_open(path,O_RDONLY,0);
	if (IS_ERR(filp) || !filp) {
		printk("filp_open(%s) failed\n", path);
		response = "550 file not found";
	} else {
		if (!filp->f_dentry || !filp->f_dentry->d_inode) {
			response = "550 file read error";
		} else {
			size = filp->f_dentry->d_inode->i_size;
			filp->f_pos = 0;
			if (!(datasock = open_datasock(sock))) {
				response = "425 failed to open data connection";
			} else {
				old_fs = get_fs();
				set_fs(KERNEL_DS);
				while (size > 0) {
					unsigned int thistime = size;
					if (thistime > sizeof(buf))
						thistime = sizeof(buf);
					rc = filp->f_op->read(filp, buf, thistime, &(filp->f_pos));
					if (rc <= 0) {
						printk("filp->f_op->read() failed; rc=%d\n", rc);
						response = "451 error reading file";
						break;
					}
					size -= rc;
					if (thistime != ksock_rw(1, datasock, buf, thistime, -1)) {
						response = "426 error sending data; transfer aborted";
						break;
					}
				}
				set_fs(old_fs);
				sock_release(datasock);
			}
		}
		filp_close(filp,NULL);
	}
	unlock_kernel();
	return response;
}

static const char *
send_dirlist (struct socket *sock, char *path)
{
	int		rc, len;
	char		buf[256];
	struct socket	*datasock = NULL;
	const char	*response = NULL;

	if (!(datasock = open_datasock(sock))) {
		response = "425 failed to open data connection";
	} else {
		len = sprintf(buf, "LIST not implemented; path='%s'\r\n", path);
		if ((rc = ksock_rw(0, datasock, buf, len, -1)) != len) {
			printk("ksock_rw(datasock) failed\n");
			response = "426 error sending data; transfer aborted";
		}
		sock_release(datasock);
	}
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
	static char	xfer_mode = 'A';
	char		path[256], buf[256];
	const char	*response = NULL;
	int		n, rc, quit = 0;
	const char	ABOR[] = {0xf2,'A','B','O','R','\0'};

	n = ksock_rw(0, sock, buf, sizeof(buf), 0);
	if (n < 0) {
		printk("client request too short, ksock_rw() failed: %d\n", n);
		return -1;
	} else if (n == 0) {
		printk("EOF on client sock\n");
		return -1;
	}
	if (n < 5 || n >= sizeof(buf)) {
		response = "501 bad command length";
	} else if (buf[--n] != '\n' || buf[--n] != '\r') {
		printk("EOL not found\n");
		response = "500 bad end of line";
	} else {
		buf[n] = '\0';	// overwrite '\r'
		printk("'%s'\n", buf);
		if (!strcasecmp(buf, "QUIT")) {
			quit = 1;
			response = "221 happy fishing";
		} else if (!strncasecmp(buf, "USER ", 5)) {
			response = "230 user logged in";
		} else if (!strncasecmp(buf, "PASS ", 5)) {
			response = "230 password okay";
		} else if (!strncasecmp(buf, "SYST", 4)) {
			response = "215 UNIX Type: L8";
		} else if (!strcasecmp(buf, "STRU F")) {
			response = "200 stru-f okay";
		} else if (!strncasecmp(buf, "TYPE ", 5)) {
			if (buf[5] != 'A' && buf[5] != 'I') {
				response = "501 unsupported xfer TYPE";
			} else {
				xfer_mode = buf[5];
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
		} else if (!strncasecmp(buf, "LIST", 4)
		//	|| !strncasecmp(buf, "STOR", 4)		// FIXME
			|| !strncasecmp(buf, "RETR", 4)) {
			strcpy(path, cwd);
			if (buf[0] != 'L' && (buf[4] != ' ' || !buf[5])) {
				response = "501 missing path";
			} else {
				if (buf[4] == ' ') {
					buf[4] = '\0';
					append_path(path, &buf[5]);
				}
				if (buf[4]) {
					response = "500 bad command";
				} else if (!have_portaddr) {
					response = "425 no PORT specified";
				} else {
					if (buf[0] == 'R')
						response = send_file(sock, path);
					else
						response = send_dirlist(sock, path);
					if (!response)
						response = "226 transmission completed";
				}
			}

		} else {
			printk("%02x - %s\n", buf[0], &buf[1]);
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
		printk("not AF_INET: %d\n", (int)clientaddr->sin_family);
	} else if (inet_ntop(AF_INET, &clientaddr->sin_addr.s_addr, clientip, sizeof(clientip)) == NULL) {
		printk("inet_ntop(%08x) failed\n", (uint32_t)clientaddr->sin_addr.s_addr);
	} else if (!send_response(sock, "220 connected")) {
		strcpy(cwd, "/");
		while (handle_command(sock) == 0);
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
		printk("make_socket(port=%d) failed, rc=%d\n", SERVER_CONTROL_PORT, rc);
		return 0;
	}
	rc = servsock->ops->listen(servsock, 1); /* allow only one client at a time */
	if (rc < 0) {
		printk("listen(port=%d) failed, rc=%d\n", SERVER_CONTROL_PORT, rc);
		return 0;
	}
	while (1) {
		struct socket		*clientsock;
		struct sockaddr_in	clientaddr;
		int			clientaddr_len = sizeof(clientaddr);

		printk("waiting for connections\n");
		clientsock = ksock_accept(servsock, (struct sockaddr *)&clientaddr, &clientaddr_len);
		if (!clientsock) {
			printk("accept() failed\n");
		} else {
			printk("new connection\n");
			handle_connection(clientsock, &clientaddr);
		}
	}
	return 0;
}

