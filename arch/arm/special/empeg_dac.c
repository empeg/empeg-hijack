/*
 * SA1100/empeg Audio Device Driver
 *
 * (C) 1999 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Hugo Fiennes, <hugo@empeg.com>
 *
 * Version 2 - this is a much, much, cut-down version of the old driver, but it's
 * more what is actually required by the empeg player software.
 * THIS CODE IS NOT FINISHED. It will not work on the empeg-car yet.
 *
 *
 * The empeg audio output device has several 'limitations' due to the hardware
 * implementation:
 *
 * - Always in stereo
 * - Always at 44.1kHz
 * - Always 16-bit little-endian signed
 *
 * Due to the high data rate these parameters dictate, this driver uses the
 * onboard SA1100 DMA engine to fill the FIFOs. As this is the first DMA
 * device on the empeg, we use DMA channel 0 for it - only a single channel
 * is needed as, although the SSP input is connected, this doesn't synchronise
 * with what we need and so we ignore the input (currently).
 *
 * The maximum DMA fill size is 8192 bytes - however, as each MPEG audio frame
 * decodes to 4608 bytes, this is what we use as it gives the neatest
 * profiling (well, I think so ;) ). We also emit the corresponding display
 * buffer at DMA time, which keeps the display locked to the visuals.
 *
 * From device initialisation onwards, we always run the DMA: this is because
 * if we stall the SSP, we get a break in the I2S WS clock which causes some
 * DACs (eg, the Crystal 4334) to go into powerdown mode, which gives around a
 * 1.5s glitch in the audio (even though the I2S glitch was much much smaller).
 * We keep track of the transitions from "good data" clocking to "zero"
 * clocking (which performs DMA from the SA's internal 'zero page' and so is
 * very bus-efficient) so we can tell if the driver has ever been starved of
 * data from userland. Annoyingly, the SSP doesn't appear to have a "transmit
 * underrun" flag which will tell you when the transmitter has been starved,
 * so short of taking timer values when you enable DMA and checking them next
 * time DMA is fed, we can't programmatically work out if the transmit has
 * been glitched.
 *
 * In theory, to get a glitch is very hard. We have to miss a buffer fill
 * interrupt for one whole buffer period (assuming that the previous interrupt
 * arrives one transfer before the current one is about to time-out) - and
 * this is 2.6ms (or so). It still seems to happen sometimes under heavy
 * IRQ/transfer load (eg, ping flooding the ethernet interface).
 *
 * Wishlist:
 * 
 * - We could do with manufacturing a tail packet of data when we transition
 *   from good data to zero clocking which gives a logarithmic falloff from
 *   the last good data sample to zero (avoids clicks at the end of tracks).
 * - Software volume control
 * - Sample rate adjustment with aliasing filters
 *
 */

#ifdef CONFIG_EMPEG_DSP
#error empeg DAC driver cannot be coexist with DSP driver
#endif

#include <linux/config.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/sched.h>
#include <linux/major.h>
#include <linux/errno.h>
#include <linux/tqueue.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/soundcard.h>
#include <asm/segment.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/arch/hardware.h>
#include <asm/arch/SA-1100.h>
#include <asm/uaccess.h>

/* For the userspace interface */
#include <linux/empeg.h>

#ifdef	CONFIG_PROC_FS
#include <linux/stat.h>
#include <linux/proc_fs.h>
#endif

/*           
 * Debugging 
 */

#define AUDIO_DEBUG			0
#define AUDIO_DEBUG_VERBOSE		0
#define AUDIO_DEBUG_STATS		1 //AUDIO_DEBUG | AUDIO_DEBUG_VERBOSE

/*           
 * Constants 
 */

/* Defaults */
#define MIXER_DEVMASK			(SOUND_MASK_VOLUME)
#define MIXER_STEREODEVS		MIXER_DEVMASK

/* Names */
#define AUDIO_NAME			"audio-empeg"
#define AUDIO_NAME_VERBOSE		"empeg dacaudio driver"
#define AUDIO_VERSION_STRING		"Revision: 1.14"

/* device numbers */
#define AUDIO_MAJOR			(EMPEG_AUDIO_MAJOR)
#define AUDIO_NMINOR			(5)
#define DSP_MINOR			(3)
#define AUDIO_MINOR			(4)
#define MIXER_MINOR			(0)
#define MIC_MINOR			(1)

