/* empeg RDS support
 *
 * (C) 1999/2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Hugo Fiennes, <hugo@empeg.com>
 *   John Ripley, <john@empeg.com>
 *
 * RDS data comes into the empeg via GPIO17 (data) and GPIO3 (clock)
 * at 1187 bits/sec.
 *
 * The decoder is very simple at the moment: we just decode the frames, and
 * when we've had an A, B, C/C' and D frame we put the data in the buffer.
 * The large amount of CRC compared to data (10 bits of CRC to 16 bits of data)
 * means that we can correct quite large error bursts.
 *
 * 19990818 john@empeg.com
 *	CRC correction done. Seems a single 1-5 bit error burst is the most we
 *      can correct for without data corruption - the likelihood of two 5 bit
 *      bursts vs completely garbled is such that we should ditch those
 *      packets.
 *
 * 19991113 hugo@empeg.com
 *      Unncessary interrupt disabling when a valid RDS packet was buffered
 *      removed
 *
 * 20000304 hugo@empeg.com
 *      Initialisation code marked as discardable
 */

#define __KERNEL_SYSCALLS__

#include <linux/config.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/major.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/init.h>

#include <linux/fs.h>
#include <linux/unistd.h>

#include <asm/byteorder.h>
#include <asm/irq.h>
#include <asm/fiq.h>
#include <asm/segment.h>
#include <asm/io.h>
#include <asm/hardware.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>

/* For the userspace interface */
#include <linux/empeg.h>

#include "empeg_rds.h"

/* Only one RDS channel */
struct rds_dev rds_devices[1];

/* Add a second rds Device just for reading */
int secondRDSDev = -1;

/* Logging for proc */
static char log[1024];
static int log_size=0;
#define LOG(x) { if (log_size<1023) log[log_size++]=(x); }
#define LOGS(x) { char *p=(x); while(*p) { LOG(*p); p++; } }

static struct file_operations rds_fops = {
  NULL, /* rds_lseek */
  rds_read,
  rds_write,
  NULL, /* rds_readdir */
  rds_poll,
  rds_ioctl,
  NULL, /* rds_mmap */
  rds_open,
  NULL, /* rds_flush */
  rds_release,
};

/* Our own IRQs are disabled as we're being called from our IRQ handler */
static void rds_processpacket(struct rds_dev *dev)
{
	int a,last_used=dev->rx_used;
	char buffer[64];
	
	/* Got a whole packet, log it and buffer */
	sprintf(buffer,"pkt %02x/%02x/%02x/%02x %02x/%02x/%02x/%02x\n",
		dev->buffer[0],dev->buffer[1],dev->buffer[2],dev->buffer[3],
		dev->buffer[4],dev->buffer[5],dev->buffer[6],dev->buffer[7]);
	LOGS(buffer);

	/* Put it into the buffer */
	if (dev->rx_free<8) {
		/* No room! */
		return;
	}

	dev->rx_free-=8;
	dev->rx_used+=8;
	dev->rx_count+=8;
	for(a=0;a<8;a++) {
		dev->rx_buffer[dev->rx_head++]=dev->buffer[a];
		if (dev->rx_head==RDS_RX_BUFFER_SIZE) dev->rx_head=0;
	}

	/* If we've filled an empty buffer, wake up readers */
	if (!last_used) wake_up_interruptible(&dev->rx_wq);
}

