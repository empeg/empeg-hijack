/*
 * $Id: c4.c,v 1.5 2000/03/16 15:21:03 calle Exp $
 * 
 * Module for AVM C4 card.
 * 
 * (c) Copyright 1999 by Carsten Paeth (calle@calle.in-berlin.de)
 * 
 * $Log: c4.c,v $
 * Revision 1.5  2000/03/16 15:21:03  calle
 * Bugfix in c4_remove: loop 5 times instead of 4 :-(
 *
 * Revision 1.4  2000/02/02 18:36:03  calle
 * - Modules are now locked while init_module is running
 * - fixed problem with memory mapping if address is not aligned
 *
 * Revision 1.3  2000/01/25 14:37:39  calle
 * new message after successfull detection including card revision and
 * used resources.
 *
 * Revision 1.2  2000/01/21 20:52:58  keil
 * pci_find_subsys as local function for 2.2.X kernel
 *
 * Revision 1.1  2000/01/20 10:51:37  calle
 * Added driver for C4.
 *
 *
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/capi.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include "capicmd.h"
#include "capiutil.h"
#include "capilli.h"
#include "avmcard.h"

static char *revision = "$Revision: 1.5 $";

#undef CONFIG_C4_DEBUG
#undef CONFIG_C4_POLLDEBUG

/* ------------------------------------------------------------- */

#ifndef PCI_VENDOR_ID_DEC
#define PCI_VENDOR_ID_DEC	0x1011
#endif

#ifndef PCI_DEVICE_ID_DEC_21285
#define PCI_DEVICE_ID_DEC_21285	0x1065
#endif

#ifndef PCI_VENDOR_ID_AVM
#define PCI_VENDOR_ID_AVM	0x1244
#endif

#ifndef PCI_DEVICE_ID_AVM_C4
#define PCI_DEVICE_ID_AVM_C4	0x0800
#endif

/* ------------------------------------------------------------- */

static int suppress_pollack = 0;

MODULE_AUTHOR("Carsten Paeth <calle@calle.in-berlin.de>");

MODULE_PARM(suppress_pollack, "0-1i");

/* ------------------------------------------------------------- */

static struct capi_driver_interface *di;

/* ------------------------------------------------------------- */

static void c4_dispatch_tx(avmcard *card);

/* ------------------------------------------------------------- */

#define DC21285_DRAM_A0MR	0x40000000
#define DC21285_DRAM_A1MR	0x40004000
#define DC21285_DRAM_A2MR	0x40008000
#define DC21285_DRAM_A3MR	0x4000C000

#define	CAS_OFFSET	0x88

#define DC21285_ARMCSR_BASE	0x42000000

#define	PCI_OUT_INT_STATUS	0x30
#define	PCI_OUT_INT_MASK	0x34
#define	MAILBOX_0		0x50
#define	MAILBOX_1		0x54
#define	MAILBOX_2		0x58
#define	MAILBOX_3		0x5C
#define	DOORBELL		0x60
#define	DOORBELL_SETUP		0x64

#define CHAN_1_CONTROL		0x90
#define CHAN_2_CONTROL		0xB0
#define DRAM_TIMING		0x10C
#define DRAM_ADDR_SIZE_0	0x110
#define DRAM_ADDR_SIZE_1	0x114
#define DRAM_ADDR_SIZE_2	0x118
#define DRAM_ADDR_SIZE_3	0x11C
#define	SA_CONTROL		0x13C
#define	XBUS_CYCLE		0x148
#define	XBUS_STROBE		0x14C
#define	DBELL_PCI_MASK		0x150
#define DBELL_SA_MASK		0x154

#define SDRAM_SIZE		0x1000000

/* ------------------------------------------------------------- */

#define	MBOX_PEEK_POKE		MAILBOX_0

#define DBELL_ADDR		0x01
#define DBELL_DATA		0x02
#define DBELL_RNWR		0x40
#define DBELL_INIT		0x80

/* ------------------------------------------------------------- */

#define	MBOX_UP_ADDR		MAILBOX_0
#define	MBOX_UP_LEN		MAILBOX_1
#define	MBOX_DOWN_ADDR		MAILBOX_2
#define	MBOX_DOWN_LEN		MAILBOX_3

#define	DBELL_UP_HOST		0x00000100
#define	DBELL_UP_ARM		0x00000200
#define	DBELL_DOWN_HOST		0x00000400
#define	DBELL_DOWN_ARM		0x00000800
#define	DBELL_RESET_HOST	0x40000000
#define	DBELL_RESET_ARM		0x80000000

/* ------------------------------------------------------------- */

#define	DRAM_TIMING_DEF		0x001A01A5
#define DRAM_AD_SZ_DEF0		0x00000045
#define DRAM_AD_SZ_NULL		0x00000000

#define SA_CTL_ALLRIGHT		0x64AA0271

#define	INIT_XBUS_CYCLE		0x100016DB
#define	INIT_XBUS_STROBE	0xF1F1F1F1

/* ------------------------------------------------------------- */

#define	RESET_TIMEOUT		(15*HZ)	/* 15 sec */
#define	PEEK_POKE_TIMEOUT	(HZ/10)	/* 0.1 sec */

/* ------------------------------------------------------------- */

#define c4outmeml(addr, value)	writel(value, addr)
#define c4inmeml(addr)	readl(addr)
#define c4outmemw(addr, value)	writew(value, addr)
#define c4inmemw(addr)	readw(addr)
#define c4outmemb(addr, value)	writeb(value, addr)
#define c4inmemb(addr)	readb(addr)

/* ------------------------------------------------------------- */

static inline int wait_for_doorbell(avmcard *card, unsigned long t)
{
	unsigned long stop;

	stop = jiffies + t;
	while (c4inmeml(card->mbase+DOORBELL) != 0xffffffff) {
		if (!time_before(jiffies, stop))
			return -1;
	}
	return 0;
}