/* interrupt numbers */
#define AUDIO_IRQ			IRQ_DMA0 /* DMA channel 0 IRQ */

/* Client parameters */
#define AUDIO_NOOF_BUFFERS		(8)   /* Number of audio buffers */
#define AUDIO_BUFFER_SIZE		(4608) /* Size of user buffer chunks */

/* Number of audio buffers that can be in use at any one time. This is
   two less since the inactive two are actually still being used by
   DMA while they look like being free. */
#define AUDIO_MAX_FREE_BUFFERS		(AUDIO_NOOF_BUFFERS - 2)

#if AUDIO_MAX_FREE_BUFFERS < 2
#error Insufficient buffer size.
#endif

/* Input channels */
#define INPUT_PCM (1)
#define INPUT_RADIO (0)
#define INPUT_AUX (2)

/*       
 * Types 
 */

/* statistics */
typedef struct {
	ulong samples;
	ulong interrupts;
	ulong wakeups;  
	ulong fifo_err;
	ulong buffer_hwm;
	ulong user_underruns;
	ulong irq_underruns;
} audio_stats;

typedef struct
{
	/* Buffer */
	unsigned char data[AUDIO_BUFFER_SIZE];
	
	/* Number of bytes in buffer */
	int  count;
} audio_buf;

typedef struct {
	/* Buffers */
	audio_buf *buffers;
	int used,free,head,tail;

	/* Buffer management */
	struct wait_queue *waitq;

	/* Statistics */
	audio_stats stats;

	/* Are we sending "good" data? */
	int good_data;
} audio_dev;

typedef struct
{
	int input;
	unsigned int flags;
	int volume;
	int loudness;
	int balance;
	int fade;
} mixer_dev;

static void emit_action(void *);

/* Devices in the system; just the one channel at the moment */
static audio_dev 	audio[1];
static mixer_dev	mixer_global;
static struct tq_struct emit_task =
{
    NULL,
    0,
    emit_action,
    NULL
};

#ifdef	CONFIG_PROC_FS
static struct proc_dir_entry *proc_audio;
static int audio_read_proc( char *buf, char **start, off_t offset, int length,
			    int *eof, void *private )
{
	audio_dev *dev=&audio[0];

	length=0;
	length+=sprintf(buf+length,"samples   : %ld\n",dev->stats.samples);
	length+=sprintf(buf+length,"interrupts: %ld\n",dev->stats.interrupts);
	length+=sprintf(buf+length,"wakeups   : %ld\n",dev->stats.wakeups);
	length+=sprintf(buf+length,"fifo errs : %ld\n",dev->stats.fifo_err);
	length+=sprintf(buf+length,"buffer hwm: %ld\n",dev->stats.buffer_hwm);
	length+=sprintf(buf+length,"usr undrrn: %ld\n",dev->stats.user_underruns);
	length+=sprintf(buf+length,"irq undrrn: %ld\n",dev->stats.irq_underruns);
	
	return(0);
}

#endif	/* CONFIG_PROC_FS */

//                                                
static void audio_output_disable( audio_dev *dev )
{
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_output_disable\n");
#endif
	
	/* Disable DMA */
	ClrDCSR0=DCSR_DONEA|DCSR_DONEB|DCSR_IE|DCSR_RUN;
}

/*                      
 * Interrupt processing 
 */