static int rds_crc_correct(struct rds_dev *dev, int rdsbits)
{
	static const unsigned crc_correction[1024] = {
		0x00000000, 0x00000001, 0x00000002, 0x00000003, 
		0x00000004, 0xffffffff, 0x00000006, 0x00000007, 
		0x00000008, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x0000000c, 0xffffffff, 0x0000000e, 0x0000000f, 
		0x00000010, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00000018, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x0000001c, 0xffffffff, 0x0000001e, 0x0000001f, 
		0x00000020, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00300000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x00001800, 
		0x00000030, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x00010000, 
		0x00000038, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x0000003c, 0xffffffff, 0x0000003e, 0xffffffff, 
		0x00000040, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00600000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00030000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00003000, 0xffffffff, 
		0x00000060, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00020000, 0xffffffff, 
		0x00000070, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x02000000, 
		0x00000078, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x0000007c, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00000080, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00070000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00f00000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x00007800, 
		0xffffffff, 0x00003e00, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00c00000, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00060000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00006000, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x000000c0, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x00000e00, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00040000, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x000000e0, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x000000f0, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x000000f8, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00000100, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x000e0000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x0001f000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x01e00000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x0003c000, 0xffffffff, 0x003e0000, 
		0xffffffff, 0xffffffff, 0x0000f000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00007c00, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00f80000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x000f0000, 0xffffffff, 0x03800000, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x01800000, 0xffffffff, 0xffffffff, 0x00000f80, 
		0xffffffff, 0x003c0000, 0xffffffff, 0x0001c000, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x000c0000, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x03e00000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x0000c000, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00000180, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00380000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00001c00, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00080000, 0x00000400, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x000001c0, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x00000f00, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x000001e0, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x000001f0, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x0007c000, 0xffffffff, 0xffffffff, 
		0x00000200, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x001c0000, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x0003e000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x03c00000, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00000780, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00078000, 0x01c00000, 
		0xffffffff, 0xffffffff, 0x007c0000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x0001e000, 0x001f0000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x0000f800, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x01f00000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x000007c0, 0x001e0000, 0x0000e000, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x03000000, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00003c00, 0x00001f00, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00780000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00038000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00000700, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x00008000, 
		0x00180000, 0xffffffff, 0xffffffff, 0x00000c00, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x01000000, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00018000, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00000300, 0xffffffff, 0xffffffff, 0x00002000, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x00400000, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00700000, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00003800, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00200000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00001000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00100000, 0xffffffff, 0x00000800, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x00000380, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x00001e00, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x00800000, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0x00000600, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0x00004000, 
		0x000003c0, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0x000003e0, 0x00007000, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 
		0xffffffff, 0xffffffff, 0x000f8000, 0xffffffff, 
		0xffffffff, 0x00e00000, 0xffffffff, 0xffffffff, 
	};

	int result, crc;
	
	crc = rdsbits & 0x3ff;
	if(!crc) {
		// we wouldn't be here in this case?
		//		dev->good_packets++;
		return rdsbits;
	}

	if(crc_correction[crc] == 0xffffffff) {
		dev->bad_packets++;
		return rdsbits;
	}

	result = rdsbits ^ crc_correction[crc];
	dev->recovered_packets++;
	
	return result;
}