static int c4_poke(avmcard *card,  unsigned long off, unsigned long value)
{

	if (wait_for_doorbell(card, HZ/10) < 0)
		return -1;
	
	c4outmeml(card->mbase+MBOX_PEEK_POKE, off);
	c4outmeml(card->mbase+DOORBELL, DBELL_ADDR);

	if (wait_for_doorbell(card, HZ/10) < 0)
		return -1;

	c4outmeml(card->mbase+MBOX_PEEK_POKE, value);
	c4outmeml(card->mbase+DOORBELL, DBELL_DATA | DBELL_ADDR);

	return 0;
}

static int c4_peek(avmcard *card,  unsigned long off, unsigned long *valuep)
{
	if (wait_for_doorbell(card, HZ/10) < 0)
		return -1;

	c4outmeml(card->mbase+MBOX_PEEK_POKE, off);
	c4outmeml(card->mbase+DOORBELL, DBELL_RNWR | DBELL_ADDR);

	if (wait_for_doorbell(card, HZ/10) < 0)
		return -1;

	*valuep = c4inmeml(card->mbase+MBOX_PEEK_POKE);

	return 0;
}

/* ------------------------------------------------------------- */

static int c4_load_t4file(avmcard *card, capiloaddatapart * t4file)
{
	__u32 val;
	unsigned char *dp;
	int left, retval;
	__u32 loadoff = 0;

	dp = t4file->data;
	left = t4file->len;
	while (left >= sizeof(__u32)) {
	        if (t4file->user) {
			retval = copy_from_user(&val, dp, sizeof(val));
			if (retval)
				return -EFAULT;
		} else {
			memcpy(&val, dp, sizeof(val));
		}
		if (c4_poke(card, loadoff, val)) {
			printk(KERN_ERR "%s: corrupted firmware file ?\n",
					card->name);
			return -EIO;
		}
		left -= sizeof(__u32);
		dp += sizeof(__u32);
		loadoff += sizeof(__u32);
	}
	if (left) {
		val = 0;
		if (t4file->user) {
			retval = copy_from_user(&val, dp, left);
			if (retval)
				return -EFAULT;
		} else {
			memcpy(&val, dp, left);
		}
		if (c4_poke(card, loadoff, val)) {
			printk(KERN_ERR "%s: corrupted firmware file ?\n",
					card->name);
			return -EIO;
		}
	}
	return 0;
}

/* ------------------------------------------------------------- */

static inline void _put_byte(void **pp, __u8 val)
{
	__u8 *s = *pp;
	*s++ = val;
	*pp = s;
}

static inline void _put_word(void **pp, __u32 val)
{
	__u8 *s = *pp;
	*s++ = val & 0xff;
	*s++ = (val >> 8) & 0xff;
	*s++ = (val >> 16) & 0xff;
	*s++ = (val >> 24) & 0xff;
	*pp = s;
}

static inline void _put_slice(void **pp, unsigned char *dp, unsigned int len)
{
	unsigned i = len;
	_put_word(pp, i);
	while (i-- > 0)
		_put_byte(pp, *dp++);
}

static inline __u8 _get_byte(void **pp)
{
	__u8 *s = *pp;
	__u8 val;
	val = *s++;
	*pp = s;
	return val;
}

static inline __u32 _get_word(void **pp)
{
	__u8 *s = *pp;
	__u32 val;
	val = *s++;
	val |= (*s++ << 8);
	val |= (*s++ << 16);
	val |= (*s++ << 24);
	*pp = s;
	return val;
}

static inline __u32 _get_slice(void **pp, unsigned char *dp)
{
	unsigned int len, i;

	len = i = _get_word(pp);
	while (i-- > 0) *dp++ = _get_byte(pp);
	return len;
}

/* ------------------------------------------------------------- */

static void c4_reset(avmcard *card)
{
	unsigned long stop;

	c4outmeml(card->mbase+DOORBELL, DBELL_RESET_ARM);

	stop = jiffies + HZ*10;
	while (c4inmeml(card->mbase+DOORBELL) != 0xffffffff) {
		if (!time_before(jiffies, stop))
			return;
		c4outmeml(card->mbase+DOORBELL, DBELL_ADDR);
	}

	c4_poke(card, DC21285_ARMCSR_BASE + CHAN_1_CONTROL, 0);
	c4_poke(card, DC21285_ARMCSR_BASE + CHAN_2_CONTROL, 0);
}

/* ------------------------------------------------------------- */

