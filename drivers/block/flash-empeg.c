/*
 * Flash block driver for empeg-car; rather heavily based on victor flash
 * driver, but with variable flash sector sizes (8k below 64k, 64k above).
 *
 * Tweaks by Hugo Fiennes <hugo@empeg.com>
 * Bug fixes by Mark Lord <mlord@pobox.com>
 * Copyright (C) 1999 Nicolas Pitre <nico@cam.org>
 *
 *  1999-02-21	Stephane Dalton		Added write functions
 */


#include <linux/config.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/malloc.h>

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <asm/delay.h>
#include <asm/arch/hardware.h>
#include <linux/empeg.h>

#define MAJOR_NR 60
#define DEVICE_NAME "flash"
#define DEVICE_REQUEST flash_request
#define DEVICE_NR(device) (MINOR(device))
#define DEVICE_ON(device)
#define DEVICE_OFF(device)
#define DEVICE_NO_RANDOM
#include <linux/blk.h>

/* Flash sector sizes: parameter blocks (8k) below 64k, then 64k blocks
   above */
#define FLASH_SMALLSECTSIZE 	(8*1024)
#define FLASH_SECTSIZE 		(64*1024)

/* Debug flags */
#undef FLASH_DEBUG
#undef FLASH_DEBUG_1

/* Partition definitions... */
#define FLASH_PARTITIONS	10
static int flash_length[FLASH_PARTITIONS] = { 
	0x02000, /* 0x00000 - bootloader (protected) */
	0x02000, /* 0x02000 - id bits (protected)
		    +0x00 - hardware revision
		    +0x04 - serial number
		    +0x08 - ~serial number
		    +0x0c - build time_t
		    +0x10 - unit id (16 bytes)
		    +0x20 - amount of ram (kb)
		    +0x24 - amount of flash (kb) */
	0x02000, /* 0x04000 - powerfail status page (64 blocks of 128 bytes) */
	0x02000, /* 0x06000 - unit configuration
		    +0x00 - number of drives (0/1/2) */
	0x02000, /* 0x08000 - unused */
	0x02000, /* 0x0a000 - unused */
	0x02000, /* 0x0c000 - watchdog power up code */
	0x02000, /* 0x0e000 - linux loader */
	0xa0000, /* 0x10000 - linux kernel */
	0x50000, /* 0xb0000 - linux initrd */
};
static unsigned char *flash_start[FLASH_PARTITIONS];
static int flash_blocksizes[FLASH_PARTITIONS];
static int flash_sectorsizes[FLASH_PARTITIONS];
static int flash_sizes[FLASH_PARTITIONS];

/* Type of flash */
static int flash_manufacturer,flash_product;

/* cache structure */
static struct flash_sect_cache_struct {
	enum { UNUSED, CLEAN, DIRTY } state;
	char *buf;
	int minor;
	int start;
        int size;
} flash_cache;

/*
 *  Macros to toggle WP pin for programming flash
 */

static struct semaphore flash_busy = MUTEX;
#define WP_ON()		do {down(&flash_busy); GPCR = EMPEG_FLASHWE;} while (0)
#define WP_OFF()	do {up(&flash_busy); GPSR = EMPEG_FLASHWE;} while (0)

/* Flash commands.. */
#define FlashCommandRead            0x00FF
#define FlashCommandErase           0x0020
#define FlashCommandConfirm         0x00D0
#define FlashCommandClear           0x0050
#define FlashCommandWrite           0x0040
#define FlashCommandStatus          0x0070
#define FlashCommandReadId          0x0090

/* C3 flash commands */
#define FLASH_C3                    0x88c1
#define FlashCommandUnlock1	    0x0060
#define FlashCommandUnlock2	    0x00d0
#define FlashCommandLock1	    0x0060
#define FlashCommandLock2	    0x0001

#define ERROR_VOLTAGE_RANGE  0x0008
#define ERROR_DEVICE_PROTECT 0x0002
#define ERROR_PROGRAMMING    0x0010
#define STATUS_BUSY          0x0080

#define ERASE_TIME_LIMIT	10000
#define WRITE_TIME_LIMIT	20


/*********************************************************************
 *  Function:   get_flash_type
 *  Author:     Hugo Fiennes
 *  History:    2000/05/20 -> creation
 *  Parameters: none
 *  Abstract:	Gets manufacturer & product code
 *********************************************************************/
