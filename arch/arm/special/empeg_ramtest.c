/* empeg ram test haxory
 *
 * (C) 1999/2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   John Ripley <john@empeg.com>
 *
 * Disable everything and test a page of RAM extensively.
 * Satan waz ere.
 *
 * Version 0.01 20000920 john@empeg.com
 *
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/major.h>
#include <linux/errno.h>
#include <linux/malloc.h>
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

#define RAMTEST_DEBUG		0

#define RAMTEST_MAJOR		251
#define RAMTEST_NAME		"ramtest"

/* evil asm calls */
void empeg_ramtest_critical_start(void);	/* don't wipe from here... */
void empeg_ramtest_critical_end(void);		/* ... to here */
int empeg_ramtest_preserve(unsigned long safe_page,
			   unsigned long test_page, int half);
int empeg_ramtest_destructive(unsigned long safe_page, int half);

struct empeg_ramtest_dev
{
	int use_count;
	unsigned long safe_page;
};

static struct empeg_ramtest_dev ramtest_device;

int __init empeg_ramtest_init(void);
static int empeg_ramtest_open(struct inode *inode, struct file *flip);
static int empeg_ramtest_release(struct inode *inode, struct file *flip);
static int empeg_ramtest_ioctl(struct inode *inode, struct file *filp,
			       unsigned int cmd, unsigned long arg);
static int empeg_ramtest_test_page(unsigned long safe_page,
				   unsigned long test_page);
static void empeg_ramtest_fault(unsigned long page, int half, int code);

static struct file_operations empeg_ramtest_fops = {
	NULL, /* lseek */
	NULL,
	NULL,
	NULL, /* readdir */
	NULL,
	empeg_ramtest_ioctl,
	NULL, /* mmap */
	empeg_ramtest_open,
	NULL, /* flush */
	empeg_ramtest_release,
};

static unsigned long critical_start, critical_end;

int __init empeg_ramtest_init(void)
{
	int err;

	critical_start = (unsigned long) empeg_ramtest_critical_start;
	critical_end = (unsigned long) empeg_ramtest_critical_end;

	critical_start &= ~(PAGE_SIZE - 1);
	critical_end += PAGE_SIZE - 1;
	critical_end &= ~(PAGE_SIZE - 1);

	critical_start = virt_to_phys(critical_start);
	critical_end = virt_to_phys(critical_end);

	if((err = register_chrdev(RAMTEST_MAJOR,
				  RAMTEST_NAME,
				  &empeg_ramtest_fops)) != 0) {
		printk(RAMTEST_NAME ": unable to register major device %d\n",
		       RAMTEST_MAJOR);
		return err;
	}

	printk(RAMTEST_NAME ": initialised, critical at %08lx-%08lx\n",
	       critical_start, critical_end);

	return 0;
}

static int empeg_ramtest_open(struct inode *inode, struct file *filp)
{
	struct empeg_ramtest_dev *dev = &ramtest_device;

	if(dev->use_count > 0) return -EBUSY;

	dev->safe_page = get_free_page(GFP_KERNEL);
	if(!dev->safe_page)
		return -ENOMEM;

	dev->use_count++;

	/* Ho-kay */
	return 0;
}

static int empeg_ramtest_release(struct inode *inode, struct file *filp)
{
	struct empeg_ramtest_dev *dev = &ramtest_device;

	free_page(dev->safe_page);
	dev->use_count--;

	/* Yippee */
	return 0;
}

