/*------------------------------------------------------------------------
 . Empeg_Pegasus_USB.c
 . This is the network interface for the Empegs USB Ethernet emulation. 
 .
 . Copyright (C) 2003 by Mark Jenkins
 . This software may be used and distributed according to the terms
 . of the GNU Public License, incorporated herein by reference.
 .
 . History:
 .	21/JAN/03  Mark Jenkins  Created 
 ----------------------------------------------------------------------------*/

static const char *version =
"Empeg_Pegasus_USB.c:v0.01 21/JAN/03 by Mark Jenkins (mark@marks-house.org)\n";  

#ifdef MODULE
#include <linux/module.h>
#include <linux/version.h>
#endif 

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/ioport.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/delay.h>
#include <linux/errno.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <asm/arch/hardware.h>

/*------------------------------------------------------------------------
 .  
 . Configuration options, for the experienced user to change. 
 .
 -------------------------------------------------------------------------*/


/* PPP */

#define PCMCIA_IOBASE_V 0xe0000000
#define PCMCIA_ATTRBASE_V 0xe8000000
#define OSITECH_AUI_CTL         0x0c
#define OSITECH_PWRDOWN         0x0d
#define OSITECH_RESET           0x0e
#define OSITECH_ISR             0x0f
#define OSITECH_AUI_PWR         0x0c
#define OSITECH_RESET_ISR       0x0e

#define OSI_AUI_PWR             0x40
#define OSI_LAN_PWRDOWN         0x02
#define OSI_MODEM_PWRDOWN       0x01
#define OSI_LAN_RESET           0x02
#define OSI_MODEM_RESET         0x01

#define ERICSSON

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned long int dword;


/* Use the MAC address from my real Netgear FA101 */
unsigned char PegasusStoredMacAddr[] = { 0x00, 0x09, 0x5B, 0x03, 0x7B, 0xDC };
struct device *GlobalHackDev;


#define SWAP(a) ((((a)&0xff00)>>8)|(((a)&0xff)<<8))

ssize_t data_usb_write(char *buf, size_t count);

/* Special function registers for Ositech cards */
#define OSITECH_AUI_CTL         0x0c
#define OSITECH_PWRDOWN         0x0d
#define OSITECH_RESET           0x0e
#define OSITECH_ISR             0x0f
#define OSITECH_AUI_PWR         0x0c
#define OSITECH_RESET_ISR       0x0e

#define OSI_AUI_PWR             0x40
#define OSI_LAN_PWRDOWN         0x02
#define OSI_MODEM_PWRDOWN       0x01
#define OSI_LAN_RESET           0x02
#define OSI_MODEM_RESET         0x01
#define OSI_TECH 1


#if 0
/* Doesn't look like we need this */
static void read_attr(void)
{
  //	int ioaddr=smc_portlist[0];
  volatile unsigned char *attrib_space=(volatile unsigned char *)PCMCIA_ATTRBASE_V;
  unsigned char buffer[256],*p; 
  int len;
  while ( *attrib_space != 0xFF ) {  /* Valid Tuple found */
    p=buffer;
    *p++ = *attrib_space; attrib_space+=2;
    *p++ = len = *attrib_space; attrib_space+=2;
    while ( len > 0 )  {
      *p++ = *attrib_space; 
      attrib_space+=2;
      len--;
    }
    if( attrib_space > (volatile unsigned char *)(PCMCIA_ATTRBASE_V+0x8000))
      break;
			
    switch ( buffer[0] ) {
    case 0x1a:  /* Read Base offset */
      if ( buffer[1] == 5 ) {
	pcmcia_attrbase = buffer[5]<<8 | buffer[4];
      }
      break;
    case 0x20:  /* Manufactor id */
      if ( buffer[1] == 4 ) {
	pcmcia_manuid= buffer[3]<<8 | buffer[2];
	pcmcia_cardid= buffer[5]<<8 | buffer[6];
      }
      break;
    }
  }
  if( pcmcia_manuid == PCMCIA_MANUID_OSITECH )
    pcmcia_attrbase=0x800;
  else
    pcmcia_attrbase=0x8000;
}
#endif



/* 
 . this is for kernels > 1.2.70 
*/
#define REALLY_NEW_KERNEL 
#ifndef REALLY_NEW_KERNEL
#define free_irq( x, y ) free_irq( x )
#define request_irq( x, y, z, u, v ) request_irq( x, y, z, u )
#endif

/* 
 . Wait time for memory to be free.  This probably shouldn't be 
 . tuned that much, as waiting for this means nothing else happens 
 . in the system 
*/
#define MEMORY_WAIT_TIME 16

/*
 . DEBUGGING LEVELS
 . 
 . 0 for normal operation
 . 1 for slightly more details
 . >2 for various levels of increasingly useless information
 .    2 for interrupt tracking, status flags
 .    3 for packet dumps, etc.
*/
/* #define PEGASUS_DEBUG 3 */

#if (PEGASUS_DEBUG > 2 )
#define PRINTK3(x) printk x 
#else 
#define PRINTK3(x) 
#endif

#if PEGASUS_DEBUG > 1 
#define PRINTK2(x) printk x 
#else
#define PRINTK2(x)
#endif

#ifdef PEGASUS_DEBUG
#define PRINTK(x) printk x
#else
#define PRINTK(x)
#endif 


#define CARDNAME "Empeg USB Ethernet Adapter Emulation"