static void get_flash_type(void)
{
	volatile unsigned short *p=(volatile unsigned short*)EMPEG_FLASHBASE;

	/* Enable writes to flash */
	WP_ON();

	/* Read the manufacturer & product */
	p[0]=FlashCommandReadId;
	flash_manufacturer=p[0];
	p[0]=FlashCommandReadId;
	flash_product=p[1];
	p[0]=FlashCommandRead;

	/* Disable writes again */
	WP_OFF();
}

/*********************************************************************
 *  Function:   full_status_check
 *  Author:     Stephane Dalton
 *  History:    1999/02/18 -> creation
 *  Parameters: in->    flash pointer
 *              out->   TRUE: status ok FALSE: problem
 *  Abstract:	
 *********************************************************************/
static inline int full_status_check( unsigned short status_reg )
{
	if( status_reg & ERROR_VOLTAGE_RANGE ) {
		printk("Flash driver: programming voltage error!\n");
		return FALSE;
	}
	if( status_reg & ERROR_DEVICE_PROTECT ) {
		printk("Flash driver: device is write protect!\n");
		return FALSE;
	}
	if( status_reg & ERROR_PROGRAMMING ) {
		printk("Flash driver: programming error!\n");
		return FALSE;
	}
	return TRUE;
}                     

/*********************************************************************
 *  Function:   erase_flash_sector
 *  Author:     Stephane Dalton
 *  History:    1999/02/18 -> creation
 *  Parameters: in->    address within the flash sector to erase
 *              out->   TRUE: sector erase FALSE otherwise
 *  Abstract:	//DO NOT REMOVE the udelay call, because we have to 
 *		//disable the interrupts to prevent further access to 
 *		//the flash during erase
 *              I've added a semaphore to control flash access,
 *              so we can sleep instead of busy waiting.  -M.Lord
 *********************************************************************/
static int erase_flash_sector(unsigned short *ptr)
{
	volatile unsigned short *flash_ptr;
	int erase_loop_ctr;
	unsigned short status;
	int rc;

	flash_ptr = ptr;

#ifdef FLASH_DEBUG
	printk("erase_flash_sector(%p)\n",ptr);
#endif
	WP_ON();

	*flash_ptr =  FlashCommandClear;
	*flash_ptr =  FlashCommandRead;

	/* Unlock if necessary */
	if (flash_product==FLASH_C3) {
		*flash_ptr =  FlashCommandUnlock1;
		*flash_ptr =  FlashCommandUnlock2;
	}

	*flash_ptr =  FlashCommandErase;
	*flash_ptr =  FlashCommandConfirm;
	*flash_ptr =  FlashCommandStatus;

	erase_loop_ctr = 0;

	while (!(*flash_ptr & STATUS_BUSY)) {
		//udelay(1000L);
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ/2);

		if(++erase_loop_ctr == ERASE_TIME_LIMIT) {
			panic("Flash seems dead... too bad!\n");
		}
		*flash_ptr =  FlashCommandStatus;
	}

	status = *flash_ptr;

	/* Lock if necessary */
	if (flash_product==FLASH_C3) {
		*flash_ptr =  FlashCommandLock1;
		*flash_ptr =  FlashCommandLock2;
	}

	*flash_ptr =  FlashCommandClear;
	*flash_ptr =  FlashCommandRead;

	rc = full_status_check(status);
	WP_OFF();

   	return rc;
}


/*********************************************************************
 *  Function:   write_flash_sector
 *  Author:     Stephane Dalton
 *  History:    1999/02/18 -> creation
 *  Parameters: in->    flash addr to write to
 *			data addr to read from
 *                      size to write
 *              out->   TRUE: sector written FALSE otherwise
 *  Abstract:   //DO NOT REMOVE the udelay call, because we have to
 *              //disable the interrupts to prevent further access to
 *              //the flash during write
 *              I've added a semaphore to control flash access,
 *              so we can sleep instead of busy waiting.  -M.Lord
 *********************************************************************/
