/*
 * empeg-car save state to flash support
 *
 * (C) 1999/2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Mike Crowe, <mac@empeg.com>
 *   Hugo Fiennes, <hugo@empeg.com>
 *
 * This driver handles the emergency flash programming system used by
 * the empeg. Periodically the player will write 128 bytes of memory
 * to this driver. When the power fails this block is written to flash
 * using an algorithm described below. The powerfail interrupt is
 * triggered when the input voltage to the PSUs falls below around
 * 10 volts - we have a bit of time due to the capacitors on the PSU
 * keeping us going!
 *
 * The 128 byte block is double-buffered so that power failure during
 * writing does not cause incorrect data to be written to the flash.
 *
 * To write to the flash you have to enable the write line, which is
 * attached to a GPIO pin - the empeg's flash is 16-bits wide, so we
 * can write a short at a time.
 *
 * To ensure consistency, each block is written with a CRC (126 bytes
 * of data, 2 byte CRC) so that on powerup, the software searches the
 * flash memory for the latest (ie, highest address) block with a
 * correct CRC, and copies this into the ram buffer. It then looks for
 * the first 100% blank (all 0xffff) page in which it will store the
 * updated information at the next powerfail. If no such block exist,
 * an erase page command is issued. Since the flash page is 8k long,
 * we only need to erase the page after 64 powerfails. As the flash
 * is rated at 100,000 erase cycles, this means the flash will give
 * out after 6,400,000 powerfails :)
 *
 * Power-on-seconds and (vague) unixtime are written to the flash on
 * powerdown and restored on powerup: we don't have an RTC, but we
 * can still count power-on time and reset this every time we dock.
 * It's only really useful for vague time measures, like determining
 * wether a tune was last heard before another one - but we could
 * always tune into RDS (or a GPS receiver) and keep it properly
 * locked. These two values account for the first 8 bytes of the
 * block, leaving (128-2-8)=118 bytes for state storage.
 *
 * 1999/03/23 MAC Initial version, memory block implementation only.
 * 1999/03/24 HBF Added actual flash access, dirty flag, etc.
 *                Now stores unixtime and elapsed time in first 8
 *                bytes.
 * 2000/03/04 HBF unixtime only restored on Mk1 empegs - Mk2s have
 *                real time clocks (see include/asm/arch/time.h)
 * 2000/05/20 HBF Support for C3 flash (unlock block before write/erase,
 *                lock afterwards)
 * 2001/01/22 JHR Added reboot notifier for soft reboot.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <asm/ptrace.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/init.h>
#include <asm/system.h>
#include <asm/arch/hardware.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <linux/empeg.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/notifier.h>

#define DEBUG 0

/* crctab calculated by Mark G. Mendel, Network Systems Corporation */
unsigned short crctab[256] = {
    0x0000,  0x1021,  0x2042,  0x3063,  0x4084,  0x50a5,  0x60c6,  0x70e7,
    0x8108,  0x9129,  0xa14a,  0xb16b,  0xc18c,  0xd1ad,  0xe1ce,  0xf1ef,
    0x1231,  0x0210,  0x3273,  0x2252,  0x52b5,  0x4294,  0x72f7,  0x62d6,
    0x9339,  0x8318,  0xb37b,  0xa35a,  0xd3bd,  0xc39c,  0xf3ff,  0xe3de,
    0x2462,  0x3443,  0x0420,  0x1401,  0x64e6,  0x74c7,  0x44a4,  0x5485,
    0xa56a,  0xb54b,  0x8528,  0x9509,  0xe5ee,  0xf5cf,  0xc5ac,  0xd58d,
    0x3653,  0x2672,  0x1611,  0x0630,  0x76d7,  0x66f6,  0x5695,  0x46b4,
    0xb75b,  0xa77a,  0x9719,  0x8738,  0xf7df,  0xe7fe,  0xd79d,  0xc7bc,
    0x48c4,  0x58e5,  0x6886,  0x78a7,  0x0840,  0x1861,  0x2802,  0x3823,
    0xc9cc,  0xd9ed,  0xe98e,  0xf9af,  0x8948,  0x9969,  0xa90a,  0xb92b,
    0x5af5,  0x4ad4,  0x7ab7,  0x6a96,  0x1a71,  0x0a50,  0x3a33,  0x2a12,
    0xdbfd,  0xcbdc,  0xfbbf,  0xeb9e,  0x9b79,  0x8b58,  0xbb3b,  0xab1a,
    0x6ca6,  0x7c87,  0x4ce4,  0x5cc5,  0x2c22,  0x3c03,  0x0c60,  0x1c41,
    0xedae,  0xfd8f,  0xcdec,  0xddcd,  0xad2a,  0xbd0b,  0x8d68,  0x9d49,
    0x7e97,  0x6eb6,  0x5ed5,  0x4ef4,  0x3e13,  0x2e32,  0x1e51,  0x0e70,
    0xff9f,  0xefbe,  0xdfdd,  0xcffc,  0xbf1b,  0xaf3a,  0x9f59,  0x8f78,
    0x9188,  0x81a9,  0xb1ca,  0xa1eb,  0xd10c,  0xc12d,  0xf14e,  0xe16f,
    0x1080,  0x00a1,  0x30c2,  0x20e3,  0x5004,  0x4025,  0x7046,  0x6067,
    0x83b9,  0x9398,  0xa3fb,  0xb3da,  0xc33d,  0xd31c,  0xe37f,  0xf35e,
    0x02b1,  0x1290,  0x22f3,  0x32d2,  0x4235,  0x5214,  0x6277,  0x7256,
    0xb5ea,  0xa5cb,  0x95a8,  0x8589,  0xf56e,  0xe54f,  0xd52c,  0xc50d,
    0x34e2,  0x24c3,  0x14a0,  0x0481,  0x7466,  0x6447,  0x5424,  0x4405,
    0xa7db,  0xb7fa,  0x8799,  0x97b8,  0xe75f,  0xf77e,  0xc71d,  0xd73c,
    0x26d3,  0x36f2,  0x0691,  0x16b0,  0x6657,  0x7676,  0x4615,  0x5634,
    0xd94c,  0xc96d,  0xf90e,  0xe92f,  0x99c8,  0x89e9,  0xb98a,  0xa9ab,
    0x5844,  0x4865,  0x7806,  0x6827,  0x18c0,  0x08e1,  0x3882,  0x28a3,
    0xcb7d,  0xdb5c,  0xeb3f,  0xfb1e,  0x8bf9,  0x9bd8,  0xabbb,  0xbb9a,
    0x4a75,  0x5a54,  0x6a37,  0x7a16,  0x0af1,  0x1ad0,  0x2ab3,  0x3a92,
    0xfd2e,  0xed0f,  0xdd6c,  0xcd4d,  0xbdaa,  0xad8b,  0x9de8,  0x8dc9,
    0x7c26,  0x6c07,  0x5c64,  0x4c45,  0x3ca2,  0x2c83,  0x1ce0,  0x0cc1,
    0xef1f,  0xff3e,  0xcf5d,  0xdf7c,  0xaf9b,  0xbfba,  0x8fd9,  0x9ff8,
    0x6e17,  0x7e36,  0x4e55,  0x5e74,  0x2e93,  0x3eb2,  0x0ed1,  0x1ef0
};