/* store this information for the driver.. */ 
struct EmpegPegasus_local {
  	/*
    	 . these are things that the kernel wants me to keep, so users
    	 . can find out semi-useless statistics of how well the card is
    	 . performing 
  	*/
  	struct enet_statistics stats;
	
  	/* 
     	 . If I have to wait until memory is available to send
     	 . a packet, I will store the skbuff here, until I get the
     	 . desired memory.  Then, I'll send it out and free it.    
  	*/
 	struct sk_buff * saved_skb;

  	/*
   	 . This keeps track of how many packets that I have
   	 . sent out.  When an TX_EMPTY interrupt comes, I know
   	 . that all of these have been sent.
  	*/
  	int	packets_waiting;
};


/*-----------------------------------------------------------------
 .
 .  The driver can be entered at any of the following entry points.
 . 
 .------------------------------------------------------------------  */

/*
 . This is called by  register_netdev().  It is responsible for 
 . checking the portlist for the SMC9000 series chipset.  If it finds 
 . one, then it will initialize the device, find the hardware information,
 . and sets up the appropriate device parameters.   
 . NOTE: Interrupts are *OFF* when this procedure is called.
 .
 . NB:This shouldn't be static since it is referred to externally.
*/
int EmpegPegasus_init(struct device *dev);

/*
 . The kernel calls this function when someone wants to use the device,
 . typically 'ifconfig ethX up'.   
*/
static int EmpegPegasus_open(struct device *dev);

/*
 . This is called by the kernel to send a packet out into the net.  it's
 . responsible for doing a best-effort send, but if it's simply not possible
 . to send it, the packet gets dropped. 
*/  
static int EmpegPegasus_send_packet(struct sk_buff *skb, struct device *dev);

/* 
 . This is called by the kernel in response to 'ifconfig ethX down'.  It
 . is responsible for cleaning up everything that the open routine 
 . does, and maybe putting the card into a powerdown state. 
*/
static int EmpegPegasus_close(struct device *dev);

/*
 . This routine allows the proc file system to query the driver's 
 . statistics.  
*/
static struct enet_statistics * EmpegPegasus_query_statistics( struct device *dev);

#if 0
/*
 . Finally, a call to set promiscuous mode ( for TCPDUMP and related 
 . programs ) and multicast modes.
*/
static void EmpegPegasus_set_multicast_list(struct device *dev);
#endif

/*---------------------------------------------------------------
 . 
 . Interrupt level calls.. 
 .
 ----------------------------------------------------------------*/

/*
 . Handles the actual interrupt 
*/
/* We don't have any ints, we get called from the empeg USB code */

/*
 . This is the Fcn that gets called when we have received ethernet
 . Data over USB. 
*/ 
void EmpegPegasus_rcv(unsigned char * data, int length);

/*
 . This handles a TX interrupt, which is only called when an error
 . relating to a packet is sent. Needed????  
*/
inline static void EmpegPegasus_tx( struct device * dev );

/*
 ------------------------------------------------------------
 . 
 . Internal routines
 .
 ------------------------------------------------------------
*/

/*
 . this routine initializes the cards hardware, prints out the configuration
 . to the system log as well as the vanity message, and handles the setup
 . of a device parameter. 
 . It will give an error if it can't initialize the card.
*/
static int EmpegPegasus_initcard(struct device *); 

/*
 . A rather simple routine to print out a packet for debugging purposes.
*/ 
#if PEGASUS_DEBUG > 2 
static void print_packet( byte *, int );
#endif  

#define tx_done(dev) 1

/* Enable Interrupts, Receive, and Transmit */
static void EmpegPegasus_enable(void);

/* this puts the device in an inactive state */
static void EmpegPegasus_shutdown(void);

/*
  this routine will set the hardware multicast table to the specified 
  values given it by the higher level routines
*/
static void EmpegPegasus_setmulticast( int ioaddr, int count, struct dev_mc_list *  );
static int crc32( char *, int );



static void EmpegPegasus_enable(void) 
{
} 	
	
static void EmpegPegasus_shutdown(void) 
{
}


#ifndef SUPPORT_OLD_KERNEL 
/* 
 . Function: smc_setmulticast( int ioaddr, int count, dev_mc_list * adds )
 . Purpose:
 .    This sets the internal hardware table to filter out unwanted multicast
 .    packets before they take up memory.  
 .    
 .    The SMC chip uses a hash table where the high 6 bits of the CRC of
 .    address are the offset into the table.  If that bit is 1, then the 
 .    multicast packet is accepted.  Otherwise, it's dropped silently.
 .  
 .    To use the 6 bits as an offset into the table, the high 3 bits are the
 .    number of the 8 bit register, while the low 3 bits are the bit within
 .    that register.
 .
 . This routine is based very heavily on the one provided by Peter Cammaert. 
*/


