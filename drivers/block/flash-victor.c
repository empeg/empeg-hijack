/*
 * Flash block driver for Victor
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


#define MAJOR_NR 60
#define DEVICE_NAME "flash"
#define DEVICE_REQUEST flash_request
#define DEVICE_NR(device) (MINOR(device))
#define DEVICE_ON(device)
#define DEVICE_OFF(device)
#define DEVICE_NO_RANDOM
#include <linux/blk.h>



/* Flash mapping start */
#define FLASH_BASE	0xd0000000


/* Flash sector size */
#define FLASH_SECTSIZE (64*1024)


/* Partition definitions... */
#define FLASH_PARTITIONS	4
static int flash_length[FLASH_PARTITIONS] = { 0x10000, 0x40000, 0x1a0000, 0x10000 };
static unsigned char *flash_start[FLASH_PARTITIONS];
static int flash_blocksizes[FLASH_PARTITIONS];
static int flash_sizes[FLASH_PARTITIONS];


/* cache structure */
static struct flash_sect_cache_struct {
	enum { UNUSED, CLEAN, DIRTY } state;
	char *buf;
	int minor;
	int start;
} flash_cache;


/*
 *  Macros to toggle WP and VPP pins for programming flash
 */
#define GPIO_FLASH_WP	(GPIO_GPIO4|GPIO_GPIO5)
#define WP_VPP_ON()	GPSR = GPIO_FLASH_WP
#define WP_VPP_OFF()	GPCR = GPIO_FLASH_WP


/* Flash commands.. */
#define FlashCommandRead            0x00FF
#define FlashCommandErase           0x0020
#define FlashCommandConfirm         0x00D0
#define FlashCommandClear           0x0050
#define FlashCommandWrite           0x0040
#define FlashCommandStatus          0x0070

#define ERROR_VOLTAGE_RANGE  0x0008
#define ERROR_DEVICE_PROTECT 0x0002
#define ERROR_PROGRAMMING    0x0010

#define ERASE_TIME_LIMIT	5000
#define WRITE_TIME_LIMIT	10



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
   if( status_reg & ERROR_VOLTAGE_RANGE ){
      printk("Flash driver: programming voltage error!\n");
      return FALSE;
   }
   if( status_reg & ERROR_DEVICE_PROTECT ){
      printk("Flash driver: device is write protect!\n");
      return FALSE;
   }
   if( status_reg & ERROR_PROGRAMMING ){
      printk("Flash driver: programming error!\n");
      return FALSE;
   }
   return TRUE;
}                     


/*********************************************************************
 *  Function:   erase_64k_flash_sector
 *  Author:     Stephane Dalton
 *  History:    1999/02/18 -> creation
 *  Parameters: in->    address within the flash sector to erase
 *              out->   TRUE: sector erase FALSE otherwise
 *  Abstract:	DO NOT REMOVE the udelay call, because we have to 
 *		disable the interrupts to prevent further access to 
 *		the flash during erase
 *********************************************************************/
static int erase_64k_flash_sector(unsigned short *ptr)
{
	volatile unsigned short *flash_ptr;
	int erase_loop_ctr;
	unsigned short status;

	flash_ptr = ptr;

	WP_VPP_ON();

	*flash_ptr =  FlashCommandClear;
	*flash_ptr =  FlashCommandRead;
	*flash_ptr =  FlashCommandErase;
	*flash_ptr =  FlashCommandConfirm;
	*flash_ptr =  FlashCommandStatus;

	erase_loop_ctr = 0;

	while(!(*flash_ptr & 0x0080 )){
		udelay(1000L);

		if(++erase_loop_ctr == ERASE_TIME_LIMIT){
			panic("Flashes seems dead... to bad!\n");
		}
		*flash_ptr =  FlashCommandStatus;
	}

	status = *flash_ptr;
	*flash_ptr =  FlashCommandClear;
	*flash_ptr =  FlashCommandRead;
	WP_VPP_OFF();

   	return(full_status_check(status));
}


/*********************************************************************
 *  Function:   write_64k_flash_sector
 *  Author:     Stephane Dalton
 *  History:    1999/02/18 -> creation
 *  Parameters: in->    flash addr to write to
 *						data addr to read from
 *              out->   TRUE: sector written FALSE otherwise
 *  Abstract:   DO NOT REMOVE the udelay call, because we have to
 *              disable the interrupts to prevent further access to
 *              the flash during write
 *********************************************************************/
static int write_64k_flash_sector(	unsigned short *ptr, 
					const char* data)
{
	volatile unsigned short * flash_ptr;
	const char *data_ptr;
	int i,write_loop_ctrl;
	int rc;

	WP_VPP_ON();
	rc = FALSE;

	flash_ptr = ptr;

	*flash_ptr =  FlashCommandClear;
	*flash_ptr =  FlashCommandRead;

	for( i = 0, flash_ptr = ptr, data_ptr = data; 
	     i < FLASH_SECTSIZE; 
	     i += 2, flash_ptr++, data_ptr += 2){

		*flash_ptr =  	FlashCommandWrite;
		*flash_ptr = 	*(unsigned short *)data_ptr;
		*flash_ptr =  	FlashCommandStatus;

		write_loop_ctrl = 0;

		while(! (*flash_ptr & 0x0080 )){
			udelay(10L);

			if( ++write_loop_ctrl == WRITE_TIME_LIMIT ){
				panic("Flashes seems dead... to bad!\n");
			}
			*flash_ptr =  FlashCommandStatus;
		}
		if(full_status_check(*flash_ptr) != TRUE){
			rc = FALSE;
			break;
		}
	}

	if( i == FLASH_SECTSIZE ){
		rc = TRUE;
	}

	*ptr =  FlashCommandClear;
	*ptr =  FlashCommandRead;
	WP_VPP_OFF();
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
		(flash_cache.state == UNUSED) ? "unused" );