static int c4_detect(avmcard *card)
{
	unsigned long stop, dummy;

	c4outmeml(card->mbase+PCI_OUT_INT_MASK, 0x0c);
	if (c4inmeml(card->mbase+PCI_OUT_INT_MASK) != 0x0c)
		return	1;

	c4outmeml(card->mbase+DOORBELL, DBELL_RESET_ARM);

	stop = jiffies + HZ*10;
	while (c4inmeml(card->mbase+DOORBELL) != 0xffffffff) {
		if (!time_before(jiffies, stop))
			return 2;
		c4outmeml(card->mbase+DOORBELL, DBELL_ADDR);
	}

	c4_poke(card, DC21285_ARMCSR_BASE + CHAN_1_CONTROL, 0);
	c4_poke(card, DC21285_ARMCSR_BASE + CHAN_2_CONTROL, 0);

	c4outmeml(card->mbase+MAILBOX_0, 0x55aa55aa);
	if (c4inmeml(card->mbase+MAILBOX_0) != 0x55aa55aa) return 3;

	c4outmeml(card->mbase+MAILBOX_0, 0xaa55aa55);
	if (c4inmeml(card->mbase+MAILBOX_0) != 0xaa55aa55) return 4;

	if (c4_poke(card, DC21285_ARMCSR_BASE+DBELL_SA_MASK, 0)) return 5;
	if (c4_poke(card, DC21285_ARMCSR_BASE+DBELL_PCI_MASK, 0)) return 6;
	if (c4_poke(card, DC21285_ARMCSR_BASE+SA_CONTROL, SA_CTL_ALLRIGHT))
		return 7;
	if (c4_poke(card, DC21285_ARMCSR_BASE+XBUS_CYCLE, INIT_XBUS_CYCLE))
		return 8;
	if (c4_poke(card, DC21285_ARMCSR_BASE+XBUS_STROBE, INIT_XBUS_STROBE))
		return 8;
	if (c4_poke(card, DC21285_ARMCSR_BASE+DRAM_TIMING, 0)) return 9;

        udelay(1000);

	if (c4_peek(card, DC21285_DRAM_A0MR, &dummy)) return 10;
	if (c4_peek(card, DC21285_DRAM_A1MR, &dummy)) return 11;
	if (c4_peek(card, DC21285_DRAM_A2MR, &dummy)) return 12;
	if (c4_peek(card, DC21285_DRAM_A3MR, &dummy)) return 13;

	if (c4_poke(card, DC21285_DRAM_A0MR+CAS_OFFSET, 0)) return 14;
	if (c4_poke(card, DC21285_DRAM_A1MR+CAS_OFFSET, 0)) return 15;
	if (c4_poke(card, DC21285_DRAM_A2MR+CAS_OFFSET, 0)) return 16;
	if (c4_poke(card, DC21285_DRAM_A3MR+CAS_OFFSET, 0)) return 17;

        udelay(1000);

	if (c4_poke(card, DC21285_ARMCSR_BASE+DRAM_TIMING, DRAM_TIMING_DEF))
		return 18;

	if (c4_poke(card, DC21285_ARMCSR_BASE+DRAM_ADDR_SIZE_0,DRAM_AD_SZ_DEF0))
		return 19;
	if (c4_poke(card, DC21285_ARMCSR_BASE+DRAM_ADDR_SIZE_1,DRAM_AD_SZ_NULL))
		return 20;
	if (c4_poke(card, DC21285_ARMCSR_BASE+DRAM_ADDR_SIZE_2,DRAM_AD_SZ_NULL))
		return 21;
	if (c4_poke(card, DC21285_ARMCSR_BASE+DRAM_ADDR_SIZE_3,DRAM_AD_SZ_NULL))
		return 22;

	/* Transputer test */
	
	if (   c4_poke(card, 0x000000, 0x11111111)
	    || c4_poke(card, 0x400000, 0x22222222)
	    || c4_poke(card, 0x800000, 0x33333333)
	    || c4_poke(card, 0xC00000, 0x44444444))
		return 23;

	if (   c4_peek(card, 0x000000, &dummy) || dummy != 0x11111111
	    || c4_peek(card, 0x400000, &dummy) || dummy != 0x22222222
	    || c4_peek(card, 0x800000, &dummy) || dummy != 0x33333333
	    || c4_peek(card, 0xC00000, &dummy) || dummy != 0x44444444)
		return 24;

	if (   c4_poke(card, 0x000000, 0x55555555)
	    || c4_poke(card, 0x400000, 0x66666666)
	    || c4_poke(card, 0x800000, 0x77777777)
	    || c4_poke(card, 0xC00000, 0x88888888))
		return 25;

	if (   c4_peek(card, 0x000000, &dummy) || dummy != 0x55555555
	    || c4_peek(card, 0x400000, &dummy) || dummy != 0x66666666
	    || c4_peek(card, 0x800000, &dummy) || dummy != 0x77777777
	    || c4_peek(card, 0xC00000, &dummy) || dummy != 0x88888888)
		return 26;

	return 0;
}

/* ------------------------------------------------------------- */

static void c4_dispatch_tx(avmcard *card)
{
	avmcard_dmainfo *dma = card->dma;
	unsigned long flags;
	struct sk_buff *skb;
	__u8 cmd, subcmd;
	__u16 len;
	__u32 txlen;
	void *p;
	
	save_flags(flags);
	cli();

	if (card->csr & DBELL_DOWN_ARM) { /* tx busy */
	        restore_flags(flags);
		return;
	}

	skb = skb_dequeue(&dma->send_queue);
	if (!skb) {
#ifdef CONFIG_C4_DEBUG
		printk(KERN_DEBUG "%s: tx underrun\n", card->name);
#endif
	        restore_flags(flags);
		return;
	}

	len = CAPIMSG_LEN(skb->data);

	if (len) {
		cmd = CAPIMSG_COMMAND(skb->data);
		subcmd = CAPIMSG_SUBCOMMAND(skb->data);

		p = dma->sendbuf;

		if (CAPICMD(cmd, subcmd) == CAPI_DATA_B3_REQ) {
			__u16 dlen = CAPIMSG_DATALEN(skb->data);
			_put_byte(&p, SEND_DATA_B3_REQ);
			_put_slice(&p, skb->data, len);
			_put_slice(&p, skb->data + len, dlen);
		} else {
			_put_byte(&p, SEND_MESSAGE);
			_put_slice(&p, skb->data, len);
		}
		txlen = (__u8 *)p - (__u8 *)dma->sendbuf;
#ifdef CONFIG_C4_DEBUG
		printk(KERN_DEBUG "%s: tx put msg len=%d\n", card->name, txlen);
#endif
	} else {
		txlen = skb->len-2;
#ifdef CONFIG_C4_POLLDEBUG
		if (skb->data[2] == SEND_POLLACK)
			printk(KERN_INFO "%s: ack to c4\n", card->name);
#endif
#ifdef CONFIG_C4_DEBUG
		printk(KERN_DEBUG "%s: tx put 0x%x len=%d\n",
				card->name, skb->data[2], txlen);
#endif
		memcpy(dma->sendbuf, skb->data+2, skb->len-2);
	}
	txlen = (txlen + 3) & ~3;

	c4outmeml(card->mbase+MBOX_DOWN_ADDR, virt_to_phys(dma->sendbuf));
	c4outmeml(card->mbase+MBOX_DOWN_LEN, txlen);

	card->csr |= DBELL_DOWN_ARM;

	c4outmeml(card->mbase+DOORBELL, DBELL_DOWN_ARM);

	restore_flags(flags);
	dev_kfree_skb(skb);
}

/* ------------------------------------------------------------- */

