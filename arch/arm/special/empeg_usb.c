#undef DEBUG_USB
#undef DEBUG_USB_1
#undef DEBUG_USB_2
#undef DEBUG_USB_6 /* Stall debug */
#define NO_ZERO_TERM

#ifdef CONFIG_EMPEG_USBD12
#error Mk1 empeg's have the 9602, Mk2's have the PDIUSBD12 - none have both!
#endif

/* empeg USB support
 *
 * (C) empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Hugo Fiennes, <hugo@empeg.com>
 *
 * This code is heavily based on the USBN9602 evaluation board example code
 * which is (c) 1997 National Semiconductor (www.national.com) and was
 * written by Jim Lyle and Bob Martin. The original copyright/notes are
 * preserved below.
 *
 * What is this USB driver thing anyway?
 * -------------------------------------
 * The USB driver in the empeg is very simple: don't confuse it with the highly
 * complex USB host device drivers like the ones which just missed the 2.2
 * kernel. Remember that usually, USB hosts are huge, firebreathing desktop
 * boxes, whereas USB slaves are little meek things like mice and scanners.
 * Ok, in our case we've got a 220Mhz USB slave, but we're atypical. If you
 * know the SA1100, you might be wondering why we're not using the USB slave
 * available on-chip: this is because it doesn't work until Rev G parts, of
 * which none are available :( ... so, we use the USBN9602 chip, which sits on
 * the 5v buffered side of the empeg bus.
 * 
 * So - the USB device functionality is pretty simple. The USBN9602 provides us
 * with a number of "endpoints" which are basically channels over which USB
 * sends or receives data. Endpoint 0 is special, and is used for supervisory
 * control information, like address allocation, disconnect & reconnect events
 * and so on. The 9602 gives us three other FIFO pairs, 1/2, 3/4 and 5/6.
 *
 * Although 5/6 are the bigger FIFOs (64 bytes), due to a bug in the 9602
 * we use 32-byte packet lengths and ping-pong between two 32-byte FIFOs
 * for our RX data transfer: this isn't optimal on speed, but it is reliable
 * and we can work on higher speed later.
 *
 * The mapping of this endpoint is simple: it basically behaves as a device,
 * /dev/usb0 which can be opened, closed, and have reads and writes
 * performed on it. Due to the high bandwidth of the USB (12Mbit) we maintain
 * local buffers to ensure that we don't get starved of data during
 * transmissions - and have a receive buffer to allow the process dealing with
 * USB to read in bigger blocks than the packet size.
 *
 * The implementation is designed to be pretty transparent: this is for a
 * number of reasons, not least of which is that we run basically the same
 * emplode-connection protocol over both the USB and the serial ports on the
 * empeg unit. Implementing the endpoint as a simple 'open/close' device
 * as opposed to a more complex network-style interface also means that we can
 * do froody stuff like run PPP over a 12Mbit usb link (host permitting, of
 * course...). To this end, there is limited control over the way the USB
 * device works - endpoint 0 is handled totally under software control, and
 * only a limited number of events are passed though for the user-side task
 * to worry about (like connection/disconnection of the USB cable).
 *
 * How?
 * ----
 * The USBN9602 is a memory-mapped device, which takes two memory locations.
 * For bulk data transfer we use the 'DMA' interface, which uses two extra
 * lines on the chip: 'DRQ' (DMA request, ie the 9602 is wanting a transfer
 * either in or out of the chip) and 'DACK' which signals when we are reading/
 * writing from the DMA port on the 9602.
 *
 * When the fifo is almost full (or almost empty, depending on the transfer
 * direction) we get an IRQ asking us for more data - we can then do a bulk
 * transfer to fill/empty the fifo in a hurry and let it get on with life.
 *
 * 20000812 hugo@empeg.com - Better NAK handling on data EP's
 *
 */

/**********************************************************************/
/* This is the source code for simple monitor program used to test    */
/* the 9602 USB device's microwire port.                              */
/*                                                                    */
/* Written by Jim Lyle, with some code borrowed from Bob Martin       */
/*                                                                    */
/* Copyright (c) 1997, National Semiconductor.  All rights reserved.  */    
/*                                                                    */   
/* VERSION 0X.50, Aug 28, 1997, Jim Lyle:  Revision control begins.   */
/*                                                                    */
/* VERSION 0X.51, Sep 04, 1997, Jim Lyle:  OHCI fix.                  */
/*                                                                    */
/* VERSION 0X.60, Sep 05, 1997, Jim Lyle:  Additional standard reqs.  */
/*   implemented to pass the chapter 9 compatibility tests.           */
/*                                                                    */  
/* VERSION 0X.61, Sep 12, 1997, Jim Lyle:  Changed the USB standard   */
/*   request decoding structure.                                      */
/*                                                                    */
/*                                                                    */  
/**********************************************************************/

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

#include <asm/byteorder.h>
#include <asm/irq.h>
#include <asm/segment.h>
#include <asm/io.h>
#include <asm/hardware.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>

#include "empeg_usbn9602.h"
#include "empeg_usb.h"

/* Only one USB channel */
struct usb_dev usb_devices[1];

/* We keep buffers to hold the last packet sent, in order that:
   On send: we can retransmit the packet on a NAK */
#define MAXTXPACKET 64
static unsigned char usb_tx[MAXTXPACKET];
static int usb_txsize;
static int usb_txidle=1;

/* ...and for receive */
#define MAXRXPACKET 32
static int usb_rxoverruns=0;

/* DMA flags */
#define DMA_OFF	(0x08+3)
#define DMA_ONS	(0x88+3)

/* Just 32-byte pingpongs */
#define WARNLEVEL 0
#define DMA_ON (DMA_OFF)

#if WARNLEVEL==0
#define WLEVEL (RFWL_DIS)
#elif WARNLEVEL==4
#define WLEVEL (RFWL_4)
#elif WARNLEVEL==8
#define WLEVEL (RFWL_8)
#elif WARNLEVEL==16
#define WLEVEL (RFWL_16)
#else
#error Warnlevel not a valid size
#endif

/* Logging for proc */
static char log[2048];
static int log_head=0;

static inline void LOG(char x)
{
	log[log_head++]=x;
	if (log_head==sizeof(log)) log_head=0;
}