static void EmpegPegasus_setmulticast( int ioaddr, int count, struct dev_mc_list * addrs ) {
#if 0
  int			i;
  unsigned char		multicast_table[ 8 ];
  struct dev_mc_list	* cur_addr;
  /* table for flipping the order of 3 bits */  
  unsigned char invert3[] = { 0, 4, 2, 6, 1, 5, 3, 7 };

  /* start with a table of all zeros: reject all */	
  memset( multicast_table, 0, sizeof( multicast_table ) );

  cur_addr = addrs;
  for ( i = 0; i < count ; i ++, cur_addr = cur_addr->next  ) {
    int position;
		
    /* do we have a pointer here? */
    if ( !cur_addr ) 
      break;
    /* make sure this is a multicast address - shouldn't this
       be a given if we have it here ? */	
    if ( !( *cur_addr->dmi_addr & 1 ) ) 	
      continue;	

    /* only use the low order bits */	
    position = crc32( cur_addr->dmi_addr, 6 ) & 0x3f;
				
    /* do some messy swapping to put the bit in the right spot */
    multicast_table[invert3[position&7]] |= 
      (1<<invert3[(position>>3)&7]);

  }
  /* now, the table can be loaded into the chipset */
  SMC_SELECT_BANK( 3 );
	
  for ( i = 0; i < 8 ; i++ ) {
    outb( multicast_table[i], ioaddr + MULTICAST1 + i );
  }
#endif
}

/*
  Finds the CRC32 of a set of bytes.
  Again, from Peter Cammaert's code. 
*/
static int crc32(char * s, int length ) 
{ 
	/* indices */
	int perByte;
	int perBit;
	/* crc polynomial for Ethernet */
	const unsigned long poly = 0xedb88320;
	/* crc value - preinitialized to all 1's */
 	unsigned long crc_value = 0xffffffff; 

	for (perByte = 0; perByte < length; perByte++) 
	{
    		unsigned char	c;
	
		c = *(s++);
    		for (perBit = 0; perBit < 8; perBit++) 
		{
      			crc_value = (crc_value>>1)^(((crc_value^c)&0x01)?poly:0);
      			c >>= 1;
    		}
  	}
  	return	crc_value;
} 

#endif 

#if 0
/* I don't think we need this, as we just kick it to USB */
/* 
 . Function: smc_wait_to_send_packet( struct sk_buff * skb, struct device * ) 
 . Purpose: 
 .    Attempt to allocate memory for a packet, if chip-memory is not
 .    available, then tell the card to generate an interrupt when it 
 .    is available.
 .
 . Algorithm:
 .
 . o	if the saved_skb is not currently null, then drop this packet
 .	on the floor.  This should never happen, because of TBUSY.
 . o	if the saved_skb is null, then replace it with the current packet,
 . o	See if I can sending it now. 
 . o 	(NO): Enable interrupts and let the interrupt handler deal with it.
 . o	(YES):Send it now.
*/
static int smc_wait_to_send_packet( struct sk_buff * skb, struct device * dev )
{ 
  struct smc_local *lp 	= (struct smc_local *)dev->priv;
  unsigned int ioaddr 	= dev->base_addr;
  word 			length;
  unsigned short 		numPages;
  word			time_out;	
	
  if ( lp->saved_skb) {
    /* THIS SHOULD NEVER HAPPEN. */
    lp->stats.tx_aborted_errors++;
    printk(CARDNAME": Bad Craziness - sent packet while busy.\n" );
    return 1;
  }
  lp->saved_skb = skb;

  length = ETH_ZLEN < skb->len ? skb->len : ETH_ZLEN;
		
  /*
    . the MMU wants the number of pages to be the number of 256 bytes 
    . 'pages', minus 1 ( since a packet can't ever have 0 pages :) ) 
  */
  numPages = length / 256;

  if (numPages > 7 ) {
    printk(CARDNAME": Far too big packet error. \n");
    /* freeing the packet is a good thing here... but should 		
       . any packets of this size get down here?   */
    dev_kfree_skb (skb);
    lp->saved_skb = NULL;
    /* this IS an error, but, i don't want the skb saved */
    return 0; 
  }
  /* either way, a packet is waiting now */
  lp->packets_waiting++;
	 
  /* now, try to allocate the memory */
  SMC_SELECT_BANK( 2 );
  outw( MC_ALLOC | numPages, ioaddr + MMU_CMD );
  /*
    . Performance Hack
    .  
    . wait a short amount of time.. if I can send a packet now, I send
    . it now.  Otherwise, I enable an interrupt and wait for one to be
    . available. 
    .
    . I could have handled this a slightly different way, by checking to
    . see if any memory was available in the FREE MEMORY register.  However,
    . either way, I need to generate an allocation, and the allocation works
    . no matter what, so I saw no point in checking free memory.   
  */ 
  time_out = MEMORY_WAIT_TIME;

  do { 
    //word	status;
    byte status;  // changed by stefan
    status = inb( ioaddr + INTERRUPT );
    if ( status & IM_ALLOC_INT ) { 
      /* acknowledge the interrupt */
      outb( IM_ALLOC_INT, ioaddr + INTERRUPT );
      break;	
    }
  } while ( -- time_out );


  if ( !time_out ) {
    /* oh well, wait until the chip finds memory later */ 
    SMC_ENABLE_INT( IM_ALLOC_INT );
    PRINTK2((CARDNAME": memory allocation deferred. \n"));
    /* it's deferred, but I'll handle it later */
    return 0;
  }
  /* or YES! I can send the packet now.. */
  smc_hardware_send_packet(dev);
	
  return 0;
}	