static void queue_pollack(avmcard *card)
{
	struct sk_buff *skb;
	void *p;

	skb = alloc_skb(3, GFP_ATOMIC);
	if (!skb) {
		printk(KERN_CRIT "%s: no memory, lost poll ack\n",
					card->name);
		return;
	}
	p = skb->data;
	_put_byte(&p, 0);
	_put_byte(&p, 0);
	_put_byte(&p, SEND_POLLACK);
	skb_put(skb, (__u8 *)p - (__u8 *)skb->data);

	skb_queue_tail(&card->dma->send_queue, skb);
	c4_dispatch_tx(card);
}

/* ------------------------------------------------------------- */

static void c4_handle_rx(avmcard *card)
{
	avmcard_dmainfo *dma = card->dma;
	struct capi_ctr *ctrl;
	avmctrl_info *cinfo;
	struct sk_buff *skb;
	void *p = dma->recvbuf;
	__u32 ApplId, DataB3Len, NCCI, WindowSize;
	__s32 MsgLen;
	__u8 b1cmd =  _get_byte(&p);
	__u32 cidx;


#ifdef CONFIG_C4_DEBUG
	printk(KERN_DEBUG "%s: rx 0x%x len=%lu\n", card->name,
				b1cmd, (unsigned long)dma->recvlen);
#endif
	
	switch (b1cmd) {
	case RECEIVE_DATA_B3_IND:

		ApplId = (unsigned) _get_word(&p);
		MsgLen = _get_slice(&p, card->msgbuf);
		DataB3Len = _get_slice(&p, card->databuf);
		cidx = CAPIMSG_CONTROLLER(card->msgbuf)-card->cardnr;
		if (cidx > 3) cidx = 0;
		ctrl = card->ctrlinfo[cidx].capi_ctrl;

		if (MsgLen < 30) { /* not CAPI 64Bit */
			memset(card->msgbuf+MsgLen, 0, 30-MsgLen);
			MsgLen = 30;
			CAPIMSG_SETLEN(card->msgbuf, 30);
		}
		if (!(skb = alloc_skb(DataB3Len+MsgLen, GFP_ATOMIC))) {
			printk(KERN_ERR "%s: incoming packet dropped\n",
					card->name);
		} else {
			memcpy(skb_put(skb, MsgLen), card->msgbuf, MsgLen);
			memcpy(skb_put(skb, DataB3Len), card->databuf, DataB3Len);
			ctrl->handle_capimsg(ctrl, ApplId, skb);
		}
		break;

	case RECEIVE_MESSAGE:

		ApplId = (unsigned) _get_word(&p);
		MsgLen = _get_slice(&p, card->msgbuf);
		cidx = CAPIMSG_CONTROLLER(card->msgbuf)-card->cardnr;
		if (cidx > 3) cidx = 0;
		ctrl = card->ctrlinfo[cidx].capi_ctrl;

		if (!(skb = alloc_skb(MsgLen, GFP_ATOMIC))) {
			printk(KERN_ERR "%s: incoming packet dropped\n",
					card->name);
		} else {
			memcpy(skb_put(skb, MsgLen), card->msgbuf, MsgLen);
			ctrl->handle_capimsg(ctrl, ApplId, skb);
		}
		break;

	case RECEIVE_NEW_NCCI:

		ApplId = _get_word(&p);
		NCCI = _get_word(&p);
		WindowSize = _get_word(&p);
		cidx = (NCCI&0x7f) - card->cardnr;
		if (cidx > 3) cidx = 0;
		ctrl = card->ctrlinfo[cidx].capi_ctrl;

		ctrl->new_ncci(ctrl, ApplId, NCCI, WindowSize);

		break;

	case RECEIVE_FREE_NCCI:

		ApplId = _get_word(&p);
		NCCI = _get_word(&p);

		if (NCCI != 0xffffffff) {
			cidx = (NCCI&0x7f) - card->cardnr;
			if (cidx > 3) cidx = 0;
			ctrl = card->ctrlinfo[cidx].capi_ctrl;
			ctrl->free_ncci(ctrl, ApplId, NCCI);
		} else {
			for (cidx=0; cidx < 4; cidx++) {
				ctrl = card->ctrlinfo[cidx].capi_ctrl;
				ctrl->appl_released(ctrl, ApplId);
			}
		}
		break;

	case RECEIVE_START:
#ifdef CONFIG_C4_POLLDEBUG
		printk(KERN_INFO "%s: poll from c4\n", card->name);
#endif
		if (!suppress_pollack)
			queue_pollack(card);
		for (cidx=0; cidx < 4; cidx++) {
			ctrl = card->ctrlinfo[cidx].capi_ctrl;
			ctrl->resume_output(ctrl);
		}
		break;

	case RECEIVE_STOP:
		for (cidx=0; cidx < 4; cidx++) {
			ctrl = card->ctrlinfo[cidx].capi_ctrl;
			ctrl->suspend_output(ctrl);
		}
		break;

	case RECEIVE_INIT:

	        cidx = card->nlogcontr++;
	        cinfo = &card->ctrlinfo[cidx];
		ctrl = cinfo->capi_ctrl;
		cinfo->versionlen = _get_slice(&p, cinfo->versionbuf);
		b1_parse_version(cinfo);
		printk(KERN_INFO "%s: %s-card (%s) now active\n",
		       card->name,
		       cinfo->version[VER_CARDTYPE],
		       cinfo->version[VER_DRIVER]);
		ctrl->ready(cinfo->capi_ctrl);
		break;

	case RECEIVE_TASK_READY:
		ApplId = (unsigned) _get_word(&p);
		MsgLen = _get_slice(&p, card->msgbuf);
		card->msgbuf[MsgLen--] = 0;
		while (    MsgLen >= 0
		       && (   card->msgbuf[MsgLen] == '\n'
			   || card->msgbuf[MsgLen] == '\r'))
			card->msgbuf[MsgLen--] = 0;
		printk(KERN_INFO "%s: task %d \"%s\" ready.\n",
				card->name, ApplId, card->msgbuf);
		break;

	case RECEIVE_DEBUGMSG:
		MsgLen = _get_slice(&p, card->msgbuf);
		card->msgbuf[MsgLen--] = 0;
		while (    MsgLen >= 0
		       && (   card->msgbuf[MsgLen] == '\n'
			   || card->msgbuf[MsgLen] == '\r'))
			card->msgbuf[MsgLen--] = 0;
		printk(KERN_INFO "%s: DEBUG: %s\n", card->name, card->msgbuf);
		break;

	default:
		printk(KERN_ERR "%s: c4_interrupt: 0x%x ???\n",
				card->name, b1cmd);
		return;
	}
}

