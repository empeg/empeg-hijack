/*
 * empeg-car flash driver. Based on the ARM-Linux nwflash driver.
 *
 * 1999/03/24 MAC Initial modifications.
 *                It won't compile yet btw.
 */
 
#ifdef MODULE
#include <linux/module.h>
#include <linux/version.h>
#else
#define MOD_INC_USE_COUNT
#define MOD_DEC_USE_COUNT
#endif

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/sched.h>
#include <linux/miscdevice.h>
#include <asm/system.h>
#include <asm/spinlock.h>
#include <asm/leds.h>
#include <linux/empeg.h>

/*****************************************************************************/
#define FLASH_MINOR     160             /* MAJOR is 10 - miscdevice */
#define CMD_WRITE_DISABLE 0 
#define CMD_WRITE_ENABLE 0x28
#define CMD_WRITE_BASE64K_ENABLE 0x47

static void kick_open(void);
static int get_flash_id(void);
static int erase_block(int nBlock);		
static int write_block(unsigned long p, const char* buf, int count);		
static int open_flash(struct inode* inodep, struct file* filep);
static int release_flash(struct inode* inodep, struct file* filep);
static int flash_ioctl(struct inode *inodep, struct file* filep, unsigned int cmd, unsigned long arg);
static ssize_t flash_read(struct file * file, char * buf, size_t count, loff_t *ppos);
static ssize_t flash_write(struct file * file, const char * buf, size_t count, loff_t *ppos);
static long long flash_llseek(struct file * file, long long offset, int orig);

#define KFLASH_BASE (EMPEG_FLASHBASE)
#define KFLASH_SIZE 1024*1024	//1 Meg
#define KFLASH_ID 0x0089	//Intel flash

static int flashdebug = 0;	//if set - we will display progress msgs
static int gbWriteEnable = 0;
static int gbWriteBase64Enable = 0;
static int gbFlashSize = KFLASH_SIZE;

static struct file_operations flash_fops = {
	flash_llseek,		/* llseek              */
	flash_read,		/* read               */
	flash_write,		/* write              */
	NULL,			/* no special readdir            */
	NULL,			/* no special select             */
	flash_ioctl,
	NULL,			/* no special mmap               */
	open_flash,
	NULL,			/* no special flush */
	release_flash,
	NULL,			/* no special fsync              */
	NULL,			/* no special fasync             */
	NULL,			/* no special check_media_change */
	NULL			/* no special revaldate          */
};

static struct miscdevice flash_miscdev=
{
	FLASH_MINOR,
	"empeg_flash",
	&flash_fops
};

//the delay routine - it is often required to let the flash "breeze"...
static void flash_wait(int timeout)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule_timeout(timeout);
}

static int get_flash_id()
{
	volatile unsigned int	c1,c2;

	// try to get flash chip ID 
	kick_open();
	c2 = inb(0x80);
	*(unsigned char *)(KFLASH_BASE+0x8000) = 0x90;
	udelay(15);
	c1 = *(unsigned char *)KFLASH_BASE;
	c2 = inb(0x80);

	//on 4 Meg flash the second byte is actually at offset 2...
	if (c1 == 0xB0)
		c2 = *(unsigned char *)(KFLASH_BASE+2);
	else
		c2 = *(unsigned char *)(KFLASH_BASE+1);
		
	c2 += (c1<<8);
	// set it back to read mode 
	*(unsigned char *)(KFLASH_BASE+0x8000) = 0xFF;

	if (c2 == KFLASH_ID4)
	    gbFlashSize = KFLASH_SIZE4;
		
	return(c2);
}	
	
static int open_flash(struct inode* inodep, struct file* filep)
{
	int	id;

	id=get_flash_id();
	if ((id != KFLASH_ID) && (id != KFLASH_ID4))
	{
		printk("Flash: incorrect ID 0x%04X.\n",id);
		return -ENXIO;
	}
	MOD_INC_USE_COUNT;

	return 0;
}


static int release_flash(struct inode* inodep, struct file* filep)
{
	MOD_DEC_USE_COUNT;
	return 0;
}