#define updcrc(cp, crc) ( crctab[((crc >> 8) & 255)] ^ (crc << 8) ^ (cp))

/* How big is each block written to the flash */
#define STATE_BLOCK_SIZE	128
#define STATE_BASE		((volatile unsigned short*)EMPEG_FLASHBASE+(0x4000/sizeof(short)))
#define STATE_BLOCKS		(0x2000/STATE_BLOCK_SIZE)
#define POWERFAIL_TIMEOUT	2 /*seconds*/
#define REENABLE_TIMEOUT	5 /*seconds*/

/* Flash commands */
#define FLASH_READID		0x90
#define FLASH_PROGRAM		0x40
#define FLASH_ERASE1		0x20
#define FLASH_ERASE2		0xd0
#define FLASH_READ		0xff
#define FLASH_BUSYBIT		0x80

/* C3 flash commands */
#define FLASH_UNLOCK1		0x60
#define FLASH_UNLOCK2		0xd0
#define FLASH_LOCK1		0x60
#define FLASH_LOCK2		0x01

/* Product IDs */
#define FLASH_B3		0x0089
#define FLASH_C3		0x88c1

/* We only support one device */
struct state_dev
{
	unsigned char *read_buffer;
	unsigned char *write_buffer;
	unsigned char *buffers[2];
	struct timer_list powerfail_timer;
};