static inline void LOGS(const char *x, ...)
{
    	static char buffer[1024];
	char *p = buffer;
    	va_list va;
	va_start(va, x);
	
	vsprintf(buffer, x, va);

	while (*p) {
		LOG(*p);
		p++;
	}

	va_end(va);
}

static inline void RESETLOG(void)
{
	memset(log,0,sizeof(log));
	log_head=0;
}

/* ...for the Natsemi code */
typedef unsigned char byte;

static struct file_operations usb_fops = {
  NULL, /* usb_lseek */
  usb_read,
  usb_write,
  NULL, /* usb_readdir */
  usb_poll,
  usb_ioctl,
  NULL, /* usb_mmap */
  usb_open,
  NULL, /* usb_flush */
  usb_release,
};

/**********************************************************************/
/* These are the macros                                               */
/**********************************************************************/

/* Flush and disable the USB TXn **************************************/
#define FLUSHTX0 write_usb(TXC0,FLUSH)
#define FLUSHTX3 write_usb(TXC3,FLUSH)

/* Flush and disable the USB RXn **************************************/
#define FLUSHRX0 write_usb(RXC0,FLUSH)
static char usb_rxstatus[]={ RXS2,RXS3 };
static char usb_rxfifo[]={ RXD2,RXD3 };
static char usb_rxcontrol[]={ RXC2,RXC3 };
static char usb_endpoint[]={ EPC4,EPC6 };

/* enable TX0, using the appropriate DATA PID *************************/
#define TXEN0_PID { write_usb(TXC0,(dtapid.TGL0PID?TX_TOGL:0)+TX_EN); dtapid.TGL0PID=!dtapid.TGL0PID; } 

/* enable TX1, using the appropriate DATA PID, but not toggling it ****/
#define TXEN1_PID_NO_TGL write_usb(TXC1,(dtapid.TGL1PID?TX_TOGL:0)+TX_LAST+TX_EN);

/* enable TX1, using the appropriate DATA PID *************************/
#define TXEN1_PID { TXEN1_PID_NO_TGL; dtapid.TGL1PID=!dtapid.TGL1PID; } 

/* enable TX2, using the appropriate DATA PID, but not toggling it ****/
#define TXEN2_PID_NO_TGL write_usb(TXC2,(dtapid.TGL2PID?TX_TOGL:0)+TX_LAST+TX_EN);

/* enable TX2, using the appropriate DATA PID *************************/
#define TXEN2_PID { TXEN2_PID_NO_TGL; dtapid.TGL2PID=!dtapid.TGL2PID;} 

/* enable TX3, using the appropriate DATA PID, but not toggling it ****/
#define TXEN3_PID_NO_TGL write_usb(TXC3,(dtapid.TGL3PID?TX_TOGL:0)+TX_LAST+TX_EN);

/* enable TX3, using the appropriate DATA PID *************************/
#define TXEN3_PID { TXEN3_PID_NO_TGL; dtapid.TGL3PID=!dtapid.TGL3PID; } 

/**********************************************************************/
/* These are the global variables                                     */
/**********************************************************************/
byte usb_buf[8];                    /* buffer used for USB */
byte desc_typ, desc_idx, desc_sze;
byte *desc_ptr;

byte usb_cfg;                       /* usb config. setting */
	
/*these are misc. status bits ***********************************/
struct { int b4,b5,b6,b7; } status; /* misc. status bits */
#define DEBUG     b4                /* set when host listening */   
#define GETDESC   b5                /* set when a get_descr. */  
#define USB_CMD   b6                /* set when doing usb cmd */   
				    /* sequence is underway */

struct { int b0,b1,b2,b3; } dtapid; /* PID related status */   
#define TGL0PID   b0                /* tracks NEXT data PID */  
#define TGL1PID   b1                /* tracks NEXT data PID */ 
#define TGL2PID   b2                /* tracks NEXT data PID */  
#define TGL3PID   b3                /* tracks NEXT data PID */  

/* Bits 0-6 correspond to like-numbered endpoints and are set to indicate the
   endpoint is stalled */
//static int stalld[6];

/* for now the sizes and offsets below need to be hand calculated, until I can
   find a better way to do it for multiple byte values, LSB goes first */
#define DEV_DESC_SIZE 18

byte DEV_DESC[] = {      DEV_DESC_SIZE,     /* length of this desc. */
			 0x01,              /* DEVICE descriptor */
			 0x00,0x01,         /* spec rev level (BCD) */
			 0x00,              /* device class */
			 0x00,              /* device subclass */
			 0x00,              /* device protocol */  
			 0x08,              /* max packet size */   
			 0x4f,0x08,         /* empeg's vendor ID */
			 0x01,0x00,         /* empeg car product ID */
			 0x01,0x00,         /* empeg's revision ID */  
			 1,                 /* index of manuf. string */   
			 2,                 /* index of prod.  string */  
			 0,                 /* index of ser. # string */   
			 0x01               /* number of configs. */ 
                         };
byte CFG_DESC[] = {      0x09,              /* length of this desc. */ 
			 0x02,              /* CONFIGURATION descriptor */  
			 0x27,0x00,         /* total length returned */ 
			 0x01,              /* number of interfaces */ 
			 0x01,              /* number of this config */ 
			 0x00,              /* index of config. string */  
			 0x40,              /* attr.: self powered */   
			 25,                /* we take no bus power */  

			 0x09,              /* length of this desc. */  
			 0x04,              /* INTERFACE descriptor */  
			 0x00,              /* interface number */
			 0x00,              /* alternate setting */  
			 0x03,              /* # of (non 0) endpoints */ 
			 0x00,              /* interface class */
			 0x00,              /* interface subclass */  
			 0x00,              /* interface protocol */  
			 0x00,              /* index of intf. string */ 

		       /* Pipe 0 */
			 0x07,              /* length of this desc. */   
			 0x05,              /* ENDPOINT descriptor */ 
			 0x81,              /* address (IN) */  
			 0x02,              /* attributes  (BULK) */    
			 (MAXTXPACKET&0xff),/* max packet size */
			 (MAXTXPACKET>>8),
			 0,                 /* interval (ms) */

		       /* Pipe 1 */ 
			 0x07,              /* length of this desc. */   
			 0x05,              /* ENDPOINT descriptor*/ 
			 0x05,/* was 2*/    /* address (OUT) */  
			 0x02,              /* attributes  (BULK) */    
			 (MAXRXPACKET&0xff),/* max packet size */
			 (MAXRXPACKET>>8),
			 0,                 /* interval (ms) */

		       /* Pipe 2 */ 
			 0x07,              /* length of this desc. */   
			 0x05,              /* ENDPOINT descriptor*/ 
			 0x02,              /* address (OUT) */  
			 0x02,              /* attributes  (BULK) */    
			 0x40,0x00,         /* max packet size (64) */
			 0};                /* interval (ms) */