static int flash_ioctl(struct inode *inodep, struct file* filep, unsigned int cmd, unsigned long arg)
{
	printk("Flash_ioctl: cmd = 0x%X.\n",cmd);

	switch (cmd)
	{
		case CMD_WRITE_DISABLE:
		    	gbWriteBase64Enable = 0;
			gbWriteEnable = 0;
			break;		

		case CMD_WRITE_ENABLE:
			gbWriteEnable = 1;
			break;

		case CMD_WRITE_BASE64K_ENABLE:
		    	gbWriteBase64Enable = 1;
			break;

		default:
		    	gbWriteBase64Enable = 0;
			gbWriteEnable = 0;
			return -EINVAL;
	}

	return 0;
}



static ssize_t flash_read(struct file * file, char * buf, size_t count, loff_t *ppos)
{
	unsigned long p = file->f_pos;
	int read;

	if (flashdebug)
	printk("Flash_dev: flash_read: offset=0x%X, buffer=0x%X, count=0x%X.\n",
		    (unsigned int)p, (unsigned int)buf, count);

	
	if (count < 0)
		return -EINVAL;

	if (count > gbFlashSize - p)
		count = gbFlashSize - p;

	if (verify_area(VERIFY_WRITE, buf, count) == -EFAULT)
		return -EFAULT;
		
	p += KFLASH_BASE;	//flash virtual address
			
	read = 0;

	if (copy_to_user(buf, (void *) p, count))
		return -EFAULT;
	read += count;
	file->f_pos += read;
	return read;
}

static ssize_t flash_write(struct file * file, const char * buf, size_t count, loff_t *ppos)
{
    unsigned long p = file->f_pos;
    int written;
    int nBlock,temp,rc;
    int	port338;
    int	i,j;

	
	if (flashdebug)
	printk("Flash_dev: flash_write: offset=0x%X, buffer=0x%X, count=0x%X.\n",
		    (unsigned int)p, (unsigned int)buf, count);
	
	if (!gbWriteEnable)
		return -EINVAL;
			
	if (p<64*1024 && (!gbWriteBase64Enable))
		return -EINVAL;
			
	if (count < 0)
		return -EINVAL;
		
	//if write size to big - error!
	if (count > gbFlashSize - p)
		return -EINVAL;

		
	if (verify_area(VERIFY_READ, buf, count) == -EFAULT)
		return -EFAULT;
		
		
	written = 0;
		
	port338 = inb(0x338);		//save port settings

			
	nBlock = (int)p>>16;		//block # of 64K bytes
	temp = ((int)(p+count)>>16)-nBlock+1; //# of 64K blocks to erase and write

	//write ends at exactly 64k boundry?
	if (((int)(p+count) & 0xFFFF)== 0)
	{
	    temp -= 1;
	
	}
			
	if (flashdebug)
	printk("FlashWrite: writing %d block(s) starting at %d.\n",temp,nBlock);	
	
	for( ;temp;temp--, nBlock++)		
	{
		if (flashdebug)
		    printk("FlashWrite: erasing block %d.\n",nBlock);
		
		//first we have to erase the block(s), where we will write...
		i = 0;
		j = 0;
RetryBlock:
		do
		{
		rc = erase_block(nBlock);
		i++;
		} while (rc && i<10);

		if (rc)
		{
		    if (flashdebug)
			printk("FlashWrite: erase error %X. Aborting...\n",rc);	
		    outb(port338, 0x338);	//restore reg on exit
		    return(written);	// bytes written
		}
		
		if (flashdebug)
		printk("FlashWrite: writing offset %X, from buf %X, bytes left %X.\n",
		    (unsigned int)p,(unsigned int)buf,count-written);

		//write_block will limit write to space left in this block
		rc = write_block(p,buf,count-written);
		j++;
				
		if (!rc)	//if somehow write verify failed? Can't happen??
		{
		    if (j < 10)	//retry up to 10 times
			goto RetryBlock;
		    else
			rc = -1;	//else quit with error...
		}
		
		if (rc < 0) 
		{
		    if (flashdebug)
			printk("FlashWrite: write error %X. Aborting...\n",rc);	
		    outb(port338, 0x338);	//restore reg on exit
		    return(written);	// bytes written
		}
		
		p += rc;		
		buf += rc;
		written += rc;
		file->f_pos += rc;

		if (flashdebug)
		    printk("FlashWrite: written 0x%X bytes OK.\n",written);	
	}		

        outb(port338, 0x338);		//restore reg on exit
	return written;
}


