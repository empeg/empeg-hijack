/*
 *	SA1100 Watchdog driver, 
 *
 *	(c) 1999 Christophe Leroy <leroy@ensea.fr>
 *
 *      adapted from
 *
 *	Industrial Computer Source WDT500/501 driver for Linux 2.1.x
 *
 *	(c) Copyright 1996-1997 Alan Cox <alan@cymru.net>, All Rights Reserved.
 *				http://www.cymru.net
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *	
 *	Neither Alan Cox nor CymruNet Ltd. admit liability nor provide 
 *	warranty for any of this software. This material is provided 
 *	"AS-IS" and at no charge.	
 *
 *	(c) Copyright 1995    Alan Cox <alan@lxorguk.ukuu.org.uk>
 *
 *	Release 0.07.
 *
 *	Fixes
 *		Dave Gregorich	:	Modularisation and minor bugs
 *		Alan Cox	:	Added the watchdog ioctl() stuff
 *		Alan Cox	:	Fixed the reboot problem (as noted by
 *					Matt Crocker).
 *		Alan Cox	:	Added wdog= boot option
 *		Alan Cox	:	Cleaned up copy/user stuff
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/miscdevice.h>
#include <linux/watchdog.h>
#include <linux/malloc.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/init.h>
#include <asm/arch/hardware.h>
#include <asm/arch/irqs.h>

static int wdog_is_open=0;

/*
 *	You must set these - there is no sane way to probe for this board.
 *	You can use wdog=x,y to set these now.
 */
 
#define WDOG_TIMO 60
#undef TEST_WDOG

/*
 *	Setup options
 */
 
__initfunc(void wdog_setup(char *str, int *ints))
{
}
 
/*
 *	Kernel methods.
 */
 
static int wdog_status(void)
{
	/*
	 *	Status register to bit flags
	 */
	 
}

#ifdef TEST_WDOG
void wdog_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	/*
	 *	Read the status register see what is up and
	 *	then printk it.
	 */
	 
	printk(KERN_CRIT "Would Reboot.\n");
	OSSR=8;
}
#endif


static long long wdog_llseek(struct file *file, long long offset, int origin)
{
	return -ESPIPE;
}

static void wdog_ping(void)
{
	/* Write a watchdog value */
	OSMR3=OSCR+WDOG_TIMO*3686400;
}

static ssize_t wdog_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	/*  Can't seek (pwrite) on this device  */
	if (ppos != &file->f_pos)
		return -ESPIPE;

	if(count)
	{
		wdog_ping();
		return 1;
	}
	return 0;
}

/*
 *	Read reports the temperature in degrees Fahrenheit.
 */
 
static ssize_t wdog_read(struct file *file, char *buf, size_t count, loff_t *ptr)
{
	return -EINVAL;
}

static int wdog_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
	unsigned long arg)
{
	return -EINVAL;
}

static int wdog_open(struct inode *inode, struct file *file)
{
	switch(MINOR(inode->i_rdev))
	{
		case WATCHDOG_MINOR:
			if(wdog_is_open)
				return -EBUSY;
			MOD_INC_USE_COUNT;
			/*
			 *	Activate 
			 */
	 
			wdog_ping();
#ifndef TEST_WDOG
			OWER|=1;
#endif
			OIER|=8;
			return 0;
		default:
			return -ENODEV;
	}
}

static int wdog_release(struct inode *inode, struct file *file)
{
	if(MINOR(inode->i_rdev)==WATCHDOG_MINOR)
	{
#ifndef CONFIG_WATCHDOG_NOWAYOUT
		OIER&=~8;
#endif
		wdog_is_open=0;
	}
	MOD_DEC_USE_COUNT;
	return 0;
}

/*
 *	Kernel Interfaces
 */

static int wdog_notify_sys(struct notifier_block *this, unsigned long code,
         void *unused)
{
	if(code==SYS_DOWN || code==SYS_HALT)
	{
		OIER&=~8;
	}
	return NOTIFY_DONE;                                                     }
												          
 
 
static struct file_operations wdog_fops = {
	wdog_llseek,
	NULL,
	wdog_write,
	NULL,		/* No Readdir */
	NULL,		/* No Select */
	wdog_ioctl,
	NULL,		/* No mmap */
	wdog_open,
	NULL,		/* flush */
	wdog_release
};

static struct miscdevice wdog_miscdev=
{
	WATCHDOG_MINOR,
	"watchdog",
	&wdog_fops
};

/*
 *	The WDT card needs to learn about soft shutdowns in order to
 *	turn the timebomb registers off. 
 */
 
static struct notifier_block wdog_notifier=
{
	wdog_notify_sys,
	NULL,
	0
};

#ifdef MODULE

#define sa1100wdog_init init_module

void cleanup_module(void)
{
#ifdef TEST_WDOG
	free_irq(IRQ_OST3,NULL);
#endif
	misc_deregister(&wdog_miscdev);
	unregister_reboot_notifier(&wdog_notifier);
}

#endif

__initfunc(int sa1100wdog_init(void))
{
	printk("SA1100 Watchdog driver %s\n",
#ifdef TEST_WDOG
		"(Test version)"
#else
		""
#endif
	);
#ifdef TEST_WDOG
        if(request_irq(IRQ_OST3, wdog_interrupt, SA_INTERRUPT, "wdog", NULL))
        {
                printk("IRQ %d is not free.\n", IRQ_OST3);
                return -EIO;
        }  
#endif
	misc_register(&wdog_miscdev);
	register_reboot_notifier(&wdog_notifier);
	return 0;
}