/* ------------------------------------------------------------- */

static void c4_handle_interrupt(avmcard *card)
{
	__u32 status = c4inmeml(card->mbase+DOORBELL);

	if (status & DBELL_RESET_HOST) {
		int i;
		c4outmeml(card->mbase+PCI_OUT_INT_MASK, 0x0c);
		printk(KERN_ERR "%s: unexpected reset\n", card->name);
                for (i=0; i < 4; i++) {
			avmctrl_info *cinfo = &card->ctrlinfo[i];
			memset(cinfo->version, 0, sizeof(cinfo->version));
			if (cinfo->capi_ctrl)
				cinfo->capi_ctrl->reseted(cinfo->capi_ctrl);
		}
		return;
	}

	status &= (DBELL_UP_HOST | DBELL_DOWN_HOST);
	if (!status)
		return;
	c4outmeml(card->mbase+DOORBELL, status);

	if ((status & DBELL_UP_HOST) != 0) {
		card->dma->recvlen = c4inmeml(card->mbase+MBOX_UP_LEN);
		c4outmeml(card->mbase+MBOX_UP_LEN, 0);
		c4_handle_rx(card);
		card->dma->recvlen = 0;
		c4outmeml(card->mbase+MBOX_UP_LEN, sizeof(card->dma->recvbuf));
		c4outmeml(card->mbase+DOORBELL, DBELL_UP_ARM);
	}

	if ((status & DBELL_DOWN_HOST) != 0) {
		card->csr &= ~DBELL_DOWN_ARM;
	        c4_dispatch_tx(card);
	} else if (card->csr & DBELL_DOWN_HOST) {
		if (c4inmeml(card->mbase+MBOX_DOWN_LEN) == 0) {
		        card->csr &= ~DBELL_DOWN_ARM;
			c4_dispatch_tx(card);
		}
	}
}

static void c4_interrupt(int interrupt, void *devptr, struct pt_regs *regs)
{
	avmcard *card;

	card = (avmcard *) devptr;

	if (!card) {
		printk(KERN_WARNING "%s: interrupt: wrong device\n", card->name);
		return;
	}
	if (card->interrupt) {
		printk(KERN_ERR "%s: reentering interrupt hander\n",
				card->name);
		return;
	}

	card->interrupt = 1;

	c4_handle_interrupt(card);

	card->interrupt = 0;
}

/* ------------------------------------------------------------- */

static void c4_send_init(avmcard *card)
{
	struct sk_buff *skb;
	void *p;

	skb = alloc_skb(15, GFP_ATOMIC);
	if (!skb) {
		printk(KERN_CRIT "%s: no memory, lost register appl.\n",
					card->name);
		return;
	}
	p = skb->data;
	_put_byte(&p, 0);
	_put_byte(&p, 0);
	_put_byte(&p, SEND_INIT);
	_put_word(&p, AVM_NAPPS);
	_put_word(&p, AVM_NCCI_PER_CHANNEL*30);
	_put_word(&p, card->cardnr - 1);
	skb_put(skb, (__u8 *)p - (__u8 *)skb->data);

	skb_queue_tail(&card->dma->send_queue, skb);
	c4_dispatch_tx(card);
}

static int c4_send_config(avmcard *card, capiloaddatapart * config)
{
	struct sk_buff *skb;
	__u8 val[sizeof(__u32)];
	void *p;
	unsigned char *dp;
	int left, retval;
	
	skb = alloc_skb(12 + ((config->len+3)/4)*5, GFP_ATOMIC);
	if (!skb) {
		printk(KERN_CRIT "%s: no memory, can't send config.\n",
					card->name);
		return	-ENOMEM;
	}
	p = skb->data;
	_put_byte(&p, 0);
	_put_byte(&p, 0);
	_put_byte(&p, SEND_CONFIG);
	_put_word(&p, 1);
	_put_byte(&p, SEND_CONFIG);
	_put_word(&p, config->len);	/* 12 */

	dp = config->data;
	left = config->len;
	while (left >= sizeof(__u32)) {
	        if (config->user) {
			retval = copy_from_user(val, dp, sizeof(val));
			if (retval) {
				dev_kfree_skb(skb);
				return -EFAULT;
			}
		} else {
			memcpy(val, dp, sizeof(val));
		}
		_put_byte(&p, SEND_CONFIG);
		_put_byte(&p, val[0]);
		_put_byte(&p, val[1]);
		_put_byte(&p, val[2]);
		_put_byte(&p, val[3]);
		left -= sizeof(val);
		dp += sizeof(val);
	}
	if (left) {
		memset(val, 0, sizeof(val));
		if (config->user) {
			retval = copy_from_user(&val, dp, left);
			if (retval) {
				dev_kfree_skb(skb);
				return -EFAULT;
			}
		} else {
			memcpy(&val, dp, left);
		}
		_put_byte(&p, SEND_CONFIG);
		_put_byte(&p, val[0]);
		_put_byte(&p, val[1]);
		_put_byte(&p, val[2]);
		_put_byte(&p, val[3]);
	}

	skb_put(skb, (__u8 *)p - (__u8 *)skb->data);

	skb_queue_tail(&card->dma->send_queue, skb);
	c4_dispatch_tx(card);

	return 0;
}