static int write_flash_sector(unsigned short *ptr,const char* data,const int size)
{
	volatile unsigned short * flash_ptr;
	const char *data_ptr;
	int i,write_loop_ctrl;
	int rc;
	
#ifdef FLASH_DEBUG
	printk("write_flash_sector(%p,%x)\n",ptr,size);
#endif
	WP_ON();
	rc = FALSE;

	flash_ptr = ptr;

	*flash_ptr =  FlashCommandClear;
	*flash_ptr =  FlashCommandRead;

	/* Unlock if necessary */
	if (flash_product==FLASH_C3) {
		*flash_ptr =  FlashCommandUnlock1;
		*flash_ptr =  FlashCommandUnlock2;
	}

	for( i = 0, flash_ptr = ptr, data_ptr = data; i < size; 
	     i += 2, flash_ptr++, data_ptr += 2) {
		
		*flash_ptr =  	FlashCommandWrite;
		*flash_ptr = 	*(unsigned short *)data_ptr;
		*flash_ptr =  	FlashCommandStatus;
		
		write_loop_ctrl = 0;

		while (!(*flash_ptr&STATUS_BUSY)) {
			schedule();
			udelay(5L);

			if(++write_loop_ctrl==WRITE_TIME_LIMIT) {
				panic("Flash seems dead... to bad!\n");
			}
			*flash_ptr=FlashCommandStatus;
		}
		if (full_status_check(*flash_ptr)!=TRUE) {
			rc=FALSE;
			break;
		}
	}

	if(i==size) rc=TRUE;

	/* Lock if necessary */
	if (flash_product==FLASH_C3) {
		*ptr =  FlashCommandLock1;
		*ptr =  FlashCommandLock2;
	}

	*ptr=FlashCommandClear;
	*ptr=FlashCommandRead;
	WP_OFF();

	return(rc);
}


/*********************************************************************
 *  Function:   write_cached_data
 *  Author:     Stephane Dalton
 *  History:    1999/03/29 -> creation
 *  Parameters: 
 *		out ->	status code
 *  Abstract:   Write back the data cached by the driver to flash
 *********************************************************************/
static int write_cached_data(void)
{
	unsigned short *flash_ptr;

#ifdef FLASH_DEBUG
	printk( "flash: minor: %d, status: %s\n",
		flash_cache.minor,
		(flash_cache.state == DIRTY) ? "dirty" : 
		(flash_cache.state == CLEAN) ? "clean" : 
		(flash_cache.state == UNUSED) ? "unused":"" );
#endif
	if(flash_cache.state == DIRTY){
		flash_ptr = (unsigned short *)
			(flash_start[flash_cache.minor] + flash_cache.start);

		if(!erase_flash_sector(flash_ptr)) 
		    return(-EIO);
		if(!write_flash_sector(flash_ptr, flash_cache.buf,
				       flash_cache.size))
		    return(-EIO);

		flash_cache.state = CLEAN;
#ifdef FLASH_DEBUG
		printk("flash: Cached data flushed\n");
#endif
	}
	return(0);
}


/*********************************************************************
 *  Function:  	flash_cached_read 
 *  Author:     Stephane Dalton
 *  History:    1999/03/29 -> creation
 *  Parameters: 
 *		in ->	buf: 	where to put read data;
 *			minor: 	minor number aka partition we have to read from;
 *			offset:	data offset in this partition;
 *			len:	the size of the read;
 *  Abstract:   If the requested data is already in the cache,
 *		the driver read from there instead of the flash itself
 *********************************************************************/
static int flash_cached_read(	char *buf, 
				int minor,
				int offset,
				int len )
{
	while(len>0) {
		/*
		 *	Check if the requested data is already cached
		 */
		int size=flash_sectorsizes[minor]-(offset%flash_sectorsizes[minor]);
		if (size>len) size=len;

		if( (flash_cache.state == DIRTY) &&
		    (flash_cache.minor == minor) &&
		    ((offset+len) <= (flash_cache.start + flash_cache.size))
			&& (offset >= flash_cache.start) )
		{
			/*
			 *	Read the requested amount of data from our
			 *      internal cache
			 */	
			memcpy(buf, flash_cache.buf+(offset-flash_cache.start),
			       size);
#ifdef FLASH_DEBUG
			printk("flash: READ from cache\n");
#endif
		} else {
			/*
			 *	Otherwise we read the data directly from flash
			 */
			down(&flash_busy);
			memcpy( buf, flash_start[minor] + offset, size );
			up(&flash_busy);
#ifdef FLASH_DEBUG
			printk("flash: READ from flash\n");
#endif
		}

		len -= size;
		buf += size;
		offset += size;
	}
	return(0);
}


/*********************************************************************
 *  Function:   flash_cached_write
 *  Author:     Stephane Dalton
 *  History:    1999/03/29 -> creation
 *  Parameters: 
 *		in ->	buf: 	where to get the data to be written;
 *			minor: 	minor number aka partition to write to;
 *			offset: where within this partition start the writing;
 *			len:	the size of the write;
 *  Abstract:   We have to cache written date to prevent overerasing 
 *			the flash.  The typical exemple is when using a 
 *			blocksize of 4k and consecutively writing a 64k block 
 *			which would generate 16 erase/write cycles for a same 
 *			flash sector.
 *			A better solution is to cache the flash sector 
 *			currently being written.  To do so, if the sector 
 *			requested is different from the previous one, 
 *			write the cached sector and read the requested one.
 *			Then the block to write is copied in the cache buffer.
 *********************************************************************/