static struct state_dev state_devices[1];

/* Used to disallow multiple opens. */
static int users = 0;

/* Address to write state back to on powerfail */
static volatile unsigned short *savebase = NULL;

/* Flash type */
static int flash_manufacturer,flash_product;

/* Has buffer been modified? */
       int empeg_state_dirty = 0;
#define dirty empeg_state_dirty

static int powerfail_disable_count=0;
static int erroneous_interrupts = 0;

/* Unixtime and elapsed power on time at power-on */
static unsigned int unixtime=0;
static unsigned int powerontime=0;

static void powerfail_disabled_timeout(unsigned long);
static void powerfail_reenabled_timeout(unsigned long);

void enable_powerfail(int enable)
{
	unsigned long flags;
	save_flags_cli(flags);
	if (enable)
		--powerfail_disable_count;
	else
		++powerfail_disable_count;
	restore_flags(flags);

#if DEBUG
	printk("Powerfail is now %s (%d)\n", (powerfail_disable_count == 0) ? "enabled" : "disabled", powerfail_disable_count);
	if (powerfail_disable_count < 0)
		printk("\n\n\n\nBAD! powerfail disable count fallen below zero to %d.\n\n\n\n", powerfail_disable_count);
#endif
}

inline int powerfail_enabled(void)
{
	return (powerfail_disable_count == 0);
}

static inline void state_enablewrite(void)
{
	/* Enable write signal to flash */
	GPCR=EMPEG_FLASHWE;
}

static inline void state_disablewrite(void)
{
	/* Disable write signal to flash */
	GPSR=EMPEG_FLASHWE;
}

static int state_makecrc(volatile unsigned char *buffer, int length)
{
	/* Calculate CRC for an entire block */
	int crc=0xffff;
	while(--length>=0) {
		crc=updcrc(*buffer,crc);
		buffer++;
	}

	return(crc&0xffff);
}

static void state_getflashtype(void)
{
	volatile unsigned short *p=(volatile unsigned short*)EMPEG_FLASHBASE;

	/* Enable writes to flash */
	state_enablewrite();

	/* Read the manufacturer & product */
	p[0]=FLASH_READID;
	flash_manufacturer=p[0];
	p[0]=FLASH_READID;
	flash_product=p[1];
	p[0]=FLASH_READ;

	/* Disable writes again */
	state_disablewrite();
}

int empeg_state_restore (unsigned char *buffer)
{
	/* EMPEG_FLASHBASE+0x4000 to +0x5fff is the space used for
	   the power-down state saving: we work through here until
	   we find the last entry which has a valid CRC - this is
	   the one we use */
	int a,calculated_crc,stored_crc, result = 0;
	struct timeval t;

	/* Find last valid block */
	for(a=(STATE_BLOCKS-1);a>=0;a--) {
		volatile unsigned short *blockptr=STATE_BASE+(a*(STATE_BLOCK_SIZE/sizeof(short)));

		/* See if this has a good CRC */
		calculated_crc=state_makecrc((unsigned char*)blockptr,STATE_BLOCK_SIZE-2);
		stored_crc=blockptr[(STATE_BLOCK_SIZE/sizeof(short))-1];

		if (calculated_crc==stored_crc) {
			/* Copy from flash */
			memcpy(buffer,(void*)blockptr,STATE_BLOCK_SIZE);
			
			break;
			}
	}

	/* Nothing valid found? Return nulls */
	if (a<0) {
		result = 1;	// failed
		memset(buffer,0,STATE_BLOCK_SIZE);
		printk("empegr_state_restore: FAILED\n");
	}

	/* Later empegs have an RTC */
	if (empeg_hardwarerevision()<6) {
		/* Before we go: the first 4 bytes of the block are the elapsed
		   unixtime: set it */
		unixtime=t.tv_sec=*((unsigned int*)buffer);
		t.tv_usec=0;
		do_settimeofday(&t);
	}

	/* Get power-on time */
	powerontime=*((unsigned int*)(buffer+4));
	return result;
}