static int c4_load_firmware(struct capi_ctr *ctrl, capiloaddata *data)
{
	avmctrl_info *cinfo = (avmctrl_info *)(ctrl->driverdata);
	avmcard *card = cinfo->card;
	unsigned long flags;
	int retval;

	if ((retval = c4_load_t4file(card, &data->firmware))) {
		printk(KERN_ERR "%s: failed to load t4file!!\n",
					card->name);
		c4_reset(card);
		return retval;
	}

	save_flags(flags);
	cli();

	card->csr = 0;
	c4outmeml(card->mbase+MBOX_UP_LEN, 0);
	c4outmeml(card->mbase+MBOX_DOWN_LEN, 0);
	c4outmeml(card->mbase+DOORBELL, DBELL_INIT);
	udelay(1000);
	c4outmeml(card->mbase+DOORBELL,
			DBELL_UP_HOST | DBELL_DOWN_HOST | DBELL_RESET_HOST);

	c4outmeml(card->mbase+PCI_OUT_INT_MASK, 0x08);

	card->dma->recvlen = 0;
	c4outmeml(card->mbase+MBOX_UP_ADDR, virt_to_phys(card->dma->recvbuf));
	c4outmeml(card->mbase+MBOX_UP_LEN, sizeof(card->dma->recvbuf));
	c4outmeml(card->mbase+DOORBELL, DBELL_UP_ARM);
	restore_flags(flags);

	if (data->configuration.len > 0 && data->configuration.data)
		c4_send_config(card, &data->configuration);

        c4_send_init(card);

	return 0;
}


void c4_reset_ctr(struct capi_ctr *ctrl)
{
	avmcard *card = ((avmctrl_info *)(ctrl->driverdata))->card;
	avmctrl_info *cinfo;
	int i;

 	c4_reset(card);

        for (i=0; i < 4; i++) {
		cinfo = &card->ctrlinfo[i];
		memset(cinfo->version, 0, sizeof(cinfo->version));
		if (cinfo->capi_ctrl)
			cinfo->capi_ctrl->reseted(cinfo->capi_ctrl);
	}
}

static void c4_remove_ctr(struct capi_ctr *ctrl)
{
	avmcard *card = ((avmctrl_info *)(ctrl->driverdata))->card;
	avmctrl_info *cinfo;
	int i;

 	c4_reset(card);

        for (i=0; i < 4; i++) {
		cinfo = &card->ctrlinfo[i];
		if (cinfo->capi_ctrl)
			di->detach_ctr(cinfo->capi_ctrl);
	}

	free_irq(card->irq, card);
	iounmap((void *) (((unsigned long) card->mbase) & PAGE_MASK));
	release_region(card->port, AVMB1_PORTLEN);
	ctrl->driverdata = 0;
	kfree(card->ctrlinfo);
	kfree(card->dma);
	kfree(card);

	MOD_DEC_USE_COUNT;
}

/* ------------------------------------------------------------- */


void c4_register_appl(struct capi_ctr *ctrl,
				__u16 appl,
				capi_register_params *rp)
{
	avmctrl_info *cinfo = (avmctrl_info *)(ctrl->driverdata);
	avmcard *card = cinfo->card;
	struct sk_buff *skb;
	int want = rp->level3cnt;
	int nconn;
	void *p;

	if (ctrl->cnr == card->cardnr) {

		if (want > 0) nconn = want;
		else nconn = ctrl->profile.nbchannel * 4 * -want;
		if (nconn == 0) nconn = ctrl->profile.nbchannel * 4;

		skb = alloc_skb(23, GFP_ATOMIC);
		if (!skb) {
			printk(KERN_CRIT "%s: no memory, lost register appl.\n",
						card->name);
			return;
		}
		p = skb->data;
		_put_byte(&p, 0);
		_put_byte(&p, 0);
		_put_byte(&p, SEND_REGISTER);
		_put_word(&p, appl);
		_put_word(&p, 1024 * (nconn+1));
		_put_word(&p, nconn);
		_put_word(&p, rp->datablkcnt);
		_put_word(&p, rp->datablklen);
		skb_put(skb, (__u8 *)p - (__u8 *)skb->data);

		skb_queue_tail(&card->dma->send_queue, skb);
		c4_dispatch_tx(card);
	}

	ctrl->appl_registered(ctrl, appl);
}

/* ------------------------------------------------------------- */

void c4_release_appl(struct capi_ctr *ctrl, __u16 appl)
{
	avmctrl_info *cinfo = (avmctrl_info *)(ctrl->driverdata);
	avmcard *card = cinfo->card;
	struct sk_buff *skb;
	void *p;

	if (ctrl->cnr == card->cardnr) {
		skb = alloc_skb(7, GFP_ATOMIC);
		if (!skb) {
			printk(KERN_CRIT "%s: no memory, lost release appl.\n",
						card->name);
			return;
		}
		p = skb->data;
		_put_byte(&p, 0);
		_put_byte(&p, 0);
		_put_byte(&p, SEND_RELEASE);
		_put_word(&p, appl);

		skb_put(skb, (__u8 *)p - (__u8 *)skb->data);
		skb_queue_tail(&card->dma->send_queue, skb);
		c4_dispatch_tx(card);
	}
}

/* ------------------------------------------------------------- */


static void c4_send_message(struct capi_ctr *ctrl, struct sk_buff *skb)
{
	avmctrl_info *cinfo = (avmctrl_info *)(ctrl->driverdata);
	avmcard *card = cinfo->card;
	skb_queue_tail(&card->dma->send_queue, skb);
	c4_dispatch_tx(card);
}

/* ------------------------------------------------------------- */

static char *c4_procinfo(struct capi_ctr *ctrl)
{
	avmctrl_info *cinfo = (avmctrl_info *)(ctrl->driverdata);

	if (!cinfo)
		return "";
	sprintf(cinfo->infobuf, "%s %s 0x%x %d 0x%lx",
		cinfo->cardname[0] ? cinfo->cardname : "-",
		cinfo->version[VER_DRIVER] ? cinfo->version[VER_DRIVER] : "-",
		cinfo->card ? cinfo->card->port : 0x0,
		cinfo->card ? cinfo->card->irq : 0,
		cinfo->card ? cinfo->card->membase : 0
		);
	return cinfo->infobuf;
}