static __inline__ void rds_processbit_cooked(struct rds_dev *dev, int rdsbit)
{
	static char tmp[256];
	unsigned int rdsdata,rdscrc,b,correct,correct2;

	static const unsigned int g[]={ 0x1b9, 0x372, 0x35d, 0x303,
					0x3bf, 0x2c7, 0x037, 0x06e,
					0x0dc, 0x1b8, 0x370, 0x359,
					0x30b, 0x3af, 0x2e7, 0x077 };

	/* Stick into shift register */
	dev->rdsstream=((dev->rdsstream<<1)|rdsbit)&0x03ffffff;
	dev->bitsinfifo++;

	/* A FIFOful? If not, we can't process packets */
	if (dev->bitsinfifo<26) return;
	
	/* Calculate checksum on this fifo */
	rdsdata=dev->rdsstream&0x03fffc00;
	rdscrc=dev->rdsstream&0x3ff;
	for(b=0;b<16;b++)
	    if (dev->rdsstream&(1<<(b+10))) rdscrc^=g[b];

	switch(dev->state) {
	case 0: /* Look for type A as start - we're not in sync, so we can't correct */
		if (rdscrc!=RDS_OFFSETA) {
			correct = rds_crc_correct(dev, rdsdata|(rdscrc^RDS_OFFSETA));
			if(correct & 0x3ff) {
				dev->sync_lost_packets++;
				break;
			}
		}
		else dev->good_packets++;

		/* Save type A packet in buffer */
		rdsdata>>=10;
		dev->buffer[0]=(rdsdata>>8);
		dev->buffer[1]=(rdsdata&0xff);
		dev->state=2;
		dev->bitsinfifo=0;
		dev->badcrccount=0;
		dev->discardpacket=0;
		break;

	case 1: /* Type A: we are in sync now */
		if (rdscrc!=RDS_OFFSETA) {
			correct = rds_crc_correct(dev, rdsdata|(rdscrc^RDS_OFFSETA));
			dev->badcrccount++;
			if(correct & 0x3ff) {
				dev->discardpacket++;
				dev->state ++;
				dev->bitsinfifo=0;
				break;
			}
			rdsdata = correct & 0x03fffc00;
		}
		else dev->good_packets++;

		/* Save type A packet in buffer */
		rdsdata>>=10;
		dev->buffer[0]=(rdsdata>>8);
		dev->buffer[1]=(rdsdata&0xff);
		dev->state++;
		dev->bitsinfifo=0;
		break;

	case 2: /* Waiting for type B */
		if (rdscrc!=RDS_OFFSETB) {
			dev->badcrccount++;
			correct = rds_crc_correct(dev, rdsdata|(rdscrc^RDS_OFFSETB));
			if(correct & 0x3ff) {
				/* Corrupted data */
				dev->discardpacket++;
				dev->state++;
				dev->bitsinfifo=0;
				break;
			}
			rdsdata = correct & 0x03fffc00;
		}
		else dev->good_packets++;

		/* Save type B packet in buffer */
		rdsdata>>=10;
		dev->buffer[2]=(rdsdata>>8);
		dev->buffer[3]=(rdsdata&0xff);
		dev->state++;
		dev->bitsinfifo=0;
		break;
		
	case 3: /* Waiting for type C or C' */
		if (rdscrc!=RDS_OFFSETC && rdscrc!=RDS_OFFSETCP) {
			dev->badcrccount++;
			correct = rds_crc_correct(dev, rdsdata|(rdscrc^RDS_OFFSETC));
			correct2 = rds_crc_correct(dev, rdsdata|(rdscrc^RDS_OFFSETCP));
			
			if(((correct & 0x3ff) != 0) && ((correct2 & 0x3ff) != 0)) {
				/* Corrupted data */
				dev->discardpacket++;
				dev->state++;
				dev->bitsinfifo=0;
				break;
			}
			rdsdata = correct & 0x03fffc00;
		}
		else dev->good_packets++;

		/* Save type C packet in buffer */
		rdsdata>>=10;
		dev->buffer[4]=(rdsdata>>8);
		dev->buffer[5]=(rdsdata&0xff);
		dev->state++;
		dev->bitsinfifo=0;
		break;
		
	case 4: /* Waiting for type D */
		if (rdscrc!=RDS_OFFSETD) {
			correct = rds_crc_correct(dev, rdsdata|(rdscrc^RDS_OFFSETD));
			dev->badcrccount++;
			if(correct & 0x3ff) {
				/* Corrupted data */
				dev->discardpacket++;
				dev->state=1;
				dev->bitsinfifo=0;
				break;
			}
			rdsdata = correct & 0x03fffc00;
		}
		else dev->good_packets++;

		/* Save type D packet in buffer */
		rdsdata>>=10;
		dev->buffer[6]=(rdsdata>>8);
		dev->buffer[7]=(rdsdata&0xff);
		
		/* Buffer it if all segments were ok */
		if (!dev->discardpacket)
			rds_processpacket(dev);

		/* We're still in sync, so back to state 1 */
		dev->state=1;
		dev->bitsinfifo=0;
		dev->discardpacket=0;
		break;
	}

	/* Lots of errors? If so, go back to resync mode */
	if (dev->discardpacket>=4) {
		dev->bitsinfifo=26;	// resync a bit faster
		dev->state=0;
	}

	sprintf(tmp, "Sync: %d  Good: %d  Bad: %d  Ugly: %d\n",
		dev->sync_lost_packets, dev->good_packets,
		dev->bad_packets, dev->recovered_packets);
	LOGS(tmp);
}

static __inline__ void rds_processbit_raw(struct rds_dev *dev, int rdsbit)
{
	dev->buffer[dev->bitsinfifo >> 3] |= rdsbit << (dev->bitsinfifo & 7);

	if(++dev->bitsinfifo < 64) return;
	dev->bitsinfifo = 0;
	rds_processpacket(dev);
	memset(dev->buffer, 0, sizeof(dev->buffer));
}