#define CFG_DESC_SIZE sizeof(CFG_DESC) 

/* Unicode descriptors for our device description */
byte UNICODE_AVAILABLE[]={ 0x04,0x00,0x09,0x04 };        /* We offer only one language: 0409, US English */
byte UNICODE_MANUFACTURER[]={12,0,'e',0,'m',0,'p',0,'e',0,'g',0};
byte UNICODE_PRODUCT[]={20,0,'e',0,'m',0,'p',0,'e',0,'g',0,'-',0,
                             'c',0,'a',0,'r',0};

/* Predeclarations */
static void tx_d(int);

/* Read/write USB registers
 *
 * The empeg has the USBN9602 on the PCMCIA expansion bus, directly above the
 * second IDE channel:
 *
 * usbn9602+128 = data register
 * usbn9602+132 = address register
 * usbn9602+136 = read/write with DACK
 *
 * Accessing registers on the USBN9602 is a two-step process - first, set
 * the address, then read or write the data: if we're using pseudo-dma to
 * transfer data, we can just access the DMA location, as this gives us a
 * DACK signal with the chipselects and so no address setup is required.
 * We do a word-wide (16-bit) accesses to the chip space, as it is mapped as
 * 16-bit IO space even though the chip is 8-bits wide (IOIS16 is tied high).
 * On reads, we then discard the top 8 bits.
 *
 * The USBN9602's IRQ and DRQ outputs are connected to GPIO1 and GPIO2
 * respectively on the SA1100, which means that IRQ1 and IRQ2 can be set
 * up to trigger on the 9602's IRQ outputs. Note that the sense (polarity)
 * and style of IRQ (level/edge) need to be set up in the 9602 before IRQs
 * are used. We use an active high level-style IRQ from the 9602, which gets
 * buffered and inverted before it gets to the SA1100, so giving us an IRQ on
 * a negative-going edge. We currently don't use the DRQ input, but it's
 * there just in case.
 *
 * We have some udelay()'s just to ensure we don't go too fast for the
 * poor little thing: the PCMCIA bus IO timings are optimised for the HDD,
 * not the USB chip. The main problem is the delay-between-accesses, the
 * cycle times themselves are compatible
 */
static volatile unsigned char *usb_data=(unsigned char*)0xe0000088;
static volatile unsigned char *usb_addr=(unsigned char*)0xe000008c;

static int lastaddress=-1;
static __inline__ void address_usb(byte adr)
{
	if (adr!=lastaddress) { 
		*usb_addr=(lastaddress=adr);
	}
}

static __inline__ byte read_usb_quick(void)
{
	return(*usb_data);
}

static __inline__ byte read_usb(byte adr)      
{
	address_usb(adr);
	return(*usb_data);
}

static __inline__ void write_usb(byte adr, byte dta)  
{
	address_usb(adr);
	*usb_data=dta;
}

static __inline__ void write_usb_quick(byte dta)  
{
	*usb_data=dta;
}

/**********************************************************************/
/* This subroutine initializes the 9602.                              */
/**********************************************************************/
static void init_usb(void)
{
	/* Toss out any previous state */
	status.GETDESC=0;
	usb_cfg = 0;

	/* Give a software reset, then set ints to active high push pull */
	write_usb(MCNTRL,SRST);
	udelay(200);
	write_usb(MCNTRL,INT_H_P);

	/* Disable clock generator: this helps with EMC issues as the 9602
	   is noisy enough as it is (and we don't use the clock output) */
	write_usb(CCONF,0x80);
	
	/* Set default address, enable EP0 only */
	write_usb(FAR,AD_EN+0);
	write_usb(EPC0, 0x00); 
	
	/* Set up interrupt masks: endpoints 0/1/2 in AND out */

	/* NAK evnts */  
	write_usb(NAKMSK,NAK_O0|NAK_O2|NAK_O3);

	/* TX events */
	write_usb(TXMSK,TXFIFO0|TXFIFO3);

	/* RX events */
	write_usb(RXMSK,RXFIFO0|RXFIFO2|RXFIFO3|RXOVRN0|RXOVRN2|RXOVRN3);

	/* No FIFO warnings: this is done by DMA */
	write_usb(FWMSK,0);

	/* No DMA */
	write_usb(DMACNTRL,DMA_ON);

	/* ALT evnts */
	write_usb(ALTMSK,SD3|RESET_A);

	/* This is modified in the suspend-resume routines so if any change is
	   made here it needs to be reflected there too. */
	write_usb(MAMSK,INTR_E|RX_EV|NAK|TX_EV|ALT);
	
	/* Enable the receiver and go operational (flush, enable) */  
	FLUSHTX0;                           
	write_usb(RXC0,RX_EN);
	
	/* Go operational */   
	write_usb(NFSR,OPR_ST);             

	/* Set NODE ATTACH */
	write_usb(MCNTRL,INT_H_P|NAT|VGE);
}

/**********************************************************************/
/* This subroutine handles USB 'alternate' events.                    */
/**********************************************************************/
static void usb_alt(int evnt)
{
	/* No printk's in here as some of this is time-critical */
	if (evnt & RESET_A) {
		LOG('R');

                /* Reset event: enter reset state */
		write_usb(NFSR,RST_ST);
		udelay(250);

		/* Set default address */
		write_usb(FAR,AD_EN+0);

		/* Enable EP0, flush & disable */
		write_usb(EPC0, 0x00);
		FLUSHTX0;           

		/* Enable the receiver */
		write_usb(RXC0,RX_EN);

		/* Adjust interrupts */
		write_usb(ALTMSK,RESUME_A|RESET_A);

		/* Go operational */ 
		write_usb(NFSR,OPR_ST);
	} else if(evnt & SD3) { 
#ifdef SUSPEND_RESUME
		/* Suspend event: adjust interrupts */ 
		write_usb(ALTMSK,RESUME_A|RESET_A);

		/* Enter suspend state */  
		write_usb(NFSR,SUS_ST);
#endif
	} else if(evnt & RESUME_A) {
#ifdef SUSPEND_RESUME
		/* Resume event: adjust interrupts */ 
		write_usb(ALTMSK,SD3|RESET_A);

		/* Go operational */ 
		write_usb(NFSR,OPR_ST);
#endif
	} else {
		/* Spurious alt. event! */
#ifdef DEBUG_USB
		printk("USB: usb_alt - unknown (%x)\n",evnt);
#endif
	}
}