static void audio_interrupt( int irq, void *dev_id, struct pt_regs *regs )
{
	audio_dev *dev=&audio[0];
	int status=RdDCSR0,dofirst=-1;
	
	/* Update statistics */
#if AUDIO_DEBUG_STATS
	dev->stats.interrupts++;
#endif

	/* Work out which DMA buffer we need to attend to first */
	dofirst=( ((status&DCSR_BIU) && (status&DCSR_STRTB)) ||
		  (!(status&DCSR_BIU) && !(status&DCSR_STRTA)))?0:1;

	/* Fill the first buffer */
        if (dofirst==0) {
		ClrDCSR0=DCSR_DONEB;
		
		/* Any data to get? */
		if (dev->used==0) {
			DBSA0=(unsigned char*)_ZeroMem;
			DBTA0=AUDIO_BUFFER_SIZE;

			/* If we've underrun, take note */
			if (dev->good_data) {
				dev->good_data=0;
				dev->stats.user_underruns++;
			}
		} else {
		        DBSA0=(unsigned char*)virt_to_phys(dev->buffers[dev->tail].data);
			DBTA0=AUDIO_BUFFER_SIZE;
			if (++dev->tail==AUDIO_NOOF_BUFFERS) dev->tail=0;
			dev->used--; dev->free++;
		}
		
		if (!(status&DCSR_STRTB)) {
			/* Filling both buffers: possible IRQ underrun */
			dev->stats.irq_underruns++;

			if (dev->used==0) {
				DBSB0=(unsigned char*)_ZeroMem;
				DBTB0=AUDIO_BUFFER_SIZE;
			} else {
				DBSB0=(unsigned char*)virt_to_phys(dev->buffers[dev->tail].data);
				DBTB0=AUDIO_BUFFER_SIZE;
				if (++dev->tail==AUDIO_NOOF_BUFFERS) dev->tail=0;
				dev->used--; dev->free++;
			}
			
			/* Start both channels */
			SetDCSR0=DCSR_STRTA|DCSR_STRTB|DCSR_IE|DCSR_RUN;
		} else {
			SetDCSR0=DCSR_STRTA|DCSR_IE|DCSR_RUN;
		}
	} else {
		ClrDCSR0=DCSR_DONEA;

		/* Any data to get? */
		if (dev->used==0) {
			DBSB0=(unsigned char*)_ZeroMem;
			DBTB0=AUDIO_BUFFER_SIZE;

			/* If we've underrun, take note */
			if (dev->good_data) {
				dev->good_data=0;
				dev->stats.user_underruns++;
			}
		} else {
			DBSB0=(unsigned char*)virt_to_phys(dev->buffers[dev->tail].data);
			DBTB0=AUDIO_BUFFER_SIZE;
			if (++dev->tail==AUDIO_NOOF_BUFFERS) dev->tail=0;
			dev->used--; dev->free++;
		}
		
		if (!(status&DCSR_STRTA)) {
			/* Filling both buffers: possible IRQ underrun */
			dev->stats.irq_underruns++;

			if (dev->used==0) {
				DBSA0=(unsigned char*)_ZeroMem;
				DBTA0=AUDIO_BUFFER_SIZE;
			} else {
				DBSA0=(unsigned char*)virt_to_phys(dev->buffers[dev->tail].data);
				DBTA0=AUDIO_BUFFER_SIZE;
				if (++dev->tail==AUDIO_NOOF_BUFFERS) dev->tail=0;
				dev->used--; dev->free++;
			}

			/* Start both channels */
			SetDCSR0=DCSR_STRTA|DCSR_STRTB|DCSR_IE|DCSR_RUN;
		} else {
			SetDCSR0=DCSR_STRTB|DCSR_IE|DCSR_RUN;
		}
	}

	/* Run the audio buffer emmitted action */
	queue_task(&emit_task, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
	
	/* Wake up waiter */
	wake_up_interruptible(&dev->waitq);
}

static void emit_action(void *p)
{
#ifdef CONFIG_EMPEG_DISPLAY
  audio_emitted_action();
#endif
}

/*                 
 * File operations 
 */

static int audio_write( struct file *file, const char *buffer, size_t count, loff_t *ppos )
{
	audio_dev *dev=&audio[0];
	int total=0;
	
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_write: count=%d\n", count);
#endif

	/* Count must be a multiple of the buffer size */
	if (count%AUDIO_BUFFER_SIZE) {
	        printk("non-4608 byte write (%d)\n",count);
		return(-EINVAL);
	}

	if (count==0) printk("zero byte write\n");

	/* Any space left? (No need to disable IRQs: we're just checking for a full buffer condition) */
	if (dev->free==0) {
		if (file->f_flags&O_NONBLOCK) {
			return(-EAGAIN);
		}

		/* Next buffer full */
		interruptible_sleep_on(&dev->waitq);
	}

	/* Fill as many buffers as we can */
	while(count>0 && dev->free>0) {
		unsigned long flags;

		/* Critical sections kept as short as possible to give good latency for
		   other tasks */
		save_flags_cli(flags);
		dev->free--;
		restore_flags(flags);

		/* Copy chunk of data from user-space. We're safe updating the head
		   when not in cli() as this is the only place the head gets twiddled */
		copy_from_user(dev->buffers[dev->head++].data,buffer,AUDIO_BUFFER_SIZE);
		if (dev->head==AUDIO_NOOF_BUFFERS) dev->head=0;
		total+=AUDIO_BUFFER_SIZE;
		dev->stats.samples+=AUDIO_BUFFER_SIZE;
		count-=AUDIO_BUFFER_SIZE;
		/* Now the buffer is ready, we can tell the IRQ section there's new data */
		save_flags_cli(flags);
		dev->used++;
		restore_flags(flags);
	}

	/* Update hwm */
	if (dev->used>dev->stats.buffer_hwm) dev->stats.buffer_hwm=dev->used;

	/* We have data (houston) */
	dev->good_data=1;

	/* Write complete */
	return(total);
}

static int audio_ioctl( struct inode *inode, struct file *file, uint command, ulong arg )
{
	/* invalid command */
	return(-EINVAL);
}

/* Open the audio device */
static int audio_open( struct inode *inode, struct file *file )
{
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": audio_open\n");
#endif
        return(0);
}

