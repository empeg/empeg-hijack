/* empeg power-pic support
 *
 * (C)2000-2001 empeg ltd, http://www.empeg.com
 *
 * Author:
 *   Hugo Fiennes, <hugo@empeg.com>
 *
 * The power-pic is a PIC12C508A in the empeg Mk2's power supply. This chip
 * powered by the permanent 12v supply line when the unit is used in-car.
 * With only the power-pic running (and the RTC power capacitor charging), the
 * empeg-car takes around 1mA.
 *
 * In normal circumstances, the power-pic waits for the accessory line of the
 * car to go high, then turns on the power supply to the main system. When
 * accessory goes low, it's up to the main system to turn itself off.
 *
 * Note that it's actually impossible for the main system to turn itself off
 * if the accessory line is high - the power will be turned on instantly again.
 *
 * When the accessory line is low, the power-pic runs a timer which can cause
 * the main system to get powered up at preset intervals. As the pic is running
 * with its internal RC oscillator (vaguely 4Mhz) this power up timer isn't
 * very accurate, and can drift by as much as 30% in either direction. The
 * timer can be set to never wake the main system, or wake it up after 'n' 15
 * second units of time have elapsed since power off (n ranges from 1 to 253,
 * giving up to an hour of powered down time before powering up again).
 *
 * The secret to accurate wakeup alarms is that when the system wakes up, it
 * checks the real time clock, then can set another alarm before powering off
 * again - iteratively approaching the wakeup time.
 *
 * If there are no events pending when the system wakes up, it just powers
 * itself off again. Generally, this brief flash of power lasts no more than
 * 300ms (using 200mA - an average hourly drain of 16uAH).
 *
 * The power pic, being connected to the permanent 5v supply, can keep track of
 * the first power up after main power has been applied: this is useful for
 * (eg) PIN numbers which will only get requested when the unit has been
 * removed from the car and not every time ACC goes high and powers the main
 * system.
 *
 * You can read the current power states from this driver with ioctls, and also
 * from /proc/empeg_power
 *
 * When run on a mk1, this code is used to provide compatible ioctls.
 *
 * 2000/03/15 HBF First version
 * 2000/05/24 HBF Added ioctls
 * 2000/07/10 HBF Will now run on Mk1's and show levels on I/O
 * 2001/04/10 HBF Added display state so we know when it comes back on
 * 
 */

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
#include <asm/fiq.h>
#include <asm/segment.h>
#include <asm/io.h>
#include <asm/hardware.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>

/* For the userspace interface */
#include <asm/arch/empeg.h>

#include "empeg_power.h"

/* Only one power pic */
struct power_dev power_devices[1];

/* First boot state */
static int power_firstboot=0;

/* Boot time temperature */
int power_firsttemperature=0;

static struct file_operations power_fops = {
  NULL, /* power_lseek */
  NULL, /* power_read */
  NULL, /* power_write */
  NULL, /* power_readdir */
  power_poll,
  power_ioctl,
  NULL, /* power_mmap */
  power_open,
  NULL, /* power_flush */
  power_release,
};

/* Get current temperature from bootloader */
void empeg_power_settemperature(char *str, int *ints)
{
	/* Should be 1 parameter */
	if (ints!=NULL && ints[0]==1) {
		power_firsttemperature=ints[1];
	}
}

/* Actual communication routine */
static void powercontrol(int b)
{
	/* Send a command to the mk2's power control PIC */
	int bit;
	unsigned long flags;

	/* Not really valid on Mk1's or Marvin */
	if (empeg_hardwarerevision()<7) return;

	/* Need to do this with IRQs disabled to preserve timings */
	save_flags_cli(flags);

	/* Starts with line high, plus a delay to ensure the PIC has noticed */
	GPSR=EMPEG_POWERCONTROL;
	udelay(100);

	/* Send 8 bits */
	for(bit=7;bit>=0;bit--) {
		/* Set line low */
		GPCR=EMPEG_POWERCONTROL;

		/* Check the bit */
		if (b&(1<<bit)) {
			/* High - 20us of low */
			udelay(20);
		} else {
			/* Low - 6us of low - changed for rev9, why? */
			udelay(6);
		}

		/* Set line high */
		GPSR=EMPEG_POWERCONTROL;

		/* Inter-bit delay */
	        udelay(20);
	}

	/* End of transmission, line low */
	GPCR=EMPEG_POWERCONTROL;

	/* Reenable IRQs */
	restore_flags(flags);
}

extern int empeg_on_dc_power;