static void set_endpoint_stall(int ep, int stall)
{
	byte bit;
	byte state;

	if (stall)
		bit = 0x80;
	else
		bit = 0x00;

	switch (ep)
	{
	case 1:
		state = read_usb(EPC1);
		write_usb(EPC1, (state & ~0x80) | bit);
		break;
	case 5:
		state = read_usb(EPC4);
		write_usb(EPC4, (state & ~0x80) | bit);
		state = read_usb(EPC6);
		write_usb(EPC6, (state & ~0x80) | bit);
		break;
	default:
		printk("Warning: set of endpoint stall state on unused endpoint %d\n", ep);
		break;
	}
}

static int get_endpoint_stall(int ep)
{
	switch (ep)
	{
	case 1:
		/* Endpoint 1 is just endpoint 1 */
		if (read_usb(EPC1) & 0x80)
			return 1;
		else
			return 0;
	case 5: {
		/* Endpoint 5 is actually 4 and 6 ping-ponged */
		int result = 0;
		if (read_usb(EPC4) & 0x80)
			result |= 1;
		if (read_usb(EPC6) & 0x80)
			result |= 2;
		return result;
	}
	default:
		printk("Warning: query for endpoint stall state on unused endpoint %d\n", ep);
		return 0;
	}
}

/**********************************************************************/
/* The CLEAR_FEATURE request is done here                             */  
/**********************************************************************/
static void clrfeature(void)
{
	/* Find request target */
	switch (usb_buf[0]&0x03) {
	case 0:                         /* DEVICE */  
		break;
		
	case 1:                         /* INTERFACE */  
		break;
		
	case 2:                         /* ENDPOINT */  
		/* Clear endpoint stall flag */
		if (usb_buf[3]<=5) {
//			stalld[usb_buf[4] & 7]=0;
#ifdef DEBUG_USB_6
			printk("stall clear on ep%d, current state %d\n",
			       usb_buf[4] & 7, get_endpoint_stall(usb_buf[4] & 7));
#endif
			set_endpoint_stall(usb_buf[4] & 7, 0);
		}
		break;
		
	default:                        /* UNDEFINED */ 
		break;
	}
}

/**********************************************************************/
/* The GET_DESCRIPTOR request is done here                            */  
/**********************************************************************/
static void getdescriptor(void)
{
	int maxlength;

	/* Enter get descriptor mode */
	status.GETDESC=1; 

	/* Store the type requested */
	desc_typ = usb_buf[3];
	if (desc_typ==DEVICE) {
		desc_ptr=DEV_DESC;
		desc_sze=DEV_DESC_SIZE;
        } else if (desc_typ==CONFIGURATION) {
		desc_ptr=CFG_DESC;
		desc_sze=CFG_DESC_SIZE;
	} else if (desc_typ==XSTRING) {
#ifdef DEBUG_USB_2
		printk("  xstring(%d lang %02x%02x)\n",usb_buf[2],usb_buf[5],usb_buf[4]);
#endif
		switch(usb_buf[2]) {
		case 0:
			desc_ptr=UNICODE_AVAILABLE;
			desc_sze=sizeof(UNICODE_AVAILABLE);
			break;

		case 1:
			desc_ptr=UNICODE_MANUFACTURER;
			desc_sze=sizeof(UNICODE_MANUFACTURER);
			break;

		case 2:
			desc_ptr=UNICODE_PRODUCT;
			desc_sze=sizeof(UNICODE_PRODUCT);
			break;
		}
	}
	
	/* Get max length that remote end wants */
	maxlength=usb_buf[6]|(usb_buf[7]<<8);
	if (desc_sze > maxlength) {
#ifdef DEBUG_USB_2
		printk("trimming response to %d bytes (was %d)\n",maxlength,desc_sze);
#endif
		desc_sze = maxlength;    
	}
	
	/* Queue the first data chunk */
	for(desc_idx=0; ((desc_idx<8)&&(desc_idx<desc_sze));)
		write_usb(TXD0,desc_ptr[desc_idx++]);
}

/**********************************************************************/
/* The GET_STATUS request is done here                                */  
/**********************************************************************/
static void getstatus(void)
{
	/* Find request target */
	switch (usb_buf[0]&0x03) {
	case 0:                         /* DEVICE */  
		write_usb(TXD0,0);      /* first byte is reserved */  
		break;
		
	case 1:                         /* INTERFACE */  
		write_usb(TXD0,0);      /* first byte is reserved */   
		break;
		
	case 2: {                       /* ENDPOINT */  
		int ep=usb_buf[3]&7;
		int stall=get_endpoint_stall(ep);
//		write_usb(TXD0,(stalld[ep]?1:0));
		write_usb(TXD0, stall);
#ifdef DEBUG_USB_6
		printk("status ep%d stall=%d\n",ep,stall);
#endif
		break;
	}
		
	default:                        /* UNDEFINED */   
		break;
	}
	
	write_usb(TXD0,0);              /* second byte is reserved */   
}