static int state_fetch(unsigned char *buffer)
{
	/* EMPEG_FLASHBASE+0x4000 to +0x5fff is the space used for
	   the power-down state saving: we work through here until
	   we find the last entry which has a valid CRC - this is
	   the one we use */
	int a;

	(void)empeg_state_restore(buffer);

	/* Nowhere to save, yet */
	savebase=NULL;

	/* Work forward until we find a totally blank page */
	for(a=0;a<STATE_BLOCKS;a++) {
		volatile unsigned short *blockptr=STATE_BASE+(a*(STATE_BLOCK_SIZE/sizeof(short)));
		int b;

		/* Look for a totally blank block */
		for(b=0;b<STATE_BLOCK_SIZE;b+=sizeof(short)) if(blockptr[b]!=0xffff) break;

		/* Blank? */
		if (b==STATE_BLOCK_SIZE) {
			savebase=blockptr;
			break;
		}
	}

	/* Found NO blank blocks? Erase it totally in that case */
	if (a==STATE_BLOCKS) {
		int status;

		state_enablewrite();

		/* Unlock if necessary */
		if (flash_product==FLASH_C3) {
			*STATE_BASE=FLASH_UNLOCK1;
			*STATE_BASE=FLASH_UNLOCK2;
		}

		/* Send erase command */
		*STATE_BASE=FLASH_ERASE1;
		*STATE_BASE=FLASH_ERASE2;

		/* Wait for erase to complete */
		while(!((status=*STATE_BASE)&FLASH_BUSYBIT));
			
		/* Lock if necessary */
		if (flash_product==FLASH_C3) {
			*STATE_BASE=FLASH_LOCK1;
			*STATE_BASE=FLASH_LOCK2;
		}

		/* Ensure we're in read mode */
		*STATE_BASE=FLASH_READ;

		state_disablewrite();
		
		/* Next block goes at the start */
		savebase=STATE_BASE;
	}

	return(0);
}

static inline int state_store(void)
{
	extern void hijack_save_settings (unsigned char *buf);

	/* Store the contents of read_buffer to flash, at savebase */
	int a,status,crc=0xffff,data;
	volatile unsigned short *from=(volatile unsigned short*)state_devices[0].read_buffer;
	
	/* Store current unixtime */
	*((unsigned int*)from)=xtime.tv_sec;

	/* Store current power-on time */
	*((unsigned int*)(from+2))=(xtime.tv_sec-unixtime)+powerontime;

	/* Store hijack_savearea */
	hijack_save_settings((unsigned char *)from);

	/* Enable writes to flash chip */
	state_enablewrite();
	
	/* Unlock if necessary */
	if (flash_product==FLASH_C3) {
		*STATE_BASE=FLASH_UNLOCK1;
		*STATE_BASE=FLASH_UNLOCK2;
	}

	/* For each halfword in the state block... */
	for(a=0;a<STATE_BLOCK_SIZE;a+=sizeof(short)) {
		/* Program word */
		*savebase=FLASH_PROGRAM;
		data=(a==(STATE_BLOCK_SIZE-2)?crc:(*from++));
		*savebase++=data;

		/* Update CRC (flash will be busy here) */
		crc=updcrc(data&0xff,crc);
		crc=updcrc(data>>8,crc);

		/* Wait for completion */
		while(!((status=*savebase)&0x80));

		/* Programmed OK? */
		if (status&0x1a) {
			printk("F%x\n",((int)savebase)&0xffff);
		}
	}

	/* Lock if necessary */
	if (flash_product==FLASH_C3) {
		*STATE_BASE=FLASH_LOCK1;
		*STATE_BASE=FLASH_LOCK2;
	}
	
	/* Back to read array mode */
	*STATE_BASE=FLASH_READ;

	state_disablewrite();
	
	/* Cleansed */
	dirty=0;

	return 0;
}

/* Forced cleanse routine, usually called just before a software-initiated
   powerdown */
extern void state_cleanse(void)
{
	/* Is the state dirty? Flush it if it is */
	if (dirty) {
		unsigned long flags;
		save_flags_cli(flags);
		state_store();
		restore_flags(flags);
	}
}