static int empeg_ramtest_ioctl(struct inode *inode, struct file *filp,
			       unsigned int cmd, unsigned long arg)
{
	int err;
	struct empeg_ramtest_dev *dev = &ramtest_device;
	unsigned long mem_size;

	switch(cmd) {
	case EMPEG_RAMTEST_TEST_PAGE: {
		struct empeg_ramtest_args_t args;
		
		if((err =
		    verify_area(VERIFY_WRITE, (void *) arg,
				sizeof(struct empeg_ramtest_args_t))) != 0)
			return -EFAULT;
		   
		/* Get page requested */
		copy_from_user(&args, (void *) arg,
			       sizeof(struct empeg_ramtest_args_t));

#if 0
		if(empeg_hardwarerevision() < 6)
		    mem_size = 8 * 1024 * 1024;
		else if(empeg_hardwarerevision() < 9)
		    mem_size = 12 * 1024 * 1024;
		else
		    mem_size = 16 * 1024 * 1024;
#else
{
		extern unsigned long memory_end;
		mem_size = memory_end & ~PAGE_OFFSET;
}
#endif

		if((args.addr & (PAGE_SIZE - 1)) ||
		   (args.addr >= mem_size))
			return -EINVAL;

		/* Test it */
		err = empeg_ramtest_test_page(dev->safe_page, args.addr);
		args.ret = (unsigned) err;

		copy_to_user((void *) arg, &args,
			     sizeof(struct empeg_ramtest_args_t));

		/* success, or error not caused by RAM */
		if(err <= 0)
			return err;

		/* RAM fault
		 * args.addr points to faulty page
		 * args.len updated
		 * args.ret contains error number
		 */
		return -EIO;
	}

	default:
#if RAMTEST_DEBUG
		printk("ramtest: bad command: %d\n", cmd);
#endif
		return -EINVAL;
	}
}

static int empeg_ramtest_test_page(unsigned long safe_page,
				   unsigned long test_page)
{
	struct empeg_ramtest_dev *dev = &ramtest_device;
	int ret = 0, half;
	unsigned long display_start, display_end;

	safe_page = virt_to_phys(safe_page);
	test_page += 0xc0000000;
	test_page = virt_to_phys(test_page);

	display_start = (unsigned long) DBAR1;
	display_start &= ~(PAGE_SIZE - 1);
	display_end = display_start + 3 * PAGE_SIZE;

	/* Just fake an OK for display memory */
	if((test_page >= display_start &&
	    test_page < display_end) ||
	   (test_page >= critical_start &&
	    test_page < critical_end))
		return 0;

#if RAMTEST_DEBUG
	printk("Testing safe_page\n");
#endif
	for(half = 0; half < 2; half++) {
#if RAMTEST_DEBUG
		printk("Testing safe_page, half %d\n", half);
#endif
		ret = empeg_ramtest_destructive(safe_page, half);

		if(ret != 0) {
#if RAMTEST_DEBUG
			printk("safe_page page failed\n");
#endif
			empeg_ramtest_fault(safe_page, half, ret);
			return (ret << 1) | half;
		}
	}
#if RAMTEST_DEBUG
	printk("Ok\n");
#endif

	for(half = 0; half < 2; half++) {
		ret = empeg_ramtest_preserve(safe_page, test_page, half);

		if(ret != 0) {
			empeg_ramtest_fault(test_page, half, ret);
			return (ret << 1) | half;
		}
	}

#if RAMTEST_DEBUG
	printk("Done\n");
#endif

	return 0;
}

static void empeg_ramtest_fault(unsigned long page, int half, int code)
{
	static const char *codes[] = {
		"Success",	/* hmm... */
		"Zero pattern failed",
		"All 1's failed",
		"0x55 bit pattern failed",
		"0xaa bit pattern failed",
		"0x5555/0xaaaa bit pattern failed",
		"0xaaaa/0x5555 bit pattern failed",
		"Rom copy test failed"
	};
	static const int ics[] = {
		1, 2, 3, 4, 19, 32
	};
	int chip;
	unsigned long max_page;

	if(empeg_hardwarerevision() < 6)
		max_page = 0xd0000000;
	else
		max_page = 0xd8000000;

	if(page < 0xc0000000 || page >= max_page ||
	   (page & 0x07c00000) != 0) {
		printk("Not a RAM address: %08lx (half %d code %d)\n",
		       page, half, code);
		return;
	}

	page -= 0xc0000000;
	chip = (page >> 27) * 2 + half;
	if(chip < 0 || chip > 5) {
		printk("Wrong ic somehow, address: %08lx (half %d code %d)\n",
		       page, half, code);
	}

	if(code < 0 || code > 7) {
		printk("RAM fault: ic %d, bad code %d\n",
		       ics[chip], code);
	}
	else {
		printk("RAM fault: ic %d: %s\n",
		       ics[chip], codes[code]);
	}
}