/**********************************************************************/
/* The SET_CONFIGURATION request is done here                         */  
/**********************************************************************/
static void setconfiguration(void)
{
	struct usb_dev *dev=usb_devices;

	/* Set the configuration # */  
	usb_cfg = usb_buf[2];

	if (usb_buf[2]!=0) {            /* set the configuration */   
		int a;

		/* First PID is DATA0 on all */
		dtapid.TGL0PID=0;
		dtapid.TGL1PID=0;
		dtapid.TGL2PID=0;
		dtapid.TGL3PID=0;

		/* Nothing stalled */
//		for(a=0;a<6;a++) stalld[a]=0;

		/* Disable unused fifos */
		write_usb(EPC1,0);      /* disable EP1 */
		write_usb(EPC2,0);      /* disable EP2 */   
		write_usb(EPC3,0);      /* disable EP3 */   

		/* Transmit on one fifo */
		FLUSHTX3;
		write_usb(EPC5,EP_EN+01);
		usb_txidle=1;

		/* Receive ping-pong on two fifos */
		for(a=0;a<2;a++) {
			/* Configure endpoint */
			write_usb(usb_endpoint[a],EP_EN|05);
			write_usb(usb_rxcontrol[a],RX_EN|FLUSH|WLEVEL);
		}

		/* If there's anything in the tx buffer, kick tx */
		if (dev->tx_used>0) tx_d(1);
	} else {
		/* Unconfigure the device  */
		write_usb(EPC1,0);      /* disable EP1 */
		write_usb(EPC2,0);      /* disable EP2 */   
		write_usb(EPC3,0);      /* disable EP3 */   
		write_usb(EPC4,0);      /* disable EP4 */   
		write_usb(EPC5,0);      /* disable EP5 */   
		write_usb(EPC6,0);      /* disable EP6 */   
	}
}

/**********************************************************************/
/* The SET_FEATURE request is done here                               */  
/**********************************************************************/
static void setfeature(void)
{
	/* Find request target */
	switch (usb_buf[0]&0x03) {
	case 0:                         /* DEVICE */  
		break;
		
	case 1:                         /* INTERFACE */  
		break;
		
	case 2:                         /* ENDPOINT */  
		/* Mark endpoint as stalled */
		if (usb_buf[3]<=5) {
			set_endpoint_stall(usb_buf[4] & 7, 1);
//			stalld[usb_buf[4] & 7]=1;
#ifdef DEBUG_USB_6
			printk("stall set on ep%d\n",usb_buf[3] & 7);
#endif

		}
		break;
		
	default:                        /* UNDEFINED */   
		break;
	}
}

/**********************************************************************/
/* This subroutine handles RX events for FIFO0 (endpoint 0)           */  
/**********************************************************************/
static void rx_0(void)
{
	/* Get receiver status */    
	struct usb_dev *dev=usb_devices;
	int rxstat=read_usb(RXS0);
#ifdef DEBUG_USB_3
	printk("  rx_0(RXS0=%02x)\n",rxstat);
#endif
	LOGS("  rx_0(RXS0=%02x)\n",rxstat);
	
	/* Bump stats */
	dev->stats_ok[0]++;

	/* Is this a setup packet? */  
	if(rxstat & SETUP_R) {
		/* Read data payload into buffer then flush/disable the RX */
		for(desc_idx=0; desc_idx<8; desc_idx++) {
			usb_buf[desc_idx] = read_usb(RXD0);
		}

		/* Disable RX & TX */
		FLUSHRX0;
		FLUSHTX0;
		
		/* If a standard request */
		if ((usb_buf[0]&0x60)==0x00) {
			/* Find request target */
			switch (usb_buf[1]) {
			case CLEAR_FEATURE:
				clrfeature(); 
#ifdef DEBUG_USB_1
				printk("USB: Clear_Feature\n");
#endif
				break;
				
			case GET_CONFIGURATION:                   
#ifdef DEBUG_USB
				printk("USB: Get_Configuration(ret=%x)\n",usb_cfg);
#endif
				/* Load the config value */ 
				write_usb(TXD0,usb_cfg);
				break;
				
			case GET_DESCRIPTOR: 
				getdescriptor();                   
#ifdef DEBUG_USB_3
				printk("USB: Get_Descriptor(%x)\n",usb_buf[3]);
#endif
				break;
				
			case GET_STATUS: 
				getstatus();   
#ifdef DEBUG_USB
				printk("USB: Get_Status\n");
#endif
				break;

			case SET_ADDRESS:
#ifdef DEBUG_USB_3
				printk("USB: Set_Address(%d)\n",usb_buf[2]);
#endif
				/* Set and enable new address for endpoint 0,
				   but set DEF too, so new address doesn't take
				   effect until the handshake completes */
				write_usb(EPC0,DEF);   
				write_usb(FAR,usb_buf[2] | AD_EN);      
				break;
				
			case SET_CONFIGURATION:
				setconfiguration(); 
#ifdef DEBUG_USB_3
				printk("USB: Set_Configuration(%d)\n",usb_buf[2]);
#endif
				break;
				
			case SET_FEATURE: 
				setfeature();  
#ifdef DEBUG_USB_3
				printk("USB: Set_Feature\n");
#endif
				break;
				
			default:      
				/* Unsupported standard req */  
#ifdef DEBUG_USB
				printk("USB: Unsupported standard request\n");
#endif
				break;
			}
		} else {                         
			/* If a non-standard req. */   
#ifdef DEBUG_USB
			printk("USB: Non-standard request\n");
#endif
		}
		
		/* The following is done for all setup packets.  Note that if  
		   no data was stuffed into the FIFO, the result of the
		   following will be a zero-length response. */ 
		write_usb(TXC0,TX_TOGL+TX_EN);

		/* Set NEXT PID state */
		dtapid.TGL0PID=0; 
	} else {   
		/* If not a setup packet, it must be an OUT packet */ 
		/* Are we in get_descr status stage? */
		if (status.GETDESC) { 
			/* Test for errors (zero length, correct PID) */
			if ((rxstat& 0x5F)!=0x10) {
				/* length error?? */
			}
			
			/* Exit get_descr mode */
			status.GETDESC=0; 

			/* Flush TX0 and disable */   
			FLUSHTX0;   
		}
		
		/* Re-enable the receiver */   
		write_usb(RXC0,RX_EN);    
	}

	/* we do this stuff for all rx_0 events */  
}