static void powerfail_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	if (powerfail_enabled()) {
		/* Power is failing, quickly do things... */
		struct state_dev *dev = state_devices;

		/* Mute audio */
		GPCR=EMPEG_DSPPOM;

		/* Store state if it's changed, or if we've been
                   powered on for 30+ seconds */
		if (dirty || ((unsigned int)xtime.tv_sec-unixtime)>=30) state_store();

		/* NOTE! This used to be BEFORE the dirty save, but on the
		   issue9 and later players, turning the display off involves
		   sending a command serially to the power PIC, which can take
		   time. Doing it here is probably ok */

#if CONFIG_EMPEG_DISPLAY
		/* Turn display off: this will buy us a little time
		   due to decreased power drain. We also disable the
		   LCD controller as this stops static junk being on
		   the screen during powerdown */
		display_powerfail_action();
#endif
		
		/* Something so we can see how close the actual
                   powerfail *is*! */
#if DEBUG
		  printk("The quick brown fox jumped over the lazy dog.\n");
#endif

		/* Queue up a powerfail timeout call just in case the
		 * power hasn't really gone away. */
		if (timer_pending(&dev->powerfail_timer))
			del_timer(&dev->powerfail_timer);
		dev->powerfail_timer.expires = jiffies + POWERFAIL_TIMEOUT * HZ;
		dev->powerfail_timer.function = powerfail_disabled_timeout;
		add_timer(&dev->powerfail_timer);
		
		/* We don't want multiple interrupts happening. */
		enable_powerfail(FALSE);
	} else
		erroneous_interrupts++;

	/* Clear edge detect register */
	GEDR = EMPEG_POWERFAIL;
}

static void powerfail_disabled_timeout(unsigned long unused)
{
	/* If we get here then a powerfail interrupt happened but the
	 * power didn't actually go away within a sensible amount of
	 * time. So, if the voltage has gone back up above the
	 * threshold we reenable the interrupt and wait for a bit
	 * longer. If the voltage hasn't gone back up enough we just
	 * reschedule ourselves to take a look at it again in a little
	 * while.
         */
	
	struct state_dev *dev = state_devices;

	unsigned long dis;
	save_flags_cli(dis);
#if DEBUG
	  printk("The power doesn't seem to have gone away after all.\n");
#endif

#ifdef CONFIG_EMPEG_DISPLAY
	/* Re-enable powerfail processing */
	display_powerreturn_action();
#endif

	/* Reenable DACs */
	GPSR=EMPEG_DSPPOM;

	if (timer_pending(&dev->powerfail_timer))
		del_timer(&dev->powerfail_timer);
	
	if (GPLR & EMPEG_POWERFAIL) {
		/* It's high, so we're lower than 10 volts. Just
		 * reschedule ourselves to take another look at it
		 *  later.
		 */
		dev->powerfail_timer.expires = jiffies + POWERFAIL_TIMEOUT * HZ;
		dev->powerfail_timer.function = powerfail_disabled_timeout;
		add_timer(&dev->powerfail_timer);
#if DEBUG
		printk("The voltage is still too low, not reenabling powerfail.\n");
#endif
	} else {
#if DEBUG
		printk("The voltage has gone high enough, reenabling powerfail tentatitively.\n");
#endif
		/* It's low, so we've gone back to above 10 volts. Reenable the
		 *  interrupt and schedule the reenable function so that it can check
		 * we haven't had any spurious interrupts too often.
		 */
		dev->powerfail_timer.expires = jiffies + REENABLE_TIMEOUT * HZ;
		dev->powerfail_timer.function = powerfail_reenabled_timeout;
		add_timer(&dev->powerfail_timer);
		
		erroneous_interrupts = 0;
	}
	restore_flags(dis);
}

static void powerfail_reenabled_timeout(unsigned long unused)
{
	/* If we get here then the power returned a while ago and we reenabled
	 * stuff.  */
	
	struct state_dev *dev = state_devices;
	unsigned long dis;
	save_flags_cli(dis);
	
	if (erroneous_interrupts) {
#if DEBUG
		  printk("The power interrupt is happening too often. Can't enable it.\n");
#endif
		if (timer_pending(&dev->powerfail_timer))
			del_timer(&dev->powerfail_timer);
		dev->powerfail_timer.expires = jiffies + POWERFAIL_TIMEOUT * HZ;
		dev->powerfail_timer.function = powerfail_reenabled_timeout;
		add_timer(&dev->powerfail_timer);
		erroneous_interrupts = 0;
	} else {
#if DEBUG
		  printk("The power hasn't failed for a while - reenabling actions.\n");
#endif

		if (timer_pending(&dev->powerfail_timer))
			del_timer(&dev->powerfail_timer);
		enable_powerfail(TRUE);
	}
	restore_flags(dis);
}