/*
 . Function:  smc_hardware_send_packet(struct device * )
 . Purpose:	
 .	This sends the actual packet to the SMC9xxx chip.   
 . 
 . Algorithm:
 . 	First, see if a saved_skb is available.    
 .		( this should NOT be called if there is no 'saved_skb'
 .	Now, find the packet number that the chip allocated
 .	Point the data pointers at it in memory 
 .	Set the length word in the chip's memory
 .	Dump the packet to chip memory
 .	Check if a last byte is needed ( odd length packet )
 .		if so, set the control flag right 
 . 	Tell the card to send it 
 .	Enable the transmit interrupt, so I know if it failed
 . 	Free the kernel data if I actually sent it.
*/
static void smc_hardware_send_packet( struct device * dev ) 
{
  struct smc_local *lp = (struct smc_local *)dev->priv;
  byte	 		packet_no;
  struct sk_buff * 	skb = lp->saved_skb;
  word			length;	
  unsigned int		ioaddr;
  byte			* buf;


  ioaddr = dev->base_addr;	

  if ( !skb ) {
    PRINTK((CARDNAME": In XMIT with no packet to send \n"));
    return;
  }
  length = ETH_ZLEN < skb->len ? skb->len : ETH_ZLEN;
  buf = skb->data;

  /* If I get here, I _know_ there is a packet slot waiting for me */	
  packet_no = inb( ioaddr + PNR_ARR + 1 ); /* INB WORKS  ??????? */
  if ( packet_no & 0x80 ) { 
    /* or isn't there?  BAD CHIP! */
    printk(KERN_DEBUG CARDNAME": Memory allocation failed. \n");
    printk("smc91c94 hardware_send_packet %x FREE 0x%x\n", packet_no,(int) skb);
    dev_kfree_skb(skb);
    lp->saved_skb = NULL;
    dev->tbusy = 0;
    return;
  }
  /* we have a packet address, so tell the card to use it */
  outw( packet_no, ioaddr + PNR_ARR );

  /* point to the beginning of the packet */	
  outw( PTR_AUTOINC , ioaddr + POINTER );

  PRINTK3((CARDNAME": Trying to xmit packet of length 0x%x\n", length ));
#if SMC_DEBUG > 2
  print_packet( buf, length );
#endif

  /* send the packet length ( +6 for status, length and ctl byte ) 
     and the status word ( set to zeros ) */ 
#ifdef USE_32_BIT
  outl(  (length +6 ) << 16 , ioaddr + DATA_1 );
#else
  outw( 0, ioaddr + DATA_1 );	 
  /* send the packet length ( +6 for status words, length, and ctl*/		
  outw( (length+6), ioaddr + DATA_1 );	 
  /*
    outb( (length+6) & 0xFF,ioaddr + DATA_1 );
    outb( (length+6) >> 8 , ioaddr + DATA_1 );
  */
#endif 

  /* send the actual data 
     . I _think_ it's faster to send the longs first, and then 
     . mop up by sending the last word.  It depends heavily 
     . on alignment, at least on the 486.  Maybe it would be 
     . a good idea to check which is optimal?  But that could take
     . almost as much time as is saved? 
  */	
#ifdef USE_32_BIT 
  if ( length & 0x2  ) {	
    outsl(ioaddr + DATA_1, buf,  length >> 2 ); 
    outw( *((word *)(buf + (length & 0xFFFFFFFC))),ioaddr +DATA_1);
  }
  else
    outsl(ioaddr + DATA_1, buf,  length >> 2 ); 
#else
  outsw(ioaddr + DATA_1 , buf, (length ) >> 1);
#endif
  /* Send the last byte, if there is one.   */
#if 1 
  /* The odd last byte, if there is one, goes in the control word. */
  outw((length & 1) ? 0x2000 | buf[length-1] : 0, ioaddr + DATA_1 );
#else 	
  if ( (length & 1) == 0 ) {
    outw( 0, ioaddr + DATA_1 );
  } else {
    outb( buf[length -1 ], ioaddr + DATA_1 );
    outb( 0x20, ioaddr + DATA_1);
  }
#endif
  /* enable the interrupts */
  SMC_ENABLE_INT( (IM_TX_INT | IM_TX_EMPTY_INT) );

  /* and let the chipset deal with it */
  outw( MC_ENQUEUE , ioaddr + MMU_CMD );

  /* This delay was found to be required by Borislav, for now we'll
   * just use it as is but a better solution should be available at
   * some point. */
  udelay(1000);
  
  PRINTK2((CARDNAME": Sent packet of length 0x%x \n",length)); 


#ifdef ARNOLD
  PRINTK2((CARDNAME " GPLVL 0x%08x GPDR 0x%08x GAFR 0x%08x GEDR 0x%08x GFER 0x%08x \n", 
	   *(volatile unsigned long *)GPLR, 
	   *(volatile unsigned long *)GPDR, 
	   *(volatile unsigned long *)GAFR,
	   *(volatile unsigned long *)GEDR,
	   *(volatile unsigned long *)GFER
	   ));
#endif

  lp->saved_skb = NULL;

  /*	printk("smc91c94 hardware_send_packet FREE 0x%x\n", (int) skb);	 */
  dev_kfree_skb (skb);

  dev->trans_start = jiffies;

  /* we can send another packet */
  dev->tbusy = 0;


  return;
}
#endif

/*-------------------------------------------------------------------------
 |
 | EmpegPegasus_init( struct device * dev )  
 |   Input parameters: 
 |	We don't need to worry about Autoprobing, as we always exist in SW, 
 |	So we can just initialise ourselves.
 |
 |   Output: 
 |	0 --> there is a device
 |	anything else, error 
 | 
 ---------------------------------------------------------------------------
*/ 
int EmpegPegasus_init(struct device *dev)
{
      return EmpegPegasus_initcard(dev);
}