static __inline__ void rds_processbit(struct rds_dev *dev, int rdsbit)
{
	if(!dev->in_use) return;

	if(dev->interface == EMPEG_RDS_INTERFACE_COOKED)
		rds_processbit_cooked(dev, rdsbit);
	else if(dev->interface == EMPEG_RDS_INTERFACE_RAW)
		rds_processbit_raw(dev, rdsbit);
}

void rds_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct rds_dev *dev=rds_devices;

	/* Get bit and feed it into state machine */
	rds_processbit(dev,(GPLR&EMPEG_RDSDATA)?0:1);
}

static int rds_read_procmem(char *buf, char **start, off_t offset, int len, int unused)
{
	struct rds_dev *dev = rds_devices;
	len = 0;

	LOG(0);
	log[1023] = 0;
	len+=sprintf(buf+len,"Log: %s\n",log);
	log_size=0;
	
	return len;
}

static struct proc_dir_entry rds_proc_entry = {
	0,			/* inode (dynamic) */
	9, "empeg_rds",  	/* length and name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	0, 			/* size */
	NULL, 			/* use default operations */
	&rds_read_procmem, 	/* function used to read data */
};

/* Device initialisation */
void __init empeg_rds_init(void)
{
        struct rds_dev *dev=rds_devices;
	int result;
	
	/* Allocate buffers */
	dev->rx_buffer=vmalloc(RDS_RX_BUFFER_SIZE);
	if (!dev->rx_buffer) {
		printk(KERN_WARNING "Could not allocate memory for RDS buffer\n");
		return;
	}

	/* Initialise buffer bits */
	dev->rx_head=dev->rx_tail=0;
	dev->rx_used=0; dev->rx_free=RDS_RX_BUFFER_SIZE;
	dev->rx_wq = NULL;

	/* Set up correct RDS source */
	GRER|=EMPEG_RDSCLOCK;
	GFER&=~EMPEG_RDSCLOCK;
	GEDR=EMPEG_RDSCLOCK;
	
	/* Claim RDS clock IRQ */
	result=request_irq(3,rds_interrupt,0,"empeg_rdsirq",dev);

	/* Got it ok? */
	if (result!=0) {
		printk(KERN_ERR "Can't get empeg RDSIRQ IRQ 3.\n");
		return;
	}

	/* Initialise state machine */
	dev->interface = EMPEG_RDS_INTERFACE_COOKED;
	dev->state=0;
	dev->bitsinfifo=0;
	dev->in_use=0;

	/* Dad's home! */
	printk("empeg RDS driver initialised\n");

	/* Get the device */
	result=register_chrdev(EMPEG_RDS_MAJOR,"empeg_rds",&rds_fops);
	if (result<0) {
		printk(KERN_WARNING "empeg RDS: Major number %d unavailable.\n",
		       EMPEG_RDS_MAJOR);
		return;
	}

#ifdef CONFIG_PROC_FS
	proc_register(&proc_root, &rds_proc_entry);
#endif
}

static int rds_open(struct inode *inode, struct file *flip)
{
        mm_segment_t fs;
	struct rds_dev *dev=rds_devices;
	
	MOD_INC_USE_COUNT;
	flip->private_data=dev;

	dev->good_packets=0;
	dev->recovered_packets=0;
	dev->bad_packets=0;
	dev->sync_lost_packets = 0;
	dev->in_use++;

        /* Write data to the second RDS device */
        fs = get_fs();
        set_fs(KERNEL_DS);	//set_fs(get_ds());
        if (secondRDSDev>=0) {
          close(secondRDSDev);
          secondRDSDev=-1;
        }
        secondRDSDev = open("/dev/rds1", O_WRONLY|O_NONBLOCK, 0);
        set_fs(fs);
        
	return 0;
}

static int rds_release(struct inode *inode, struct file *flip)
{
	struct rds_dev *dev=rds_devices;
	dev->in_use--;
	
	MOD_DEC_USE_COUNT;
	return 0;
}