static int state_open(struct inode *inode, struct file *filp)
{
	struct state_dev *dev = state_devices;

	/* Hmm, is there a race condition here? */
	if (users && filp->f_flags)	// allow subsequent O_RDONLY opens only
		return -EBUSY;

	if (filp->f_flags)
		users++;
	MOD_INC_USE_COUNT;

	/* Everything is set up on initialisation */
	filp->private_data = dev;
	
	return 0;
}

static int state_release(struct inode *inode, struct file *filp)
{
	if (filp->f_flags)
		--users;
	MOD_DEC_USE_COUNT;
	return 0;
}

/* Read the contents of the buffer. Must read the whole lot - there is
 * no support for reading only part of it. If someone tries then
 * they'll get EINVAL.
 */

static ssize_t state_read(struct file *filp, char *dest, size_t count,
			  loff_t *ppos)
{
	struct state_dev *dev = filp->private_data;

	if (count > STATE_BLOCK_SIZE || count <= 0)
		count = STATE_BLOCK_SIZE;

	copy_to_user_ret(dest, dev->read_buffer, count, -EFAULT);

	return count;
}

typedef struct savearea_fields_s {
	long		filler0;
	long		filler1;
	int		filler2;
	unsigned	filler3   :2;
	unsigned	ac_volume :7;
	unsigned	dc_volume :7;
	unsigned	filler4   :16;
} savearea_fields_t;

extern int hijack_volumelock_enabled;


static ssize_t state_write(struct file *filp, const char *source, size_t count,
		     loff_t *ppos)
{
	struct state_dev *dev = filp->private_data;
	unsigned long flags;
	unsigned char *temp;

	if (count > STATE_BLOCK_SIZE || count <= 0)
		return -EINVAL;

	copy_from_user_ret(dev->write_buffer, source, count, -EFAULT);

	if (hijack_volumelock_enabled) {
		savearea_fields_t	*new = (savearea_fields_t *)dev->write_buffer;
		savearea_fields_t	*old = (savearea_fields_t *)dev->read_buffer;
		new->ac_volume = old->ac_volume;
		new->dc_volume = old->dc_volume;
	}

	/* Now we've written, switch the buffers and copy */
	save_flags_cli(flags);
	temp = dev->read_buffer;
	dev->read_buffer = dev->write_buffer;
	dev->write_buffer = temp;


	/* Mark as dirty */
	dirty=1;

	restore_flags(flags);

	memcpy(dev->write_buffer, dev->read_buffer, count);
	return count;	
}


int state_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
	     unsigned long arg)
{
	/*struct state_dev *dev = filp->private_data;*/

	switch(cmd)
	{
	case EMPEG_STATE_FORCESTORE:
		return state_store();
		break;

	case EMPEG_STATE_FAKEPOWERFAIL:
	{
		unsigned long flags;
		save_flags_cli(flags);
		powerfail_interrupt(0,NULL,NULL);
		restore_flags(flags);
		return 0;
	}
	default:
		return -EINVAL;
	}
}

static struct file_operations state_fops = {
	NULL, /* state_lseek */
	state_read,
	state_write,
	NULL, /* state_readdir */
	NULL, /* state_select */
	state_ioctl,
	NULL, /* state_mmap */
	state_open,
	NULL, /* state_flush */
	state_release,
};

int state_read_procmem(char *buf, char **start, off_t offset, int len, int unused)
{
	/*struct state_dev *dev = state_devices;*/
	len = 0;

	len += sprintf(buf+len, "PowerOnSeconds=%ld\n",(xtime.tv_sec-unixtime)+powerontime);
	len += sprintf(buf+len, "SaveBase=%p\n",savebase);
	len += sprintf(buf+len, "DirtyFlag=%d\n",dirty);
	return len;
}

static struct proc_dir_entry state_proc_entry = {
	0,			/* inode (dynamic) */
	11, "empeg_state",  	/* length and name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	0, 			/* size */
	NULL, 			/* use default operations */
	&state_read_procmem, 	/* function used to read data */
};

static int empeg_state_reboot_notifier(struct notifier_block *block,
				       unsigned long event,
				       void *buffer);

static struct notifier_block empeg_state_notifier_block = {
	empeg_state_reboot_notifier,
	NULL,
	0
};