#ifndef NO_AUTOPROBE
#if 0
/*----------------------------------------------------------------------
 . smc_findirq 
 . 
 . This routine has a simple purpose -- make the SMC chip generate an 
 . interrupt, so an auto-detect routine can detect it, and find the IRQ,
 ------------------------------------------------------------------------
*/
int smc_findirq( int ioaddr ) 
{
  int	timeout = 20;


  /* I have to do a STI() here, because this is called from
     a routine that does an CLI during this process, making it 
     rather difficult to get interrupts for auto detection */
  sti();

  autoirq_setup( 0 );

  /*
   * What I try to do here is trigger an ALLOC_INT. This is done
   * by allocating a small chunk of memory, which will give an interrupt
   * when done.
   */

	  
  SMC_SELECT_BANK(2);	
  /* enable ALLOCation interrupts ONLY */
  outb( IM_ALLOC_INT, ioaddr + INT_MASK );	

  /*
    . Allocate 512 bytes of memory.  Note that the chip was just 
    . reset so all the memory is available
  */
  outw( MC_ALLOC | 1, ioaddr + MMU_CMD );		

  /*
    . Wait until positive that the interrupt has been generated
  */
  while ( timeout ) {
    byte	int_status;

    int_status = inb( ioaddr + INTERRUPT );

    if ( int_status & IM_ALLOC_INT ) 	
      break;		/* got the interrupt */
    timeout--;
  }
  /* there is really nothing that I can do here if timeout fails,
     as autoirq_report will return a 0 anyway, which is what I
     want in this case.   Plus, the clean up is needed in both
     cases.  */

  /* DELAY HERE!
     On a fast machine, the status might change before the interrupt
     is given to the processor.  This means that the interrupt was 
     never detected, and autoirq_report fails to report anything.  
     This should fix autoirq_* problems. 
  */
  SMC_DELAY();
  SMC_DELAY();		
	
  /* and disable all interrupts again */
  outb( 0, ioaddr + INT_MASK );

  /* clear hardware interrupts again, because that's how it
     was when I was called... */
  cli();

  /* and return what I found */
  return autoirq_report( 0 );	
}
#endif
#endif
 

/*---------------------------------------------------------------
 . Here I do typical initialization tasks. 
 . 
 . o  Initialize the structure if needed
 . o  print out my vanity message if not done so already
 . o  print out what type of hardware is detected
 . o  print out the ethernet address
 . o  find the IRQ 
 . o  set up my private data 
 . o  configure the dev structure with my subroutines
 . o  actually GRAB the irq.
 . o  GRAB the region 
 .-----------------------------------------------------------------
*/
static int  EmpegPegasus_initcard(struct device *dev)
{
	int i;

  	static unsigned version_printed = 0;

	printk("EmpegPegasus_initcard()\n");

  	/* see if I need to initialize the ethernet card structure */
  	if (dev == NULL)
	{
		dev = init_etherdev(0, 0);
		if (dev == NULL)
		{
      			return -ENOMEM;
		}
	}

	if (!version_printed++)
	{
    		printk("%s", version);
	}

	/* fill in some of the fields */
  	dev->base_addr = 0;		/* SW emulation of a device remember */

	/*
	 . Get the MAC address
	*/

	for (i = 0; i < 6; i++) 
	{ 
    		dev->dev_addr[i] = PegasusStoredMacAddr[i];	
  	}

	/* Define our Interface port as Ethernet */
	dev->if_port = 1;

  	dev->irq = 0;		/* Derrr, SW device, so no int */

	/* now, print out the card info, in a short format.. */
	
  	printk(CARDNAME ": INTF:Ethernet ");
  	/*
	 . Print the Ethernet address 
  	*/
  	printk("MAC ");
  	for (i = 0; i < 5; i++)
	{
    		printk("%2.2x:", dev->dev_addr[i]);
	}
  	printk("%2.2x\n", dev->dev_addr[5] );


  	/* Initialize the private structure. */
  	if (dev->priv == NULL) 
	{
    		dev->priv = kmalloc(sizeof(struct EmpegPegasus_local), GFP_KERNEL);
    		if (dev->priv == NULL)
		{
      			return -ENOMEM;
		}
  	}
  	/* set the private data to zero by default */
  	memset(dev->priv, 0, sizeof(struct EmpegPegasus_local));

  	/* Fill in the fields of the device structure with ethernet values. */
  	ether_setup(dev);

  	dev->open = EmpegPegasus_open;
  	dev->stop = EmpegPegasus_close;
  	dev->hard_start_xmit = EmpegPegasus_send_packet;
  	dev->get_stats = EmpegPegasus_query_statistics;

	GlobalHackDev = dev;
	return 0;
}

#if PEGASUS_DEBUG > 2
static void print_packet(byte * buf, int length) 
{ 
	int i;
	int remainder;
	int lines;
	
	printk("Packet of length %d \n", length);
	lines = length / 16;
	remainder = length % 16;

  	for (i = 0; i < lines ; i++) 
	{ 
    		int cur;

		for (cur = 0; cur < 8; cur++) 
		{ 
      			byte a, b;

      			a = *(buf++);
      			b = *(buf++);
      			printk("%02x%02x ", a, b);
    		}
    		printk("\n");
  	}

  	for (i = 0; i < remainder/2 ; i++) 
	{
    		byte a, b;

		a = *(buf++);
    		b = *(buf++);
    		printk("%02x%02x ", a, b);
  	}
  	printk("\n");
}
#endif	