static int flash_cached_write(	const char *buf,
				int minor,
				int offset,
				int len )
{
	if( flash_cache.state == UNUSED ) {
		/* here we aren't in a process context -- use GFP_ATOMIC
		   priority */
		flash_cache.buf = (char *)kmalloc( FLASH_SECTSIZE, GFP_ATOMIC );
		if( flash_cache.buf == 0 ) {
			printk( "Flash driver: mem allocation error\n" );
			return -ENOMEM;
		}
		flash_cache.minor = -1;
		flash_cache.state = CLEAN;
	}

	while( len > 0 ) {
		int err;
		int size=flash_sectorsizes[minor]-(offset%flash_sectorsizes[minor]);
		if (size>len) size=len;

		if( (flash_cache.minor != minor) ||
		    ((offset+size) > (flash_cache.start+flash_cache.size)) ||
		    (offset < flash_cache.start) ) 
			{
				/*
				 * We have to write the data previously cached
				 * to the flash and read the requested sector
				 * to the cache
				 */	
				err = write_cached_data();
				if( err ) return err;
				/*
				 * Get the correct flash sector corresponding
				 * to the requested offset
				 */
				flash_cache.size=flash_sectorsizes[minor];
				flash_cache.start=(offset&~(flash_cache.size-1));
				down(&flash_busy);
				memcpy(	flash_cache.buf, 
					flash_start[minor] + flash_cache.start, 
					flash_cache.size );
				up(&flash_busy);
				flash_cache.minor = minor;
#ifdef FLASH_DEBUG
				printk("flash.start = %d, minor = %d, size = %x, flash.buf = 0x%p\n",
				       flash_cache.start,
				       flash_cache.minor,
				       flash_cache.size,
				       flash_cache.buf);
#endif
			}
		
		/*
		 *	Write the requested amount of data to our internal cache
		 */	
		memcpy( flash_cache.buf + (offset - flash_cache.start), buf, size );
		flash_cache.state = DIRTY;
		
		len -= size;
		buf += size;
		offset += size;
	}
	return(0);
}


/*********************************************************************
 *  Function:  	flash_request
 *  Author:     Nicola Pitre
 *  History:    
 *  Parameters: 
 *  Abstract:	Flash block request routine
 *********************************************************************/
static void flash_request(void)
{
	unsigned int minor;
	int offset, len;
	
	while(1) {
		INIT_REQUEST;
		
		minor = MINOR(CURRENT->rq_dev);
		if (minor >= FLASH_PARTITIONS) {
			printk( "flash: out of partition range (minor = %d)\n", minor );
			end_request(0);
			continue;
		}
		
		offset = CURRENT->sector << 9;
		len = CURRENT->current_nr_sectors << 9;
		if ((offset + len) > flash_length[minor]) {
			printk( "flash_request: access beyond end of partition\n" );
			end_request(0);
			continue;
		}
		
#ifdef FLASH_DEBUG_1
		printk( "flash_request: %s for part %d at %p, size %d\n", 
			CURRENT->cmd == READ ? "read" : 
			CURRENT->cmd == WRITE ? "write" : "unknown", 
			minor,
			flash_start[minor] + offset, len);
#endif
		switch( CURRENT->cmd ) {
		case READ:
			if( flash_cached_read(CURRENT->buffer,
					      minor,offset,len)!=0) 
				{
					end_request(0);
				} else {
					end_request(1);
				}
			break;
		
		case WRITE:
			if( flash_cached_write(CURRENT->buffer, 
					       minor,offset,len)!=0)
				{
					end_request(0);
				}else{
					end_request(1);	
				}
			break;
			
		default:
			end_request(0);
			break;
		}
	}
} 


/*********************************************************************
 *  Function:  	flash_ioctl	
 *  Author:     Nicola Pitre
 *  History:    
 *  Parameters: 
 *  Abstract:	
 *********************************************************************/
static int flash_ioctl(	struct inode *inode, 
			struct file *file, 
			unsigned int cmd, 
			unsigned long arg)
{
	switch (cmd) {
	case BLKFLSBUF:
		if (!capable(CAP_SYS_ADMIN)) return -EACCES;
		invalidate_buffers(inode->i_rdev);
		break;
		
	case BLKGETSIZE:
		/* Return device size */
		return put_user( flash_length[MINOR(inode->i_rdev)] / 512, 
				 (long *) arg );
		
	case BLKSSZGET:
		/* Block size of media (real size is 64kb but...) */
		return put_user( BLOCK_SIZE, (int *)arg );
		
	default:
		printk( "flash: unimplemented ioctl(0x%08X)\n", cmd );
		return -EINVAL;
	}
	
	return 0;
}