static int c4_read_proc(char *page, char **start, off_t off,
        		int count, int *eof, struct capi_ctr *ctrl)
{
	avmctrl_info *cinfo = (avmctrl_info *)(ctrl->driverdata);
	avmcard *card = cinfo->card;
	__u8 flag;
	int len = 0;
	char *s;

	len += sprintf(page+len, "%-16s %s\n", "name", card->name);
	len += sprintf(page+len, "%-16s 0x%x\n", "io", card->port);
	len += sprintf(page+len, "%-16s %d\n", "irq", card->irq);
	len += sprintf(page+len, "%-16s 0x%lx\n", "membase", card->membase);
	switch (card->cardtype) {
	case avm_b1isa: s = "B1 ISA"; break;
	case avm_b1pci: s = "B1 PCI"; break;
	case avm_b1pcmcia: s = "B1 PCMCIA"; break;
	case avm_m1: s = "M1"; break;
	case avm_m2: s = "M2"; break;
	case avm_t1isa: s = "T1 ISA (HEMA)"; break;
	case avm_t1pci: s = "T1 PCI"; break;
	case avm_c4: s = "C4"; break;
	default: s = "???"; break;
	}
	len += sprintf(page+len, "%-16s %s\n", "type", s);
	if ((s = cinfo->version[VER_DRIVER]) != 0)
	   len += sprintf(page+len, "%-16s %s\n", "ver_driver", s);
	if ((s = cinfo->version[VER_CARDTYPE]) != 0)
	   len += sprintf(page+len, "%-16s %s\n", "ver_cardtype", s);
	if ((s = cinfo->version[VER_SERIAL]) != 0)
	   len += sprintf(page+len, "%-16s %s\n", "ver_serial", s);

	if (card->cardtype != avm_m1) {
        	flag = ((__u8 *)(ctrl->profile.manu))[3];
        	if (flag)
			len += sprintf(page+len, "%-16s%s%s%s%s%s%s%s\n",
			"protocol",
			(flag & 0x01) ? " DSS1" : "",
			(flag & 0x02) ? " CT1" : "",
			(flag & 0x04) ? " VN3" : "",
			(flag & 0x08) ? " NI1" : "",
			(flag & 0x10) ? " AUSTEL" : "",
			(flag & 0x20) ? " ESS" : "",
			(flag & 0x40) ? " 1TR6" : ""
			);
	}
	if (card->cardtype != avm_m1) {
        	flag = ((__u8 *)(ctrl->profile.manu))[5];
		if (flag)
			len += sprintf(page+len, "%-16s%s%s%s%s\n",
			"linetype",
			(flag & 0x01) ? " point to point" : "",
			(flag & 0x02) ? " point to multipoint" : "",
			(flag & 0x08) ? " leased line without D-channel" : "",
			(flag & 0x04) ? " leased line with D-channel" : ""
			);
	}
	len += sprintf(page+len, "%-16s %s\n", "cardname", cinfo->cardname);

	if (off+count >= len)
	   *eof = 1;
	if (len < off)
           return 0;
	*start = page + off;
	return ((count < len-off) ? count : len-off);
}

/* ------------------------------------------------------------- */

static int c4_add_card(struct capi_driver *driver, struct capicardparams *p)
{
	unsigned long base, page_offset;
	avmctrl_info *cinfo;
	avmcard *card;
	int retval;
	int i;

	MOD_INC_USE_COUNT;

	card = (avmcard *) kmalloc(sizeof(avmcard), GFP_ATOMIC);

	if (!card) {
		printk(KERN_WARNING "%s: no memory.\n", driver->name);
	        MOD_DEC_USE_COUNT;
		return -ENOMEM;
	}
	memset(card, 0, sizeof(avmcard));
	card->dma = (avmcard_dmainfo *) kmalloc(sizeof(avmcard_dmainfo), GFP_ATOMIC);
	if (!card->dma) {
		printk(KERN_WARNING "%s: no memory.\n", driver->name);
		kfree(card);
	        MOD_DEC_USE_COUNT;
		return -ENOMEM;
	}
	memset(card->dma, 0, sizeof(avmcard_dmainfo));
        cinfo = (avmctrl_info *) kmalloc(sizeof(avmctrl_info)*4, GFP_ATOMIC);
	if (!cinfo) {
		printk(KERN_WARNING "%s: no memory.\n", driver->name);
	        kfree(card->ctrlinfo);
		kfree(card->dma);
		kfree(card);
	        MOD_DEC_USE_COUNT;
		return -ENOMEM;
	}
	memset(cinfo, 0, sizeof(avmctrl_info)*4);
	card->ctrlinfo = cinfo;
	for (i=0; i < 4; i++) {
		cinfo = &card->ctrlinfo[i];
		cinfo->card = card;
	}
	sprintf(card->name, "c4-%x", p->port);
	card->port = p->port;
	card->irq = p->irq;
	card->membase = p->membase;
	card->cardtype = avm_c4;

	if (check_region(card->port, AVMB1_PORTLEN)) {
		printk(KERN_WARNING
		       "%s: ports 0x%03x-0x%03x in use.\n",
		       driver->name, card->port, card->port + AVMB1_PORTLEN);
	        kfree(card->ctrlinfo);
		kfree(card->dma);
		kfree(card);
	        MOD_DEC_USE_COUNT;
		return -EBUSY;
	}

	base = card->membase & PAGE_MASK;
	page_offset = card->membase - base;
	card->mbase = ioremap_nocache(base, page_offset + 128);
	if (card->mbase) {
		card->mbase += page_offset;
	} else {
		printk(KERN_NOTICE "%s: can't remap memory at 0x%lx\n",
					driver->name, card->membase);
	        kfree(card->ctrlinfo);
		kfree(card->dma);
		kfree(card);
	        MOD_DEC_USE_COUNT;
		return -EIO;
	}

	if ((retval = c4_detect(card)) != 0) {
		printk(KERN_NOTICE "%s: NO card at 0x%x (%d)\n",
					driver->name, card->port, retval);
                iounmap((void *) (((unsigned long) card->mbase) & PAGE_MASK));
	        kfree(card->ctrlinfo);
		kfree(card->dma);
		kfree(card);
	        MOD_DEC_USE_COUNT;
		return -EIO;
	}
	c4_reset(card);

	request_region(p->port, AVMB1_PORTLEN, card->name);

	retval = request_irq(card->irq, c4_interrupt, SA_SHIRQ, card->name, card);
	if (retval) {
		printk(KERN_ERR "%s: unable to get IRQ %d.\n",
				driver->name, card->irq);
                iounmap((void *) (((unsigned long) card->mbase) & PAGE_MASK));
		release_region(card->port, AVMB1_PORTLEN);
	        kfree(card->ctrlinfo);
		kfree(card->dma);
		kfree(card);
	        MOD_DEC_USE_COUNT;
		return -EBUSY;
	}

	for (i=0; i < 4; i++) {
		cinfo = &card->ctrlinfo[i];
		cinfo->card = card;
		cinfo->capi_ctrl = di->attach_ctr(driver, card->name, cinfo);
		if (!cinfo->capi_ctrl) {
			printk(KERN_ERR "%s: attach controller failed (%d).\n",
					driver->name, i);
			for (i--; i >= 0; i--) {
				cinfo = &card->ctrlinfo[i];
				di->detach_ctr(cinfo->capi_ctrl);
			}
                	iounmap((void *) (((unsigned long) card->mbase) & PAGE_MASK));
			free_irq(card->irq, card);
			release_region(card->port, AVMB1_PORTLEN);
	        	kfree(card->dma);
	        	kfree(card->ctrlinfo);
			kfree(card);
	        	MOD_DEC_USE_COUNT;
			return -EBUSY;
		}
		if (i == 0)
			card->cardnr = cinfo->capi_ctrl->cnr;
	}

	skb_queue_head_init(&card->dma->send_queue);

	printk(KERN_INFO
		"%s: AVM C4 at i/o %#x, irq %d, mem %#lx\n",
		driver->name, card->port, card->irq, card->membase);

	return 0;
}