/*
 * Initialize some Stuff
 * 
 */
static int EmpegPegasus_open(struct device *dev)
{
  	/* clear out all the junk that was put here before... */
	printk("EmpegPegasus_open()\n");

  	memset(dev->priv, 0, sizeof(struct EmpegPegasus_local));

  	dev->tbusy 	= 0;
  	dev->interrupt  = 0;
  	dev->start 	= 1;
#ifdef MODULE
  	MOD_INC_USE_COUNT;
#endif

  	/* reset the hardware */
	EmpegPegasus_enable();

	return 0;
}

/*--------------------------------------------------------
 . Called by the kernel to send a packet out into the void
 . of the net.  This routine is largely based on 
 . skeleton.c, from Becker.   
 .--------------------------------------------------------
*/
static int EmpegPegasus_send_packet(struct sk_buff *skb, struct device *dev)
{
	unsigned char * TempData;
	unsigned int len;

	/* Add optional Status to the end of the message */
	len = skb->len +4;

	if(len % 0x40 != 0)
	{
		len = len + (0x40 - (len%0x40));

	}
	TempData = kmalloc(len, GFP_KERNEL);
	
	/* Copy the received data into the new area */
	memcpy(TempData, skb->data, skb->len);
	
	/* Add the Status Data */
	TempData[skb->len] = (skb->len) & 0xFF;
	TempData[skb->len+1] = (skb->len >> 8) & 0x0F;
	if(skb->len < 0x40)
	{
		TempData[skb->len+2] = 0x04;
	}
	else if(skb->len > 1518)
	{
		TempData[skb->len+2] = 0x02;
	}
	else
	{
		TempData[skb->len+2] = 0x00;
	}
	TempData[skb->len+3] = 0x00;

#if 0
	/* Print Some Debug*/
	printk("Send\n");
	print_packet(TempData, len);
#endif

	/* Send the frame on, the free the allocated space */
	data_usb_write(TempData, (size_t)len);
	kfree(TempData);
	dev_kfree_skb(skb);
 	return 0;
}

/*--------------------------------------------------------------------
 .
 . This is the main routine of the driver, to handle the device when
 . it needs some attention.
 .
 . So:
 .   first, save state of the chipset
 .   branch off into routines to handle each case, and acknowledge 
 .	    each to the interrupt register
 .   and finally restore state. 
 .  
 ---------------------------------------------------------------------*/
#if 0
#ifdef REALLY_NEW_KERNEL
static void smc_interrupt(int irq, void * dev_id,  struct pt_regs * regs)
#else 
     static void smc_interrupt(int irq, struct pt_regs * regs)
#endif 
{
  struct device *dev 	= dev_id;	
  int ioaddr 		= dev->base_addr;
  struct smc_local *lp 	= (struct smc_local *)dev->priv;

  byte	status;
  word	card_stats;
  byte	mask;
  int	timeout;
  /* state registers */
  word	saved_bank;
  word	saved_pointer;

  PRINTK3((CARDNAME": SMC interrupt started \n"));

  if (dev == NULL) {
    printk(KERN_WARNING  CARDNAME": irq %d for unknown device.\n", 
	   irq);
    /*		disable_irq( irq); */	/* Wichtig sonst immer und immer wieder ! */		
    return;
  }

  /* will Linux let this happen ??  If not, this costs some speed */
  if ( dev->interrupt ) { 
    printk(KERN_WARNING CARDNAME": interrupt inside interrupt.\n");
    return;
  }
		
  dev->interrupt = 1;

  saved_bank = inw( ioaddr + BANK_SELECT );

  SMC_SELECT_BANK(2);
  saved_pointer = inw( ioaddr + POINTER );

  mask = inb( ioaddr + INT_MASK );
  /* clear all interrupts */
  outb( 0, ioaddr + INT_MASK );


  /* set a timeout value, so I don't stay here forever */
  timeout = 4;

  PRINTK2((KERN_WARNING CARDNAME ": MASK IS %x \n", mask ));
  do { 	
    /* read the status flag, and mask it */
    status = inb( ioaddr + INTERRUPT ) & mask;
    if (!status )
      break;

    PRINTK3((KERN_WARNING CARDNAME
	     ": Handling interrupt status %x \n", status )); 

    if (status & IM_RCV_INT) {
      /* Got a packet(s). */
      PRINTK2((KERN_WARNING CARDNAME
	       ": Receive Interrupt\n"));
      smc_rcv(dev);
    } else if (status & IM_TX_INT ) {
      PRINTK2((KERN_WARNING CARDNAME
	       ": TX ERROR handled\n"));
      smc_tx(dev);
      outb(IM_TX_INT, ioaddr + INTERRUPT ); 
    } else if (status & IM_TX_EMPTY_INT ) {
      /* update stats */
      SMC_SELECT_BANK( 0 );
      card_stats = inw( ioaddr + COUNTER );
      /* single collisions */
      lp->stats.collisions += card_stats & 0xF;
      card_stats >>= 4;
      /* multiple collisions */
      lp->stats.collisions += card_stats & 0xF;

      /* these are for when linux supports these statistics */
      SMC_SELECT_BANK( 2 );
      PRINTK2((KERN_WARNING CARDNAME 
	       ": TX_BUFFER_EMPTY handled\n"));
      outb( IM_TX_EMPTY_INT, ioaddr + INTERRUPT );
      mask &= ~IM_TX_EMPTY_INT;
      lp->stats.tx_packets += lp->packets_waiting;
      lp->packets_waiting = 0;

    } else if (status & IM_ALLOC_INT ) {
      PRINTK2((KERN_DEBUG CARDNAME
	       ": Allocation interrupt \n"));
      /* clear this interrupt so it doesn't happen again */
      mask &= ~IM_ALLOC_INT;
		
      smc_hardware_send_packet( dev );
			
      /* enable xmit interrupts based on this */
      mask |= ( IM_TX_EMPTY_INT | IM_TX_INT );

      /* and let the card send more packets to me */
      mark_bh( NET_BH );

      PRINTK2((CARDNAME": Handoff done successfully.\n"));	
    } else if (status & IM_RX_OVRN_INT ) {
      lp->stats.rx_errors++;
      lp->stats.rx_fifo_errors++;			
      outb( IM_RX_OVRN_INT, ioaddr + INTERRUPT );
    } else if (status & IM_EPH_INT ) {
      PRINTK((CARDNAME ": UNSUPPORTED: EPH INTERRUPT \n"));
    } else if (status & IM_ERCV_INT ) {
      PRINTK((CARDNAME ": UNSUPPORTED: ERCV INTERRUPT \n"));
      outb( IM_ERCV_INT, ioaddr + INTERRUPT );
    }
  } while ( timeout -- ); 

#if 1 
  if ( pcmcia_manuid == PCMCIA_MANUID_OSITECH )
    {	/* Retrigger interrupt if needed */
      mask_bits(0x00ff, ioaddr-0x10+OSITECH_RESET_ISR);
      set_bits(0x0300, ioaddr-0x10+OSITECH_RESET_ISR);
    }	
#endif 
	
#ifdef ARNOLD
  /* restore state register */
  *(volatile unsigned long *)GEDR |=  2;	
#endif

  SMC_SELECT_BANK( 2 );
  outb( mask, ioaddr + INT_MASK );
	
  PRINTK3(( KERN_WARNING CARDNAME ": MASK is now %x \n", mask ));
  outw( saved_pointer, ioaddr + POINTER );

  SMC_SELECT_BANK( saved_bank );

  dev->interrupt = 0;
  PRINTK3((CARDNAME ": Interrupt done\n"));
  return;
}
#endif

