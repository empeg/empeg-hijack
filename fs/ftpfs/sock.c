#define __NO_VERSION__


#include<linux/version.h>
#include<linux/config.h>
#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/socket.h>
#include<linux/sched.h>
#include<linux/fcntl.h>
#include<linux/errno.h>
#include<linux/in.h>
#include<linux/net.h>
#include<linux/mm.h>

#include<net/scm.h>
#include<net/ip.h>

#include<asm/uaccess.h>

#include"ftpfs.h"

int
sock_send(struct socket *sock, const void *buf, int len){
	struct iovec iov;
	struct msghdr msg;
	struct scm_cookie scm;
	int err;
	mm_segment_t fs;

	fs = get_fs();
	set_fs(get_ds());

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;
	
	iov.iov_base = (void*)buf;
	iov.iov_len = len;

	err = scm_send(sock, &msg, &scm);
	if (err >=0){
		err = sock->ops->sendmsg(sock, &msg, len, &scm);
		scm_destroy(&scm);
	}

	set_fs(fs);
	return err;
}

int
sock_recv(struct socket *sock, unsigned char *buf, int size, unsigned flags){
	struct iovec iov;
	struct msghdr msg;
	struct scm_cookie scm;
	mm_segment_t fs;

	fs = get_fs();
	set_fs(get_ds());
	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;

	iov.iov_base = buf;
	iov.iov_len = size;
	memset(&scm, 0, sizeof(scm));
	size =  sock->ops->recvmsg(sock, &msg, size, flags, &scm);
	if(size>=0)
		scm_recv(sock, &msg, &scm, flags);
	set_fs(fs);
	return size;
}