/* Bitset of current power state */
int getbitset(void)
{
	struct power_dev *dev=power_devices;

	/* These bits should be stable for half a second before they are
	   allowed to change */
	const int unstable_bits = EMPEG_POWER_FLAG_FAILENABLED
		| EMPEG_POWER_FLAG_EXTMUTE | EMPEG_POWER_FLAG_ACCESSORY;
	static int saved_unstable = 0;
	static unsigned long stable_time = 0;
		
	/* Any activity on these bits indicates a presence, if they go
	   low for a while then don't worry. */
	static unsigned long last_lights_activity;
	
	
	int bitset=0; // 6; // marvin hack
	unsigned int gplr=GPLR;
	
	if (empeg_hardwarerevision()<6) {
		/* Mk1 */
		//if (gplr&EMPEG_EXTPOWER)  bitset|=EMPEG_POWER_FLAG_DC;
		if (powerfail_enabled())  bitset|=EMPEG_POWER_FLAG_FAILENABLED;
		/* Accessory ON */        bitset|=EMPEG_POWER_FLAG_ACCESSORY;
		if (dev->displaystate)	  bitset|=EMPEG_POWER_FLAG_DISPLAY;
	} else {
		/* Mk2 */
		//if (gplr&EMPEG_EXTPOWER)  bitset|=EMPEG_POWER_FLAG_DC;
		if (powerfail_enabled())  bitset|=EMPEG_POWER_FLAG_FAILENABLED;
		if (gplr&EMPEG_ACCSENSE)  bitset|=EMPEG_POWER_FLAG_ACCESSORY;
		if (power_firstboot)      bitset|=EMPEG_POWER_FLAG_FIRSTBOOT;
		if (!(gplr&EMPEG_SERIALDCD)) bitset|=EMPEG_POWER_FLAG_EXTMUTE; /* Tel mute */
		if (!(gplr&EMPEG_SERIALCTS)) bitset|=EMPEG_POWER_FLAG_LIGHTS; /* Dimmer sense - inverted */
		if (dev->displaystate)	  bitset|=EMPEG_POWER_FLAG_DISPLAY;
	}
	
	if (empeg_on_dc_power)
		bitset|=EMPEG_POWER_FLAG_DC;
	if (saved_unstable == (bitset & unstable_bits)) {
		 /* It hasn't changed, so keep the timeout up to date */
		stable_time = jiffies;
	} else if ((jiffies - stable_time) > HZ/4) {
		/* It has changed but has been stable for a quarter of
                   a second so use it. */
		
		saved_unstable = bitset & unstable_bits;
	}

	if (bitset & EMPEG_POWER_FLAG_LIGHTS) {
		/* We've got some activity on the lights. */
		last_lights_activity = jiffies;
	} else if (jiffies - last_lights_activity < HZ) {
		/* We had some activity less than a quarter of a second ago. */
		bitset |= EMPEG_POWER_FLAG_LIGHTS;
	}

	return (bitset & ~unstable_bits) | saved_unstable;
}

/* Timer routine */
static void check_power(void *dev_id)
{
	struct power_dev *dev=(struct power_dev*)dev_id;
	int state=getbitset();

	/* Changed? */
	if (state!=dev->laststate) {
		/* Save new state */
		dev->newstate=state;

		/* Something has happened, wake up any waiters */
		wake_up_interruptible(&dev->wq);
	}
	
	/* Requeue ourselves */
	queue_task(&dev->poller, &tq_timer);
}

static int power_read_procmem(char *buf, char **start, off_t offset,
			      int len, int unused)
{
	int state=getbitset();
	len = 0;
	//if (state & EMPEG_POWER_FLAG_DC)
	if (empeg_on_dc_power)
		len += sprintf(buf + len, "1 (Battery Power)\n");
	else
		len += sprintf(buf + len, "0 (AC Power)\n");
	if (state & EMPEG_POWER_FLAG_FAILENABLED)
		len += sprintf(buf + len, "1 (Powerfail enabled)\n");
	else
		len += sprintf(buf + len, "0 (Powerfail disabled)\n");
	if (empeg_hardwarerevision() >= 6) {
		if (state & EMPEG_POWER_FLAG_ACCESSORY)
			len += sprintf(buf + len, "1 (Accessory line high)\n");
		else
			len += sprintf(buf + len, "0 (Accessory line low)\n");
		if (state & EMPEG_POWER_FLAG_FIRSTBOOT)
			len += sprintf(buf + len, "1 (First boot)\n");
		else
			len += sprintf(buf + len, "0 (Subsequent boot)\n");
		if (state & EMPEG_POWER_FLAG_EXTMUTE)
			len += sprintf(buf + len, "1 (External mute high)\n");
		else
			len += sprintf(buf + len, "0 (External mute low)\n");
		if (state & EMPEG_POWER_FLAG_LIGHTS)
			len += sprintf(buf + len, "1 (Lights sense high)\n");
		else
			len += sprintf(buf + len, "0 (Lights sense low)\n");
	}

	/* Both mk1 & mk2 */
	if (state & EMPEG_POWER_FLAG_DISPLAY)
		len += sprintf(buf + len, "1 (Display on)\n");
	else
		len += sprintf(buf + len, "0 (Display off)\n");

	len += sprintf(buf + len, "All flags: 0x%x\n", state);
	return len;
}