/*-------------------------------------------------------------
 .
 . EmpegPegasus_rcv -  receive a packet from USB
 .
 --------------------------------------------------------------
*/
void EmpegPegasus_rcv(unsigned char * data, int length)
{
	struct device *dev = GlobalHackDev;
	struct EmpegPegasus_local *lp = (struct EmpegPegasus_local *)dev->priv;
	struct sk_buff  * skb;
    	byte		* skb_data;

	word	packet_length; 
	
	packet_length = length;

  	PRINTK2(("RCV: LENGTH %4x\n", packet_length ));


    	skb = dev_alloc_skb( packet_length + 5); 

    	if (skb == NULL) 
	{	
      		printk(KERN_NOTICE CARDNAME ": Low memory, packet dropped.\n");
      		lp->stats.rx_dropped++;
    	}

    	skb_reserve( skb, 2 );   /* 16 bit alignment */

	skb->dev = dev;
    
	skb_data = skb_put( skb, packet_length);

	memcpy(skb_data, data, packet_length);

#if PEGASUS_DEBUG > 2 	
    	print_packet(skb_data, packet_length);
#endif

    	skb->protocol = eth_type_trans(skb, dev ); 

    	netif_rx(skb);
    	lp->stats.rx_packets++;
  
	return;
}


#if 0
/************************************************************************* 
 . smc_tx
 . 
 . Purpose:  Handle a transmit error message.   This will only be called
 .   when an error, because of the AUTO_RELEASE mode. 
 . 
 . Algorithm:
 .	Save pointer and packet no
 .	Get the packet no from the top of the queue
 .	check if it's valid ( if not, is this an error??? )
 .	read the status word 
 .	record the error
 .	( resend?  Not really, since we don't want old packets around )
 .	Restore saved values 
 ************************************************************************/ 
static void smc_tx( struct device * dev ) 
{
  int	ioaddr = dev->base_addr;
  struct smc_local *lp = (struct smc_local *)dev->priv;
  byte saved_packet;
  byte packet_no;
  word tx_status;


  /* assume bank 2  */

  saved_packet = inb( ioaddr + PNR_ARR );
  packet_no = inw( ioaddr + FIFO_PORTS );
  packet_no &= 0x7F;

  /* select this as the packet to read from */
  outb( packet_no, ioaddr + PNR_ARR ); 
	
  /* read the first word from this packet */	
  outw( PTR_AUTOINC | PTR_READ, ioaddr + POINTER );

  tx_status = inw( ioaddr + DATA_1 );
  PRINTK3((CARDNAME": TX DONE STATUS: %4x \n", tx_status ));
	
  lp->stats.tx_errors++;
  if ( tx_status & TS_LOSTCAR ) lp->stats.tx_carrier_errors++;
  if ( tx_status & TS_LATCOL  ) {
    printk(KERN_DEBUG CARDNAME 
	   ": Late collision occurred on last xmit.\n");
    lp->stats.tx_window_errors++;
  }

  if ( tx_status & TS_SUCCESS ) {  
    printk(CARDNAME": Successful packet caused interrupt \n");
  } 
  /* re-enable transmit */
  SMC_SELECT_BANK( 0 );
  outw( inw( ioaddr + TCR ) | TCR_ENABLE, ioaddr + TCR );

  /* kill the packet */			
  SMC_SELECT_BANK( 2 );
  outw( MC_FREEPKT, ioaddr + MMU_CMD );

  /* one less packet waiting for me */
  lp->packets_waiting--;
		
  outb( saved_packet, ioaddr + PNR_ARR );
  return;
}
#endif