/**********************************************************************/
/* This subroutine handles TX events for FIFO0 (endpoint 0)           */
/**********************************************************************/
static void tx_0(void)
{
	struct usb_dev *dev=usb_devices;
      	int txstat=read_usb(TXS0);
#ifdef DEBUG_USB_3
	printk("  tx_0(%02x)\n",txstat);
#endif
	LOGS("  tx_0(%02x)\n",txstat);
	
	/* If a transmission has completed successfully, check to see if
	   we have anything else that needs to go out, otherwise turn the 
	   receiver back on */  
	if ((txstat & ACK_STAT) && (txstat & TX_DONE)) {
		/* Flush TX0 and disable */
		FLUSHTX0;

		/* The desc. is sent in pieces; queue another piece if nec.  */
		if(status.GETDESC) {
			if (desc_idx<desc_sze) {
				/* Still got stuff to send: queue it */
				int lim=desc_idx+8;  /*set new max limit */

				/* move the data into the FIFO */  
				for(;((desc_idx<lim)&&(desc_idx<desc_sze));)
					write_usb(TXD0,desc_ptr[desc_idx++]);

				/* Enable TX, choose PID */
				TXEN0_PID;
			} else {
				/* Nothing to send: don't queue anything, this
				   will result in a 0-length packet: also,
				   we're now not sending anything */
				status.GETDESC=0;

				/* All done, reenable RX */
				write_usb(RXC0,RX_EN);
#ifdef DEBUG_USB_3
				printk("Wrote %d bytes out of endpoint 0\n",desc_idx);
#endif
			}

		} else {
			/* All done, reenable RX */
			write_usb(RXC0,RX_EN);
		}

		/* Bump stats */
		dev->stats_ok[1]++;
	} else {	
		/* Otherwise something must have gone wrong with the previous
		   transmission, or we got here somehow we shouldn't have */ 
#ifdef DEBUG_USB
		printk("tx0 error (status=%02x)\n",txstat);
#endif

		/* Bump stats */
		dev->stats_err[1]++;
	}
  
	/* We do this stuff for all tx_0 events */  
}

/**********************************************************************/
/* This subroutine handles RX events for the data FIFOs               */
/**********************************************************************/

/* Deal with RX on a FIFO */
static __inline__ void rx_d(int fifo)
{
	struct usb_dev *dev=usb_devices;
	int rxstat,bytes;

	/* We have a RX event: before we read RXS and clear the status bits,
	   we need to switch FIFOs for the ping-pong */
	write_usb(usb_rxcontrol[1-fifo],RX_EN|FLUSH|WLEVEL);
	write_usb(usb_endpoint[1-fifo],EP_EN|5);
	write_usb(usb_endpoint[fifo],5);
	rxstat=read_usb(usb_rxstatus[fifo]);

	/* Endpoint setup? */
	if(rxstat & SETUP_R) {
		printk("rx_d: setup packet received\n");
	} else if (rxstat & RX_ERR) {
		/* Flush the buffer */
		write_usb(usb_rxcontrol[fifo],FLUSH|WLEVEL);
		
		/* Bump stats */
		dev->stats_err[2]++;
	} else {
		/* While there's stuff in the buffer... (it saturates at 15
		   bytes, so we need to read, empty buffer, and read again
		   until we get to zero) */
		bytes=rxstat&0x0f;
		while(bytes>0) {
			address_usb(usb_rxfifo[fifo]);
			dev->rx_count+=bytes;
			if (bytes<=dev->rx_free) {
				/* Enough space in the buffer */
				dev->rx_used+=bytes;
				dev->rx_free-=bytes;
				while(bytes--) {
					/* Buffer the data */
					dev->rx_buffer[dev->rx_head++]=read_usb_quick();
					if (dev->rx_head==USB_RX_BUFFER_SIZE)
						dev->rx_head=0;
				}
			} else {
				/* Not enough space, slower method */
				while(bytes--) {
					if (dev->rx_free) {
						/* Buffer the data */
						dev->rx_buffer[dev->rx_head++]=read_usb_quick();
						if (dev->rx_head==USB_RX_BUFFER_SIZE)
							dev->rx_head=0;
						dev->rx_used++;
						dev->rx_free--;
					} else {
						/* Trash the data */
						(void)read_usb_quick();
					}
				}
			}

			bytes=read_usb(usb_rxstatus[fifo])&0xf;
		}
		
		/* Wake up anyone that's waiting on read */
		wake_up_interruptible(&dev->rx_wq);
		
		/* Bump stats */
		dev->stats_ok[2]++;
	}
}

/**********************************************************************/
/* This subroutine handles TX events for the data FIFOs               */
/**********************************************************************/
static void tx_d(int force)
{
	struct usb_dev *dev=usb_devices;
	int txstat=read_usb(TXS3),a;
	unsigned char *c=usb_tx;

	/* Force load of buffer? */
	if (force) txstat=ACK_STAT|TX_DONE;
#ifdef DEBUG_USB_3
	printk("tx event stat=%x\n",txstat);
#endif
	LOGS("tx event stat=%x\n", txstat);
	
	/* If a transmission has completed successfully, update the data
	   toggle and queue up a dummy packet */ 
	if ((txstat & ACK_STAT) && (txstat & TX_DONE)) {
		/* Flip the data toggle & flush */
		if (!force) dtapid.TGL3PID=!dtapid.TGL3PID;
		FLUSHTX3;

		/* Sucessfully sent some stuff: bump counts & reset buffer */
		dev->tx_count+=usb_txsize;
		dev->stats_ok[3]++;

		/* If last packet was short, and there's nothing in the buffer to send,
		   then just stop here with TX disabled */
		if (usb_txsize<MAXTXPACKET && dev->tx_used==0) {
#ifdef NO_ZERO_TERM
			usb_txidle=1;
#else	
			/* Just send zero-length packet */
			usb_txidle=0;
			usb_txsize=0;
			TXEN3_PID_NO_TGL;
#endif
			return;
		}

		/* Fill local packet buffer from TX buffer: if there's nothing
		   to send (there might be: we need to be able to send zero
		   length packets to terminate a transfer of an exact multiple
		   of the buffer size), then we'll be sending a 0 byte
		   packet */		
		if ((usb_txsize=dev->tx_used)>MAXTXPACKET) usb_txsize=MAXTXPACKET;
		dev->tx_used-=usb_txsize;
		dev->tx_free+=usb_txsize;
		a=usb_txsize;
		
		/* Put it into the retry buffer and the FIFO */
		address_usb(TXD3);
		while(a--) {
			write_usb_quick(*c++=dev->tx_buffer[dev->tx_tail++]);
			if (dev->tx_tail==USB_TX_BUFFER_SIZE)
				dev->tx_tail=0;
		}

		/* Enable TX, choose PID */ 
		usb_txidle=0;
		TXEN3_PID_NO_TGL;

		/* Wake up anyone that's waiting on write when we've got a decent amount of free space */
		if (dev->tx_free>(USB_TX_BUFFER_SIZE/4)) wake_up_interruptible(&dev->tx_wq);

#ifdef DEBUG_USB_3
		printk("tx_d() queued %d byte packet with PID %d\n",
		       usb_txsize,dtapid.TGL3PID);
#endif
		LOGS("Q(len=%d,pid=%d)\n", usb_txsize, dtapid.TGL3PID);
	} else {
		/* Didn't get an ACK? */
		if (txstat & TX_DONE) {
			dev->stats_err[3]++;
#ifdef DEBUG_USB
			printk("tx_d() no ACK, requeueing with PID %d\n",dtapid.TGL3PID);
#endif
			LOGS("tx_d() no ACK, requeing with PID %d\n", dtapid.TGL3PID);
			/* Empty buffer */
			FLUSHTX3;

			/* Requeue last packet */
			address_usb(TXD3);
			a=usb_txsize;
			while(a--) write_usb_quick(*c++);
			
			/* Enable TX, choose PID */ 
			TXEN3_PID_NO_TGL;
		} else {
			/* TX_DONE not set. Eh?! */
#ifdef DEBUG_USB
			printk("tx_d() tx_done not set\n");
#endif
			LOGS("*TXDNS*");
		}
	}
}