static struct proc_dir_entry power_proc_entry = {
	0,			/* inode (dynamic) */
	11, "empeg_power",  	/* length and name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	0, 			/* size */
	NULL, 			/* use default operations */
	&power_read_procmem, 	/* function used to read data */
};

/* Device initialisation */
void __init empeg_power_init(void)
{
	struct power_dev *dev=power_devices;
	unsigned long flags;
	int result;

	/* Get the device */
	result=register_chrdev(EMPEG_POWER_MAJOR,"empeg_power",&power_fops);
	if (result<0) {
		printk(KERN_WARNING "empeg power: Major number %d unavailable.\n",
		       EMPEG_POWER_MAJOR);
		return;
	}

	/* On Mk2, check for first boot flag */
	if (empeg_hardwarerevision()>=6) {
		/* Disable IRQs completely here as timing is critical */
		save_flags_cli(flags);

		/* Ask power PIC If this is our first boot */
		powercontrol(2);

		/* Wait to ensure PIC has twiddled accessory sense line
		   correctly */
		udelay(100);

		/* Read reply */
		power_firstboot=(GPLR&EMPEG_ACCSENSE)?0:1;

		/* IRQs back on */
		restore_flags(flags);
	}

	/* Initialise current state */
	dev->laststate=dev->newstate=getbitset();

	/* Initialise polling of state */
	dev->wq=NULL;
	dev->poller.sync=0;
	dev->poller.routine=check_power;
	dev->poller.data=dev;
	queue_task(&dev->poller,&tq_timer);
	
#ifdef CONFIG_PROC_FS
	proc_register(&proc_root, &power_proc_entry);
#endif
	/* Print init message */
	if (empeg_hardwarerevision()>=6) {
		printk("empeg power-pic driver initialised%s\n",power_firstboot?" (first boot)":"");
	} else {
		/* We're on a mk1 */
		printk("empeg power state driver initialised\n");
	}
}

static int power_open(struct inode *inode, struct file *flip)
{
	struct power_dev *dev=power_devices;
	
	MOD_INC_USE_COUNT;
	flip->private_data=dev;

	return 0;
}

static int power_release(struct inode *inode, struct file *flip)
{
	MOD_DEC_USE_COUNT;
	return 0;
}

static unsigned int power_poll(struct file *flip, poll_table *wait)
{
	struct power_dev *dev=(struct power_dev*)flip->private_data;

	/* Wait on queue */
	poll_wait(flip, &dev->wq, wait);

	if (dev->laststate!=dev->newstate)
		return POLLIN | POLLRDNORM;
	else
		return 0;
}

static int power_ioctl(struct inode *inode, struct file *flip,
		       unsigned int cmd, unsigned long arg)
{
	struct power_dev *dev=(struct power_dev*)flip->private_data;

	switch(cmd) {
	case EMPEG_POWER_TURNOFF: {
#ifdef CONFIG_EMPEG_STATE
	        /* Ensure state information is flushed, as we won't get a
		   powerdown interrupt */
		extern void state_cleanse(void);
		state_cleanse();
#endif
		/* Mk1? */
		if (empeg_hardwarerevision()<6) return -EINVAL;

		/* Turn off now */
		powercontrol(0);
		return 0;
	}

	case EMPEG_POWER_WAKETIME: {
		int waketime;

		/* Mk1? */
		if (empeg_hardwarerevision()<6) return -EINVAL;

		/* 0=never wakeup, 1-253=wakeup in n*15 seconds */
		get_user_ret(waketime, (int*)arg, -EFAULT);

		/* Valid time? */
		if (waketime>253) return -EINVAL;

		if (waketime==0) {
			/* Never wakeup */
			powercontrol(1);
		} else {
			/* Wake up in n*15 seconds */
			powercontrol(1+waketime);
		}

		return 0;
	}
	case EMPEG_POWER_READSTATE: {
		/* Build bitset:
		   b0 = 0 ac power, 1 dc power
		   b1 = 0 powerfail disabled, 1 powerfail enabled
		   b2 = 0 accessory low, 1 accessory high
		   b3 = 0 2nd or later boot, 1 first boot
		   b4 = 0 tel mute low, 1 tel mute high
		   b5 = 0 lights off, 1 lights on
		   b6 = 0 display off, 1 display on
		*/
		unsigned long flags;
		int returnstate;

		/* Swap over with IRQs disabled */
		save_flags_cli(flags);
		returnstate=dev->laststate=dev->newstate;
		restore_flags(flags);

		put_user_ret(returnstate, (int*)arg, -EFAULT);

		return 0;
	}

	}

	return -EINVAL;
}

extern void empeg_displaypower(int on)
{
	struct power_dev *dev=power_devices;

	if (empeg_hardwarerevision()<9) {
		/* Just twiddle appropriate line */
		if (on) GPSR=EMPEG_DISPLAYPOWER;
		else GPCR=EMPEG_DISPLAYPOWER;
	} else {
		/* Send actual command */
		powercontrol(on?3:4);
	}

	/* Record the state */
	dev->displaystate=on?1:0;
}