/*----------------------------------------------------
 . EmpegPegasus_close
 . 
 . this makes the board clean up everything that it can
 . and not talk to the outside world.   Caused by
 . an 'ifconfig ethX down'
 .
 -----------------------------------------------------*/
static int EmpegPegasus_close(struct device *dev)
{

	printk("EmpegPegasus_close()\n");
	dev->tbusy = 1;
  	dev->start = 0;

  	/* clear everything */
  	EmpegPegasus_shutdown();

	/* Update the statistics here. */
#ifdef MODULE
  	MOD_DEC_USE_COUNT;
#endif

  	return 0;
}

/*------------------------------------------------------------
 . Get the current statistics.	
 . This may be called with the card open or closed. 
 .-------------------------------------------------------------*/
static struct enet_statistics * EmpegPegasus_query_statistics(struct device *dev) 
{
  	struct EmpegPegasus_local *lp = (struct EmpegPegasus_local *)dev->priv;

  	return &lp->stats;
}
#if 0
/*-----------------------------------------------------------
 . EmpegPegasus_set_multicast_list
 .  
 . This routine will, depending on the values passed to it,
 . either make it accept multicast packets, go into 
 . promiscuous mode ( for TCPDUMP and cousins ) or accept
 . a select set of multicast packets  
*/

static void EmpegPegasus_set_multicast_list(struct device *dev) 
{
  int ioaddr = dev->base_addr;

  SMC_SELECT_BANK(0);

 if ( dev->flags & IFF_PROMISC ) {
      outw( inw(ioaddr + RCR ) | RCR_PROMISC, ioaddr + RCR );
 }
  /* BUG?  I never disable promiscuous mode if multicasting was turned on. 
     Now, I turn off promiscuous mode, but I don't do anything to multicasting
     when promiscuous mode is turned on. 
  */

  /* Here, I am setting this to accept all multicast packets.  
     I don't need to zero the multicast table, because the flag is
     checked before the table is 
  */
 else if (dev->flags & IFF_ALLMULTI) {
	outw( inw(ioaddr + RCR ) | RCR_ALMUL, ioaddr + RCR ); 
 }
  /* We just get all multicast packets even if we only want them
     . from one source.  This will be changed at some future
     . point. */
    else if (dev->mc_count )  { 
      /* support hardware multicasting */
      /* be sure I get rid of flags I might have set */	
      outw( inw( ioaddr + RCR ) & ~(RCR_PROMISC | RCR_ALMUL), 
	    ioaddr + RCR );
      /* NOTE: this has to set the bank, so make sure it is the
	 last thing called.  The bank is set to zero at the top */
      smc_setmulticast( ioaddr, dev->mc_count, dev->mc_list );
    } else  {
      outw( inw( ioaddr + RCR ) & ~(RCR_PROMISC | RCR_ALMUL), 
	    ioaddr + RCR );

      /* 
	 since I'm disabling all multicast entirely, I need to 
	 clear the multicast list 
      */
      SMC_SELECT_BANK( 3 );
      outw( 0, ioaddr + MULTICAST1 ); 
      outw( 0, ioaddr + MULTICAST2 ); 
      outw( 0, ioaddr + MULTICAST3 ); 
      outw( 0, ioaddr + MULTICAST4 ); 
    }
}
#endif 

#ifdef MODULE

static char devicename[9] = { 0, };
static struct device devEmpegPegasus = {
  devicename, /* device name is inserted by linux/drivers/net/net_init.c */
  0, 0, 0, 0,
  0x300, 1,  /* I/O address, IRQ */
  0, 0, 0, NULL, EmpegPegasus_init };

int io = 0;
int irq = 0;
int ifport = 0;

int init_module(void)
{
  int result;

  if (io == 0)
    printk(KERN_WARNING 
	   CARDNAME": You shouldn't use auto-probing with insmod!\n" );

  /* copy the parameters from insmod into the device structure */
  devEmpegPegasus.base_addr = io;
  devEmpegPegasus.irq       = irq;
  devEmpegPegasus.if_port	= ifport;
  if ((result = register_netdev(&devEmpegPegasus)) != 0)
    return result;

  return 0;
}

void cleanup_module(void)
{
  /* No need to check MOD_IN_USE, as sys_delete_module() checks. */
  unregister_netdev(&devEmpegPegasus);

  free_irq(devEmpegPegasus.irq, NULL );
  irq2dev_map[devEmpegPegasus.irq] = NULL;
  release_region(devEmpegPegasus.base_addr, SMC_IO_EXTENT);

  if (devEmpegPegasus.priv)
    kfree_s(devEmpegPegasus.priv, sizeof(struct EmpegPegasus_local));
}

#endif /* MODULE */