/*
 * The memory devices use the full 32/64 bits of the offset, and so we cannot
 * check against negative addresses: they are ok. The return value is weird,
 * though, in that case (0).
 *
 * also note that seeking relative to the "end of file" isn't supported:
 * it has no meaning, so it returns -EINVAL.
 */
static long long flash_llseek(struct file * file, long long offset, int orig)
{

	if (flashdebug)
	printk("Flash_dev: flash_lseek, offset=0x%X, orig=0x%X.\n",
	    (unsigned int)offset, (unsigned int)orig);

	switch (orig) {
		case 0:
			if (offset < 0)
			    return -EINVAL;

			if ((unsigned int)offset > gbFlashSize)
			    return -EINVAL;

			file->f_pos = (unsigned int)offset;
			return file->f_pos;
		case 1:
			if ((file->f_pos + offset) > gbFlashSize)
			    return -EINVAL;
			if ((file->f_pos + offset) < 0)
			    return -EINVAL;
			file->f_pos += offset;
			return file->f_pos;
		default:
			return -EINVAL;
	}
}


//assume that main Write routine did the parameter checking...
//so just go ahead and erase, what requested!

static int erase_block(int nBlock)
{
	volatile unsigned int	c1;
	volatile unsigned char* pWritePtr;
	int temp,temp1;

	leds_event(led_amber);			//orange LED == erase
	
	// reset footbridge to the correct offset 0 (...0..3)
	*(volatile unsigned char*)KROM_ACCESS = 0;

	c1 = *(volatile unsigned char *)(KFLASH_BASE+0x8000);	//dummy ROM read
	kick_open();
	*(volatile unsigned char*)(KFLASH_BASE+0x8000) = 0x50;	//reset status if old errors

	// erase a block...
	//aim at the middle of a current block...
	pWritePtr = (unsigned char*)((unsigned int)(KFLASH_BASE+0x8000+(nBlock<<16)));
	c1 = *pWritePtr;	//dummy read
	
	kick_open();
	*(volatile unsigned char*)pWritePtr = 0x20;	//erase
	*(volatile unsigned char*)pWritePtr = 0xD0;	//confirm

	//wait 10 ms
	flash_wait(HZ / 100);
			
	// wait while erasing in process (up to 10 sec)
	temp = jiffies+10*HZ;
	c1=0;
	while (!(c1 & 0x80) && temp>jiffies)
	{
		flash_wait(HZ / 100);
		c1 = *(volatile unsigned char*)(pWritePtr);	//read any address
//		printk("Flash_erase: status=%X.\n",c1);
	}

	//set flash for normal read access
	kick_open();
//	*(volatile unsigned char*)(KFLASH_BASE+0x8000) = 0xFF;
	*(volatile unsigned char*)pWritePtr = 0xFF; //back to normal operation

	//check if erase errors were reported
	if (c1 & 0x20)
	{
		if (flashdebug)
		printk("Flash_erase: err at %X.\n",(unsigned int)pWritePtr);
		*(volatile unsigned char*)(KFLASH_BASE+0x8000) = 0x50;	//reset error
		return(-2);
	}
			
	// just to make sure - verify if erased OK...
	flash_wait(HZ / 100);

	pWritePtr = (unsigned char*)((unsigned int)(KFLASH_BASE+(nBlock<<16)));
	for (temp=0;temp<16*1024;temp++,pWritePtr+=4)
	{
		if ((temp1 = *(volatile unsigned int*)pWritePtr) != 0xFFFFFFFF)
		{
			if (flashdebug)
			printk("Flash_erase: verify err at %X = %X.\n",
			    (unsigned int)pWritePtr,temp1);
			return(-1);
		}
	}

	return(0);		
		
}		

//write_block will limit number of bytes written to the space in this block