/*********************************************************************
 *  Function:	flash_fsync
 *  Author:     Stephane Dalton
 *  History:    
 *  Parameters: 
 *  Abstract:	
 *********************************************************************/
static int flash_fsync( struct file *file, struct dentry *dentry )
{
	int err;
	
#ifdef FLASH_DEBUG
	printk("flash_fsync() called.\n");
#endif
	
	if( (err = block_fsync( file, dentry )) == 0 )
		err = write_cached_data();
	return err;
}


/*********************************************************************
 *  Function:	flash_open	
 *  Author:     Nicola Pitre
 *  History:    
 *  Parameters: 
 *  Abstract:	
 *********************************************************************/
static int flash_open(struct inode * inode, struct file * filp)
{
	if (DEVICE_NR(inode->i_rdev) >= FLASH_PARTITIONS) return -ENXIO;
	return 0;
}


/*********************************************************************
 *  Function:	flash_release
 *  Author:     Nicola Pitre
 *  History:    
 *  Parameters: 
 *  Abstract:	
 *********************************************************************/
static int flash_release(struct inode * inode, struct file * filp)
{
#ifdef FLASH_DEBUG
	printk("flash_release() called\n");
#endif
	return( write_cached_data() );
}


/*********************************************************************
 *  Function:   victor_flash_init
 *  Author:     Nicola Pitre
 *  History:    
 *  Parameters: 
 *  Abstract:   This is the registration and initialization 
 *		function for the flash driver
 *********************************************************************/

struct file_operations flash_fops = {
	NULL,			/* lseek */
	block_read,		/* read */
	block_write,		/* write */
	NULL,			/* readdir */
	NULL,			/* poll */
	flash_ioctl,		/* ioctl */
	NULL,			/* mmap */
	flash_open,		/* open */
	NULL,			/* flush */
	flash_release,		/* close */
	flash_fsync,		/* fsync */ 
	NULL,			/* fasync */
	NULL,			/* check_media_change */
	NULL,			/* revalidate */
	NULL			/* lock */
};


int __init empeg_flash_init(void)
{
	int i,base;
	
	if (register_blkdev(MAJOR_NR, DEVICE_NAME, &flash_fops)) {
		printk("FLASHDISK: Could not get major %d", MAJOR_NR);
		return -EIO;
	}
	
	flash_cache.state = UNUSED;
	
	base=0;
	for (i = 0; i < FLASH_PARTITIONS; i++) {
		flash_start[i] = (unsigned char*)(EMPEG_FLASHBASE+base);
		flash_blocksizes[i] = BLOCK_SIZE;
		flash_sectorsizes[i] = (base<0x10000)?
			FLASH_SMALLSECTSIZE:FLASH_SECTSIZE;
		flash_sizes[i] = flash_length[i] / BLOCK_SIZE;

		/* Next minor device */
		base+=flash_length[i];
	}

	blksize_size[MAJOR_NR] = flash_blocksizes;
	blk_size[MAJOR_NR] = flash_sizes;
	blk_dev[MAJOR_NR].request_fn = flash_request;

	/* Read product ID - needed to work out if we need to do erases */
	get_flash_type();

	printk("empeg-flash driver initialized\n");
	
	return(0);
}


#ifdef MODULE

/*********************************************************************
 *  Function:   init_module
 *  Author:     Stephane Dalton
 *  History:    1999/03/29 -> creation
 *  Parameters: 
 *  Abstract:   Flash module initialisation function
 *********************************************************************/
int init_module(void)
{
	return( victor_flash_init() );
}


/*********************************************************************
 *  Function:  	cleanup_module 
 *  Author:     Stephane Dalton
 *  History:    1999/03/29 -> creation
 *  Parameters: 
 *  Abstract:   Clean the structure used by the initialisation 
 *		function and frees the memory associated with the 
 *		caching system
 *********************************************************************/
void cleanup_module(void)
{
	int i;
	
	for(i=0;i<FLASH_PARTITIONS;i++) {
		fsync_dev(MKDEV(MAJOR_NR, i));
	}
	
	unregister_blkdev(MAJOR_NR, DEVICE_NAME);
	blk_dev[MAJOR_NR].request_fn = NULL;
	blk_size[MAJOR_NR] = NULL;
	blksize_size[MAJOR_NR] = NULL;
	
	kfree(flash_cache.buf);
}

#endif