#endif

	if(flash_cache.state == DIRTY){
		flash_ptr = (unsigned short *)
			(flash_start[flash_cache.minor] + flash_cache.start);

		if(!erase_64k_flash_sector(flash_ptr)) 
		    return(-EIO);
		if(!write_64k_flash_sector(flash_ptr, flash_cache.buf))
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
    while( len > 0 ){
	int size = FLASH_SECTSIZE - (offset % FLASH_SECTSIZE);
	if( size > len ) size = len;

/*
 *	Check if the requested data is already cached
 */
	if( (flash_cache.state == DIRTY) &&
	    (flash_cache.minor == minor) &&
	    ((offset+size) <= (flash_cache.start + FLASH_SECTSIZE)) &&
	    (offset >= flash_cache.start) )
	{
/*
 *	Read the requested amount of data from our internal cache
 */	
		memcpy(buf, flash_cache.buf+(offset-flash_cache.start), size);
#ifdef FLASH_DEBUG
		printk("flash: READ from cache\n");
#endif
	}
/*
 *	Otherwise we read the data directly from flash
 */	
	else{
		memcpy( buf, flash_start[minor] + offset, size );
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
    if( flash_cache.state == UNUSED ){
	/* here we aren't in a process context -- use GFP_ATOMIC priority */
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
	int size = FLASH_SECTSIZE - (offset % FLASH_SECTSIZE);
	if( size > len ) size = len;

	if( (flash_cache.minor != minor) ||
	    ((offset+size) > (flash_cache.start + FLASH_SECTSIZE)) ||
	    (offset < flash_cache.start) ) 
	{
/*
 *	We have to write the data previously cached to the flash and read 
 *	the requested sector to the cache
 */	
		err = write_cached_data();
		if( err ) return err;
/*
 *	Get the correct flash sector corresponding to the requested offset
 */
		flash_cache.start = (offset & ~(FLASH_SECTSIZE-1));
		memcpy(	flash_cache.buf, 
			flash_start[minor] + flash_cache.start, 
			FLASH_SECTSIZE );
		flash_cache.minor = minor;
#ifdef FLASH_DEBUG
		printk("flash.start = %d, minor = %d, flash.buf = 0x%08x\n",
			flash_cache.start,
			flash_cache.minor,
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

    for(;;) {
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

#ifdef FLASH_DEBUG
	printk( "flash_request: %s for part %d at %lX, size %ld from 0x%08X\n", 
		CURRENT->cmd == READ ? "read" : 
		CURRENT->cmd == WRITE ? "write" : "unknowk", 
		minor,
		flash_start[minor] + offset, len );
		CURRENT->buffer);
#endif

	switch( CURRENT->cmd ) {
	    case READ:
		if( flash_cached_read(	CURRENT->buffer,
					minor, 
					offset, 
					len) != 0) 
		{
		    end_request(0);
		}else{
		    end_request(1);
		}
		break;

	    case WRITE:
		if( flash_cached_write(	CURRENT->buffer, 
					minor,
					offset, 
					len) != 0) 
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

static struct file_operations flash_fops = {
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


int __init victor_flash_init(void)
{
    int		i;

    if (register_blkdev(MAJOR_NR, DEVICE_NAME, &flash_fops)) {
	printk("FLASHDISK: Could not get major %d", MAJOR_NR);
	return -EIO;
    }

    flash_cache.state = UNUSED;

    for (i = 0; i < FLASH_PARTITIONS; i++) {
	flash_start[i] = i ? (flash_start[i-1] + flash_length[i-1]) 
			   : (unsigned char *)FLASH_BASE;
	flash_blocksizes[i] = BLOCK_SIZE;
	flash_sizes[i] = flash_length[i] / BLOCK_SIZE;
    }

    blksize_size[MAJOR_NR] = flash_blocksizes;
    blk_size[MAJOR_NR] = flash_sizes;
    blk_dev[MAJOR_NR].request_fn = flash_request;

/*
 *	Initialize GPIO to communicate with the flash WPP
 */
	GPDR |= GPIO_FLASH_WP;
	GPCR = GPIO_FLASH_WP;
	MSC0 =  0x5389538c;			/* clock speed */

	printk("FLASH driver initialized\n" );

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
 
    for(i=0;i<FLASH_PARTITIONS;i++){
	fsync_dev(MKDEV(MAJOR_NR, i));
    }
 
    unregister_blkdev(MAJOR_NR, DEVICE_NAME);
    blk_dev[MAJOR_NR].request_fn = NULL;
    blk_size[MAJOR_NR] = NULL;
    blksize_size[MAJOR_NR] = NULL;

    kfree(flash_cache.buf);
}

#endif