static int write_block(unsigned long p, const char* buf, int count)
{
	volatile unsigned int c1;
	volatile unsigned int c2;
	unsigned char* pWritePtr;
	unsigned int uAddress;
	unsigned int offset;
	unsigned int timeout;
	unsigned int timeout1;

	leds_event(led_red);			//red LED == write
	
	pWritePtr = (unsigned char*)((unsigned int)(KFLASH_BASE + p));

	offset = p & 0xFFFF; //check if write will end in this block....
	if (offset + count > 0x10000)
	{
	    count = 0x10000-offset;
	}
	
	timeout = jiffies+30*HZ;	//wait up to 30 sec for this block

	for (offset=0;offset<count;offset++,pWritePtr++)
	{
		uAddress = (unsigned int)pWritePtr;
		uAddress &= 0xFFFFFFFC;
		if (get_user(c2, buf+offset))
			return -EFAULT;

WriteRetry:		
		c1 = *(volatile unsigned char *)(KFLASH_BASE+0x8000);//dummy read
				
		//kick open the write gate 
		kick_open();
		
		// program footbridge to the correct offset...0..3
		*(volatile unsigned char*)(KROM_ACCESS) = (unsigned int)pWritePtr & 3;
		
		*(volatile unsigned char*)(uAddress) = 0x40;	//write cmd
		*(volatile unsigned char*)(uAddress) = c2;//data to write
		
		*(volatile unsigned char*)(KFLASH_BASE+0x10000) = 0x70; //get status

		c1=0;
		timeout1 = jiffies+1*HZ;	//wait up to 1 sec for this byte
		while (!(c1 & 0x80) && timeout1>jiffies)	//while not ready...
		{
			c1 = *(volatile unsigned char *)(KFLASH_BASE+0x8000);
		}
		
		if (timeout1<=jiffies)	//if timeout getting status
		{
		    kick_open();
		    *(volatile unsigned char*)(KFLASH_BASE+0x8000) = 0x50; //reset err
		    goto WriteRetry;
		}

		//switch on read access, as a default flash operation mode
		kick_open();
		*(volatile unsigned char*)(KFLASH_BASE+0x8000) = 0xFF; //read access
		
	//if hardware reports an error writing, and not timeout - 
	// reset the chip and retry
				
		if (c1 & 0x10)	//the byte write status BAD?
		{
		    kick_open();
		    *(volatile unsigned char*)(KFLASH_BASE+0x8000) = 0x50; //reset err
		    
		    if (timeout > jiffies)	//before timeout?
		    {
			if (flashdebug)
			{
			    printk("FlashWrite: Retrying write (addr=0x%X)...\n",
			        (unsigned int)pWritePtr-KFLASH_BASE);
			}	

			leds_event(led_black);	//no LED == waiting
			flash_wait(HZ / 100);	//wait couple ms		
			leds_event(led_red);	//red LED == write

			goto WriteRetry;
		    }
		    else
		    {	
			printk("Timeout in flash write! (addr=0x%X) Aborting...\n",
			    (unsigned int)pWritePtr-KFLASH_BASE);
			return(-2); //return error -2
		    }
		}
	}


	leds_event(led_green);	//green LED == read/verify

	flash_wait(HZ / 100);

	pWritePtr = (unsigned char*)((unsigned int)(KFLASH_BASE + p));
	for (offset=0;offset<count;offset++)
	{
	    char c,c1;
	    if (get_user(c, buf)) return -EFAULT;
	    buf++;
	    if ((c1=*pWritePtr++) != c)
	    {
		if (flashdebug)
		printk("flash write verify error at 0x%X! (%02X!=%02X) Retrying...\n",
		    (unsigned int)pWritePtr, c1, c);
		return(0);
	    }
	}

	return(count);
}


static void kick_open()
{
	unsigned long flags;

	//we want to write a bit pattern XXX1 to Xilinx to enable write
	//the write gate will be open for the next ~2ms.
	spin_lock_irqsave(&gpio_lock, flags);
	cpld_modify(1, 1);
	spin_unlock_irqrestore(&gpio_lock, flags);
	udelay(15);	//let the ISA bus to catch on...
}

int nwflash_init(void)
{
int id;

    id=get_flash_id();
    printk("Flash ROM driver v.%s, flash device ID 0x%04X, size %d Mb.\n",
	NWFLASH_VERSION, id, gbFlashSize/(1024*1024));

    misc_register(&flash_miscdev);

    return(0);
}

#ifdef MODULE
int init_module(void)
{
	return(nwflash_init());
}

void cleanup_module(void)
{
	misc_deregister(&flash_miscdev);
}
#endif