/*                
 * Mixer
 *
 * This is really just a token effort, we just ignore all the calls
 * to make sure things don't complain. Ideally, we need to support
 * software volume control in here (for neatness).
 *
 */

//
static struct file_operations audio_fops =
{
   NULL,		/* no special lseek */
   NULL,		/* no special read */
   audio_write,
   NULL,		/* no special readdir */
   NULL,                /* no special poll */
   audio_ioctl,
   NULL,		/* no special mmap */
   audio_open,
   NULL,                /* no special flush */
   NULL,                /* audio_release */
   NULL,                /* audio_fsync */
   NULL,		/* no special fasync */
   NULL,		/* no special check_media_change */
   NULL,       		/* no special revalidate */
   NULL                 /* no special lock */
};

static void __init mixer_dev_init(mixer_dev *dev, int minor)
{
	dev->input = SOUND_MASK_PCM;
	dev->flags = 0;
}

static int mixer_open(struct inode *inode, struct file *file)
{
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": mixer_open\n");
#endif
	file->private_data = (void *)&mixer_global;

	return 0;
}

static int mixer_release(struct inode *inode, struct file *file)
{
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": mixer_release\n");
#endif
	
	file->private_data = NULL;
	return 0;
}

static int mixer_ioctl(struct inode *inode, struct file *file, uint command, ulong arg)
{
	mixer_dev *dev = file->private_data;
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": mixer_ioctl\n");
#endif
	switch(command)
	{
	case SOUND_MIXER_READ_DEVMASK:
		put_user_ret(MIXER_DEVMASK, (int *)arg, -EFAULT);
		return 0;
	case SOUND_MIXER_READ_STEREODEVS:
		put_user_ret(MIXER_STEREODEVS, (int *)arg, -EFAULT);
		return 0;

	case EMPEG_MIXER_READ_LOUDNESS:
	case EMPEG_MIXER_WRITE_LOUDNESS:
	case EMPEG_MIXER_READ_LOUDNESS_DB:
	case EMPEG_MIXER_WRITE_BALANCE:
	case EMPEG_MIXER_READ_BALANCE:
	case EMPEG_MIXER_READ_BALANCE_DB:
	case EMPEG_MIXER_WRITE_FADE:
	case EMPEG_MIXER_READ_FADE:
	case EMPEG_MIXER_READ_FADE_DB:
	case MIXER_WRITE(SOUND_MIXER_VOLUME):
	case MIXER_READ(SOUND_MIXER_VOLUME):
	case EMPEG_MIXER_READ_DB:
	case EMPEG_MIXER_READ_ZERO_LEVEL:
		put_user_ret(0, (int *)arg, -EFAULT);
		return 0;

	case EMPEG_MIXER_WRITE_SOURCE:
	case EMPEG_MIXER_READ_SOURCE:
		dev->input=SOUND_MASK_PCM;
		put_user_ret(dev->input, (int *)arg, -EFAULT);
		return 0;

	case EMPEG_MIXER_WRITE_FLAGS:
	{
		int flags;
		get_user_ret(flags, (int *)arg, -EFAULT);
		dev->flags = flags;
		return 0;
	}
	case EMPEG_MIXER_READ_FLAGS:
	{
		put_user_ret(dev->flags, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_SET_EQ:
	case EMPEG_MIXER_GET_EQ:
		return 0;
	default:
		return -EINVAL;
	}
}

static struct file_operations mixer_fops =
{
   NULL,		/* no special lseek */
   NULL,		/* no special read */
   NULL,		/* no special write */
   NULL,		/* no special readdir */
   NULL,                /* no special poll */
   mixer_ioctl,
   NULL,		/* no special mmap */
   mixer_open,
   NULL,                /* no special flush */
   mixer_release,
   NULL,		/* no special fsync */
   NULL,		/* no special fasync */
   NULL,		/* no special check_media_change */
   NULL,       		/* no special revalidate */
   NULL                 /* no special lock */
};

/*                
 * initialization 
 *                
 */

//                                                                      
static int generic_open_switch( struct inode *inode, struct file *file )
{
	/* select based on minor device number */
	switch( MINOR( inode->i_rdev )) {
	case AUDIO_MINOR:
	case DSP_MINOR:
		file->f_op = &audio_fops;
		break;
	case MIXER_MINOR:
		file->f_op = &mixer_fops;
		break;
	default:
		return(-ENXIO);
	}
	
	/* invoke specialized open */
	return(file->f_op->open(inode, file));
}

//                                          
static struct file_operations generic_fops =
{
   NULL,		/* no special lseek */
   NULL,		/* no special read */
   NULL,		/* no special write */
   NULL,		/* no special readdir */
   NULL,		/* no special select */
   NULL,		/* no special ioctl */
   NULL,		/* no special mmap */
   generic_open_switch,	/* switch for real open */
   NULL,		/* no special release */
   NULL,		/* no special fsync */
   NULL,		/* no special fasync */
   NULL,		/* no special check_media_change */
   NULL			/* no special revalidate */
};

int __init audio_empeg_init( void )
{
	audio_dev *dev=&audio[0];
	int a,err;
	
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_sa1100_init\n");
#endif

	/* Blank everything to start with */
	memset(dev,0,sizeof(audio_dev));
	
	/* Initialise mixer */
	mixer_dev_init(&mixer_global, MIXER_MINOR);

	/* Allocate buffers */
	if ((dev->buffers=kmalloc(sizeof(audio_buf)*AUDIO_NOOF_BUFFERS,GFP_KERNEL))==NULL) {
		/* No memory */
		printk(AUDIO_NAME ": can't get memory for buffers");
	}

	/* Clear them */
	for(a=0;a<AUDIO_NOOF_BUFFERS;a++) {
		dev->buffers[a].count=0;
	}

	/* Set up queue */
	dev->head=dev->tail=dev->used=0;
	dev->free=AUDIO_MAX_FREE_BUFFERS;

	/* Request appropriate interrupt line */
	if((err=request_irq(AUDIO_IRQ,audio_interrupt,0,AUDIO_NAME,NULL))!=0) {
		/* fail: unable to acquire interrupt */
		printk(AUDIO_NAME ": request_irq failed: %d\n", err);
		return(err);
	}

	/* Setup I2S clock on GAFR */
	GAFR|=GPIO_GPIO19;

	/* Setup SSP */
	Ser4SSCR0=0x8f; //SSCR0_DataSize(16)|SSCR0_Motorola|SSCR0_SSE;
	Ser4SSCR1=0x30; //SSCR1_ECS|SSCR1_SP;
	Ser4SSSR=SSSR_ROR; /* ...baby one more time */

	/* Start DMA: Clear bits in DCSR0 */
	ClrDCSR0=DCSR_DONEA|DCSR_DONEB|DCSR_IE|DCSR_RUN;
	
	/* Initialise DDAR0 for SSP */
	DDAR0=0x81c01be8;

	/* Start both buffers off with zeros */
	DBSA0=(unsigned char*)_ZeroMem;
	DBTA0=AUDIO_BUFFER_SIZE;
	DBSB0=(unsigned char*)_ZeroMem;
	DBTB0=AUDIO_BUFFER_SIZE;
	SetDCSR0=DCSR_STRTA|DCSR_STRTB|DCSR_IE|DCSR_RUN;

	/* Register device */
	if((err=register_chrdev(AUDIO_MAJOR,AUDIO_NAME,&generic_fops))!=0) {
		/* Log error and fail */
		printk(AUDIO_NAME ": unable to register major device %d\n", AUDIO_MAJOR);
		return(err);
	}
	
#ifdef	CONFIG_PROC_FS
	/* Register procfs devices */
	proc_audio=create_proc_entry("audio",0,0);
	if (proc_audio) proc_audio->read_proc=audio_read_proc;
#endif	/* CONFIG_PROC_FS */
	
	/* Log device registration */
	printk(AUDIO_NAME_VERBOSE " initialized\n");
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": %s\n", AUDIO_VERSION_STRING);
#endif
	/* Everything OK */
	return(0);
}