void __init empeg_state_init(void)
{
	struct state_dev *dev = state_devices;
	unsigned char *buffer = NULL;
	extern unsigned char **empeg_state_writebuf;
	int result;
		
	/* First grab the memory buffer */
	buffer = vmalloc(STATE_BLOCK_SIZE * 2);
	if (!buffer) {
		printk(KERN_WARNING "Could not allocate memory buffer for empeg state.\n");
		return;
	}

	dev->buffers[0] = buffer;
	dev->buffers[1] = buffer + STATE_BLOCK_SIZE;

	dev->read_buffer = dev->buffers[0];
	dev->write_buffer = dev->buffers[1];
	empeg_state_writebuf = &dev->write_buffer;

	/* Get the flash product ID to work out if it's a B3 or C3 flash */
	state_getflashtype();

	/* Fetch the last correct state from flash into buffer */
	state_fetch(dev->buffers[0]);

	/* Copy the current state to other buffer */
	memcpy(dev->buffers[1],dev->buffers[0],STATE_BLOCK_SIZE);

	/* Ensure the IRQ is disabled at source */
	GRER&=~EMPEG_POWERFAIL;
	GEDR=EMPEG_POWERFAIL;

#if DEBUG
 	printk("Powerfail is now %s (%d)\n", (powerfail_disable_count == 0) ? "enabled" : "disabled", powerfail_disable_count);
	printk("Powerfail line current level is %d\n", GPLR & EMPEG_POWERFAIL);
#endif
	result = request_irq(EMPEG_IRQ_POWERFAIL, powerfail_interrupt, SA_INTERRUPT,
			     "empeg_state", dev);
	
	if (result) {
		printk(KERN_ERR "Can't get empeg powerfail IRQ %d.\n", EMPEG_IRQ_POWERFAIL);
	}

	result = register_chrdev(EMPEG_STATE_MAJOR, "empeg_state", &state_fops);
	if (result < 0) {
		printk(KERN_WARNING "empeg state: Major number %d unavailable.\n",
			   EMPEG_STATE_MAJOR);
		return;
	}
#ifdef CONFIG_PROC_FS
	proc_register(&proc_root, &state_proc_entry);
#endif

	/* Initialise the timer for handling timeout of a powerfail. */
	init_timer(&dev->powerfail_timer);
	dev->powerfail_timer.data = 0;
	
	printk("empeg state support initialised %04x/%04x (save to %p).\n",
	       flash_manufacturer,flash_product,savebase);

	/* Enable powerfail interrupts if the voltage level isn't already too low */
	if (GPLR & EMPEG_POWERFAIL) {
		/* Pretend we've just received a powerfail interrupt */
		powerfail_disable_count = 1;
		dev->powerfail_timer.expires = jiffies + POWERFAIL_TIMEOUT * HZ;
		dev->powerfail_timer.function = powerfail_disabled_timeout;
		add_timer(&dev->powerfail_timer);		
	} else
		powerfail_disable_count = 0;
#if DEBUG
	printk("Powerfail is now %s (%d)\n", (powerfail_disable_count == 0) ? "enabled" : "disabled", powerfail_disable_count);
#endif
	
	/* We want interrupts on rising only */
	GRER|=EMPEG_POWERFAIL;

	register_reboot_notifier(&empeg_state_notifier_block);
}

static inline void empeg_state_cleanup(void)
{
	int result;
	struct state_dev *dev = state_devices;

	unregister_reboot_notifier(&empeg_state_notifier_block);
	state_cleanse();
	
	free_irq(EMPEG_IRQ_POWERFAIL, dev);

	/* No longer require interrupts */
	GFER&=~(EMPEG_POWERFAIL);
	result = unregister_chrdev(EMPEG_STATE_MAJOR, "empeg_state");
	if (result < 0)
		printk(KERN_WARNING "empeg state: Unable to unregister device.\n");

	printk("empeg state cleanup complete.\n");
}

static int empeg_state_reboot_notifier(struct notifier_block *block,
				       unsigned long event,
				       void *buffer)
{
	state_cleanse();
	return NOTIFY_OK;
}

#ifdef MODULE
MODULE_AUTHOR("Mike Crowe/Hugo Fiennes");
MODULE_DESCRIPTION("A driver for empeg state storage to flash support");
MODULE_SUPPORTED_DEVICE("empeg_state");

EXPORT_NO_SYMBOLS

int init_module(void)
{
	return empeg_state_init();
}

void cleanup_module(void)
{
	empeg_state_cleanup();
}

#endif /* MODULE */