/* ------------------------------------------------------------- */

static struct capi_driver c4_driver = {
    "c4",
    "0.0",
    c4_load_firmware,
    c4_reset_ctr,
    c4_remove_ctr,
    c4_register_appl,
    c4_release_appl,
    c4_send_message,

    c4_procinfo,
    c4_read_proc,
    0,	/* use standard driver_read_proc */

    0, /* no add_card function */
};

#ifdef MODULE
#define c4_init init_module
void cleanup_module(void);
#endif

#ifndef PCI_ANY_ID
#define PCI_ANY_ID (~0)
#endif

static struct pci_dev *
pci_find_subsys(unsigned int vendor, unsigned int device,
		unsigned int ss_vendor, unsigned int ss_device,
		struct pci_dev *from)
{
	unsigned short subsystem_vendor, subsystem_device;

	while ((from = pci_find_device(vendor, device, from))) {
		pci_read_config_word(from, PCI_SUBSYSTEM_VENDOR_ID, &subsystem_vendor);
		pci_read_config_word(from, PCI_SUBSYSTEM_ID, &subsystem_device);
		if ((ss_vendor == PCI_ANY_ID || subsystem_vendor == ss_vendor) &&
		    (ss_device == PCI_ANY_ID || subsystem_device == ss_device))
			return from;
	}
	return NULL;
}

static int ncards = 0;

int c4_init(void)
{
	struct capi_driver *driver = &c4_driver;
	struct pci_dev *dev = NULL;
	char *p;
	int retval;

	if ((p = strchr(revision, ':'))) {
		strncpy(driver->revision, p + 1, sizeof(driver->revision));
		p = strchr(driver->revision, '$');
		*p = 0;
	}

	printk(KERN_INFO "%s: revision %s\n", driver->name, driver->revision);

        di = attach_capi_driver(driver);

	if (!di) {
		printk(KERN_ERR "%s: failed to attach capi_driver\n",
				driver->name);
		return -EIO;
	}

#ifdef CONFIG_PCI
	if (!pci_present()) {
		printk(KERN_ERR "%s: no PCI bus present\n", driver->name);
    		detach_capi_driver(driver);
		return -EIO;
	}

	while ((dev = pci_find_subsys(
			PCI_VENDOR_ID_DEC, PCI_DEVICE_ID_DEC_21285,
			PCI_VENDOR_ID_AVM, PCI_DEVICE_ID_AVM_C4, dev))) {
		struct capicardparams param;

		param.port = dev->base_address[ 1] & PCI_BASE_ADDRESS_IO_MASK;
		param.irq = dev->irq;
		param.membase = dev->base_address[ 0] & PCI_BASE_ADDRESS_MEM_MASK;

		printk(KERN_INFO
			"%s: PCI BIOS reports AVM-C4 at i/o %#x, irq %d, mem %#x\n",
			driver->name, param.port, param.irq, param.membase);
		retval = c4_add_card(driver, &param);
		if (retval != 0) {
		        printk(KERN_ERR
			"%s: no AVM-C4 at i/o %#x, irq %d detected, mem %#x\n",
			driver->name, param.port, param.irq, param.membase);
#ifdef MODULE
			cleanup_module();
#endif
			return retval;
		}
		ncards++;
	}
	if (ncards) {
		printk(KERN_INFO "%s: %d C4 card(s) detected\n",
				driver->name, ncards);
		return 0;
	}
	printk(KERN_ERR "%s: NO C4 card detected\n", driver->name);
	return -ESRCH;
#else
	printk(KERN_ERR "%s: kernel not compiled with PCI.\n", driver->name);
	return -EIO;
#endif
}

#ifdef MODULE
void cleanup_module(void)
{
    detach_capi_driver(&c4_driver);
}
#endif