/* Read data from RDS buffer */
ssize_t rds_read(struct file *flip, char *dest, size_t count, loff_t *ppos)
{
        mm_segment_t fs;
        char* rdscopy=dest;
	struct rds_dev *dev=flip->private_data;
	unsigned long flags;
	size_t bytes;

	while (dev->rx_used==0) {
		if (flip->f_flags & O_NONBLOCK)
			return -EAGAIN;
      
		interruptible_sleep_on(&dev->rx_wq);
		/* If the sleep was terminated by a signal give up */
		if (signal_pending(current))
			return -ERESTARTSYS;
	}

	/* Protect this bit */
	save_flags_cli(flags);
	if (count > dev->rx_used) count = dev->rx_used;
	dev->rx_used -= count;
	restore_flags(flags);

	bytes = count;
	while (bytes--) {
		*dest++ = dev->rx_buffer[dev->rx_tail++];
		if (dev->rx_tail==RDS_RX_BUFFER_SIZE)
			dev->rx_tail=0;
	}

	/* Protect this bit */
	save_flags_cli(flags);
	dev->rx_free += count;
	restore_flags(flags);

        if (secondRDSDev>=0) {
          fs = get_fs();
          set_fs(KERNEL_DS);	//set_fs(get_ds());
          write(secondRDSDev, rdscopy, count);
          set_fs(fs);
        }
                
	return count;
}

/* Write data to the RDS buffer */
static ssize_t rds_write(struct file *filp, const char *buf, size_t count,
			 loff_t *ppos)
{
	/* Only radio stations can write RDS */
	return -EINVAL;
}

static unsigned int rds_poll(struct file *filp, poll_table *wait)
{
	struct rds_dev *dev = filp->private_data;
	unsigned int mask = 0;

	poll_wait(filp, &dev->rx_wq, wait);

	/* Is there stuff in the read buffer? */
	if (dev->rx_used)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static int rds_ioctl(struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct rds_dev *dev = filp->private_data;
	if (_IOC_TYPE(cmd) != EMPEG_RDS_MAGIC)
		return -EINVAL;
	
	switch(cmd) {
	case EMPEG_RDS_GET_INTERFACE:
		copy_to_user_ret((void *) arg, &dev->interface, sizeof(int),
				 -EFAULT);
		return 0;

	case EMPEG_RDS_SET_INTERFACE:
		{
			int val;
			copy_from_user_ret(&val, (void *) arg, sizeof(int),
					   -EFAULT);
			if(val != EMPEG_RDS_INTERFACE_COOKED &&
			   val != EMPEG_RDS_INTERFACE_RAW)
				return -EINVAL;

			dev->interface = val;
			dev->state = 0;
			dev->bitsinfifo = 0;
		}
		return 0;

	default:
		return -EINVAL;
	}
}

#ifdef ALICE_IN_USERLAND
/* This is some basic test code for userland. It's very short, and so is included here
   for the benefit of people possibly wanting to do something with the RDS output.
   Passed a buffer, this decodes RDS data */
void decode_rds(unsigned char *buffer)
{
	static char station[]="        ";
	static char radiotext[]="                                                                ";
	static int h=0,m=0,o=0;
	char *p;
	
	switch(buffer[2]>>4) {
	case 0: /* Type 0: Station name */
		p=station+((buffer[3]&3)<<1);
		*p++=buffer[6];
		*p++=buffer[7];
		break;
		
	case 2: /* Type 2: Radiotext */
		p=radiotext+((buffer[3]&0xf)<<2);
		*p++=buffer[4];
		*p++=buffer[5];
		*p++=buffer[6];
		*p++=buffer[7];
		break;
		
	case 4: /* Type 4: Clock/time */
		h=((buffer[5]&1)<<4)|(buffer[6]>>4);
		m=((buffer[6]&0xf)<<2)|(buffer[7]>>6);
		o=(buffer[7]&0x1f);
		if (buffer[7]&0x20) o=-o;
		break;
		
	}
	
	printf("PI=%02x%02x Time=%02d:%02d/%d ID='%s'\nRT='%s'\n",buffer[0],buffer[1],h,m,o,station,radiotext);
}
#endif