/**********************************************************************/
/* This subroutine handles OUT NAK events for FIFO0 (endpoint 0)      */
/**********************************************************************/
static void nak_0(void)
{
	struct usb_dev *dev=usb_devices;

	/* Important note:  even after servicing a NAK, another NAK
	   interrupt may occur if another 'OUT' or 'IN' packet comes in
	   during our NAK service. */
  
	/* If we're currently doing something that requires multiple 'IN'
	   transactions, 'OUT' requests will get NAKs because the FIFO is
	   busy with the TX data.  Since the 'OUT' here means a premature
	   end to the previous transfer, just flush the FIFO, disable the
	   transmitter, and re-enable the receiver. */
	if (status.GETDESC) {                  /* get_descr status stage? */   
		status.GETDESC=0;              /* exit get_descr mode */

		/* Flush TX0 and disable */  
		FLUSHTX0; 

		/* Re-enable the receiver */   
		write_usb(RXC0,RX_EN);
	}
  
	/* We do this stuff for all nak0 events */   
	dev->stats_nak[0]++;
}

void usb_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct usb_dev *dev=usb_devices;
	int evnt=read_usb(MAEV),evnt2;

	LOG('i');
 again:
        if (evnt&RX_EV) {
		LOG('R');

		/* Check the RX events */  
		evnt2=read_usb(RXEV);

		/* Check for overruns */
		if (evnt2&RXOVRN2) {
			dev->stats_overrun[2]++;
			usb_rxoverruns++;
			LOG('o');
		}

		/* Check for RX data */
		if (evnt2&RXFIFO2) rx_d(0);
		else if (evnt2&RXFIFO3) rx_d(1);
		else if (evnt2&RXFIFO0) rx_0();

	} else if (evnt&TX_EV) {
		LOG('T');

		/* Check the TX events */  
		evnt2=read_usb(TXEV);

		if (evnt2&TXFIFO3) tx_d(0);
		else if (evnt2&TXFIFO0) tx_0();

	} else if (evnt&NAK) {
		LOG('N');

		/* Check the NAK events */
		evnt2=read_usb(NAKEV); 

		if (evnt2&NAK_O0) nak_0();
		if (evnt2&NAK_O2) dev->stats_nak[2]++;
		if (evnt2&NAK_O3) dev->stats_nak[2]++;
	} else if (evnt&ALT) {
		LOG('A');

		/* Check the events */   
		evnt2=read_usb(ALTEV);

		/* Process it elsewhere */
		usb_alt(evnt2);
	} else {
		char buffer[16];
		sprintf(buffer,"O%02x",evnt);
		LOGS(buffer);
	}

	/* Any more pending? */
	if ((evnt=read_usb(MAEV))!=0)
		goto again;

#if 1
	/* Faff with interrupts to get edges: basically, we disable then
	   renable them */
	evnt=read_usb(MAMSK);
	write_usb(MAMSK,0);
	write_usb(MAMSK,evnt);
#endif
}

static int usb_read_procmem(char *buf, char **start, off_t offset, int len, int unused)
{
	struct usb_dev *dev = usb_devices;
	int a,b;
	len = 0;

	len+=sprintf(buf+len,"Control endpoint 0\n");
	len+=sprintf(buf+len,"  %9d RX ok\n",dev->stats_ok[0]);
	len+=sprintf(buf+len,"  %9d RX error\n",dev->stats_err[0]);
	len+=sprintf(buf+len,"  %9d RX nak\n",dev->stats_nak[0]);
	len+=sprintf(buf+len,"  %9d TX ok\n",dev->stats_ok[1]);
	len+=sprintf(buf+len,"  %9d TX error\n\n",dev->stats_err[1]);
	
	len+=sprintf(buf+len,"Overall stats\n");
	len+=sprintf(buf+len,"  %9d RX bytes\n",dev->rx_count);
	len+=sprintf(buf+len,"  %9d TX bytes\n\n",dev->tx_count);

	len+=sprintf(buf+len,"RX stats\n");
	len+=sprintf(buf+len,"  %9d RX ok\n",dev->stats_ok[2]);
	len+=sprintf(buf+len,"  %9d RX error\n",dev->stats_err[2]);
	len+=sprintf(buf+len,"  %9d RX overruns\n",dev->stats_overrun[2]);
	len+=sprintf(buf+len,"  %9d RX nak\n",dev->stats_nak[2]);

	len+=sprintf(buf+len,"TX stats\n");
	len+=sprintf(buf+len,"  %9d TX ok\n",dev->stats_ok[3]);
	len+=sprintf(buf+len,"  %9d TX error\n\n",dev->stats_err[3]);
	len+=sprintf(buf+len,"  %9d TX nak\n",dev->stats_nak[3]);

	len+=sprintf(buf+len,"Log: ");
	b=log_head;
	for(a=0;a<sizeof(log);a++) {
		if (++b==sizeof(log)) b=0;
		if (log[b]) buf[len++]=log[b];
	}
	len+=sprintf(buf+len,"\n");	
	RESETLOG();
	
	return len;
}

struct proc_dir_entry usb_proc_entry = {
	0,			/* inode (dynamic) */
	9, "empeg_usb",  	/* length and name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	0, 			/* size */
	NULL, 			/* use default operations */
	&usb_read_procmem, 	/* function used to read data */
};

/* Device initialisation */
void __init empeg_usb_init(void)
{
        struct usb_dev *dev=usb_devices;
	int result; unsigned long flags;

	/* Reset the log */
	RESETLOG();

	/* Do chip setup */
	init_usb();
	
	/* Allocate buffers */
	dev->tx_buffer=vmalloc(USB_TX_BUFFER_SIZE);
	if (!dev->tx_buffer) {
		printk(KERN_WARNING "Could not allocate memory for USB transmit buffer\n");
		return;
	}

	dev->rx_buffer=vmalloc(USB_RX_BUFFER_SIZE);
	if (!dev->rx_buffer) {
		printk(KERN_WARNING "Could not allocate memory for USB receive buffer\n");
		return;
	}

	/* Initialise buffer bits */
	dev->tx_head=dev->tx_tail=0;
	dev->tx_used=0; dev->tx_free=USB_TX_BUFFER_SIZE;
	dev->rx_head=dev->rx_tail=0;
	dev->rx_used=0; dev->rx_free=USB_RX_BUFFER_SIZE;
	dev->rx_wq = dev->tx_wq = NULL;
	
	/* Claim USB IRQ */
	result=request_irq(EMPEG_IRQ_USBIRQ,usb_interrupt,0,"empeg_usbirq",dev);

	/* Got it ok? */
	if (result==0) {

		/* Enable IRQs on falling edge only (there's an inverter
		   between the 9602 and the SA) */
		GFER|=EMPEG_USBIRQ;
		GRER&=~EMPEG_USBIRQ;
		
		/* Clear edge detect */
		GEDR=EMPEG_USBIRQ;

		/* Dad's home! */
		printk("empeg usb initialised, USBN9602 revision %d\n",
		       read_usb(RID)&0x0f);
	}
	else {
		printk(KERN_ERR "Can't get empeg USBIRQ IRQ %d.\n",
		       EMPEG_IRQ_USBIRQ);
		return;
	}

	/* Get the device */
	result=register_chrdev(EMPEG_USB_MAJOR,"empeg_usb",&usb_fops);
	if (result<0) {
		printk(KERN_WARNING "empeg USB: Major number %d unavailable.\n",
		       EMPEG_USB_MAJOR);
		return;
	}

#ifdef CONFIG_PROC_FS
	proc_register(&proc_root, &usb_proc_entry);
#endif
	/* Queue a 0-byte write to start with */
	save_flags_cli(flags);
	tx_d(1);
	restore_flags(flags);
}

/* Open and release are very simplistic: by the nature of USB, we don't enable
   and disable the device when we open and close the device (which in this
   case is just an endpoint anyway) - otherwise, we wouldn't be able to see
   stuff for endpoint 0, ie configuration and connection events */
static int usb_open(struct inode *inode, struct file *flip)
{
	struct usb_dev *dev=usb_devices;
	
	MOD_INC_USE_COUNT;
	flip->private_data=dev;
	return 0;
}

static int usb_release(struct inode *inode, struct file *flip)
{
	MOD_DEC_USE_COUNT;
	return 0;
}

/* Read data from USB buffer */
static ssize_t usb_read(struct file *flip, char *dest, size_t count, loff_t *ppos)
{
	struct usb_dev *dev=flip->private_data;
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
		if (dev->rx_tail==USB_RX_BUFFER_SIZE)
			dev->rx_tail=0;
	}
	
	/* Protect this bit */
	save_flags_cli(flags);
	dev->rx_free += count;
	restore_flags(flags);

	return count;
}

/* Write data to the USB buffer */
static ssize_t usb_write(struct file *filp, const char *buf, size_t count,
			 loff_t *ppos)
{
	/* This will need heavy mods to cause the device to be kicked */
	struct usb_dev *dev = filp->private_data;
	unsigned long flags;
	size_t bytes;
	
	while (dev->tx_free == 0) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
      
		interruptible_sleep_on(&dev->tx_wq);

		/* If the sleep was terminated by a signal give up */
		if (signal_pending(current))
			return -ERESTARTSYS;
	}
	
	/* How many bytes can we write? */
	save_flags_cli(flags);
	if (count > dev->tx_free) count = dev->tx_free;
	dev->tx_free -= count;
	restore_flags(flags);

	bytes = count;
	while (bytes--) {
		dev->tx_buffer[dev->tx_head++] = *buf++;
		if (dev->tx_head == USB_TX_BUFFER_SIZE)
			dev->tx_head=0;
	}

	/* Protect this bit */
	save_flags_cli(flags);
	dev->tx_used += count;

	/* Do we need to kick the TX? */
	if (usb_txidle) {
		/* TX not going, kick the b'stard (while he's down) */
		tx_d(1);
	}
	restore_flags(flags);

	return count;
}

static unsigned int usb_poll(struct file *filp, poll_table *wait)
{
	struct usb_dev *dev = filp->private_data;
	unsigned int mask = 0;

	poll_wait(filp, &dev->rx_wq, wait);
	poll_wait(filp, &dev->tx_wq, wait);

	/* Is there stuff in the read buffer? */
	if (dev->rx_used)
		mask |= POLLIN | POLLRDNORM;

	/* Is there room in the write buffer? */
	if (dev->tx_free)
		mask |= POLLOUT | POLLWRNORM;

	return mask;
}

static int usb_ioctl(struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	unsigned long flags;
	switch (cmd)
	{
	case 0: /* Get pipe status */
	{
		int ep;
		int *ptr = (int *)arg;

		get_user_ret(ep, ptr, -EFAULT);

		/* Check EP */
		if (ep != 1 && ep != 5)
			return -EINVAL;
		
//		put_user_ret(stalld[ep], ptr + 1, -EFAULT);
		save_flags_cli(flags);
		put_user_ret(get_endpoint_stall(ep), ptr + 1, -EFAULT);
		restore_flags(flags);
		return 0;
	}

	case 1: /* Set pipe status */
	{
		int ep, stall;
		int *ptr = (int *)arg;

		get_user_ret(ep, ptr, -EFAULT);
		get_user_ret(stall, ptr + 1, -EFAULT);

		if (ep != 1 && ep != 5)
			return -EINVAL;

		save_flags_cli(flags);
		set_endpoint_stall(ep, stall);
		restore_flags(flags);
		return 0;
	}
	default:
	    	return -EINVAL;
	}		
	return 0;
}
