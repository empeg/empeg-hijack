/*
 * SA1100/empeg Audio Device Driver
 *
 * (C) 1999/2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Hugo Fiennes, <hugo@empeg.com>
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
#include <asm/delay.h>

#ifdef	CONFIG_PROC_FS
#include <linux/stat.h>
#include <linux/proc_fs.h>
#endif

static struct tq_struct i2c_queue = {
	NULL, 0, NULL, NULL
};

#include "empeg_audio.h"

/* options */
#define OPTION_DEBUGHOOK		0

/*           
 * Debugging 
 */

#define AUDIO_DEBUG			0
#define AUDIO_DEBUG_VERBOSE		0
#define AUDIO_DEBUG_STATS		1 //AUDIO_DEBUG | AUDIO_DEBUG_VERBOSE

#define RADIO_DEBUG			0

/*           
 * Constants 
 */

/* Defaults */
#define MIXER_DEVMASK			(SOUND_MASK_VOLUME)
#define MIXER_STEREODEVS		MIXER_DEVMASK

/* Names */
#define AUDIO_NAME			"audio-empeg"
#define AUDIO_NAME_VERBOSE		"empeg dspaudio driver"
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
#define MAX_FREE_BUFFERS		(AUDIO_NOOF_BUFFERS - 2)

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

	/* beep timeout */
	struct timer_list beep_timer;

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

typedef struct
{
	u16 vat;
	u16 vga;
	int db;
} volume_entry;

typedef struct
{
	u16 Cllev;
	int db;
} loudness_entry;

typedef struct
{
	u16 ball;
	u16 balr;
	int db;
} balance_entry;

typedef struct
{
	u16 Fcof;
	u16 Rcof;
	int db;
} fade_entry;

typedef struct { int address; int data; } dsp_setup;

#define LOUDNESS_TABLE_SIZE 11
#define BALANCE_TABLE_SIZE 15
#define BALANCE_ZERO 7
#define FADE_TABLE_SIZE 15
#define FADE_ZERO 7
volume_entry volume_table[101];
loudness_entry loudness_table[LOUDNESS_TABLE_SIZE];
balance_entry balance_table[BALANCE_TABLE_SIZE];
fade_entry fade_table[FADE_TABLE_SIZE];
static unsigned long csin_table_44100[];
static unsigned long csin_table_38000[];
static unsigned long *csin_table = csin_table_44100;
static unsigned stereo_level = 0;
static struct empeg_eq_section_t eq_init[20];
static struct empeg_eq_section_t eq_current[20];
static int eq_section_order[20];
static int mixer_compression = 0;
/* setup stuff for the beep coefficients (see end of file) */
static dsp_setup beep_setup[];

/* The level that corresponds to 0dB */
#define VOLUME_ZERO_DB (90)

static void emit_action(void *);
static void mixer_select_input(int input);
static void mixer_setloudness(mixer_dev *dev, int level);
static int mixer_setvolume(mixer_dev *dev, int vol);
static inline int mixer_getdb(mixer_dev *dev);
static inline void mixer_inflict_flags(mixer_dev *dev);
static void mixer_setbalance(mixer_dev *dev, int balance);
static inline int mixer_getloudnessdb(mixer_dev *dev);
static void mixer_setbalance(mixer_dev *dev, int balance);
static void mixer_setfade(mixer_dev *dev, int fade);
static inline void mixer_mute(int on);
static void mixer_select_input(int input);
void audio_beep(audio_dev *dev, int pitch, int length, int volume);
static void audio_beep_end(unsigned long);
static void mixer_eq_reset(void);
static void mixer_eq_set(struct empeg_eq_section_t *sections);
static void mixer_eq_apply(void);
static unsigned int eq_last[40];
static unsigned int eq_reg_last = 0;
static unsigned int radio_sensitivity = 0x1048;	// for marvin, 0x1000 for <6

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
	
	return(length);
}

#endif	/* CONFIG_PROC_FS */

/* DSP operations */

static int dsp_write(unsigned short address, unsigned int data)
{
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": dsp_write %x=%x\n",address,data);
#endif
	return(i2c_write1(IICD_DSP,address,data));
}  

static int dsp_read_yram(unsigned short address, unsigned int *data)
{
	return i2c_read1(IICD_DSP, address, data);
}

static int dsp_read_xram(unsigned short address, unsigned int *data)
{
	int status;
	status = i2c_read1(IICD_DSP, address, data);
	*data &= 0x3FFFF; /* Only eighteen bits of real data */
	return status;
}

static int dsp_writemulti(dsp_setup *setup)
{
	int a;
	for(a=0;setup[a].address!=0;a++) {
		if (dsp_write(setup[a].address,setup[a].data)) {
			printk(KERN_ERR "I2C write failed (%x, %x)\n",
			       setup[a].address,setup[a].data);
			return(1);
		}
	}
	
	return(0);
}

static int dsp_patchmulti(dsp_setup *setup, int address, int new_data)
{
	int a;
	for(a=0;setup[a].address!=0;a++) {
		if (setup[a].address == address) {
			setup[a].data = new_data;
			return 0;
		}
	}	
	return 1;
}	



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

	/* Any space left? (No need to disable IRQs: we're just checking for a
	   full buffer condition) */
	/* This version doesn't have races, see p209 of Linux Device Drivers */
	if (dev->free==0) {
	    struct wait_queue wait = { current, NULL };

	    add_wait_queue(&dev->waitq, &wait);
	    current->state = TASK_INTERRUPTIBLE;
	    while (dev->free == 0) {
		schedule();
	    }
	    current->state = TASK_RUNNING;
	    remove_wait_queue(&dev->waitq, &wait);
	}

	/* Fill as many buffers as we can */
	while(count>0 && dev->free>0) {
		unsigned long flags;

		/* Critical sections kept as short as possible to give good
		   latency for other tasks */
		save_flags_cli(flags);
		dev->free--;
		restore_flags(flags);

		/* Copy chunk of data from user-space. We're safe updating the
		   head when not in cli() as this is the only place the head
		   gets twiddled */
		copy_from_user(dev->buffers[dev->head++].data,buffer,AUDIO_BUFFER_SIZE);
		if (dev->head==AUDIO_NOOF_BUFFERS) dev->head=0;
		total+=AUDIO_BUFFER_SIZE;
		dev->stats.samples+=AUDIO_BUFFER_SIZE;
		count-=AUDIO_BUFFER_SIZE;
		/* Now the buffer is ready, we can tell the IRQ section
		   there's new data */
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

/* Throw away all complete blocks waiting to go out to the DAC and return how
   many bytes that was. */
static int audio_purge(audio_dev *dev)
{
	unsigned long flags;
	int bytes;

	/* We don't want to get interrupted here */
	save_flags_cli(flags);

	/* Work out how many bytes are left to send to the audio device:
	   we only worry about full buffers */
	bytes=dev->used*AUDIO_BUFFER_SIZE;

	/* Empty buffers */
	dev->head=dev->tail=dev->used=0;
	dev->free=MAX_FREE_BUFFERS;
	
	/* Let it run again */
	restore_flags(flags);

	return bytes;
}

static int audio_ioctl( struct inode *inode, struct file *file, uint command, ulong arg )
{
	audio_dev *dev = &audio[0];

	switch (command)
	{
	case EMPEG_DSP_BEEP: {
		int pitch, length, volume;
		int *ptr = (int *)arg;
		get_user_ret(pitch, ptr, -EFAULT);
		get_user_ret(length, ptr + 1, -EFAULT);
		get_user_ret(volume, ptr + 2, -EFAULT);
		audio_beep(dev, pitch, length, volume);
		return 0;
	}

	case EMPEG_DSP_PURGE: {
		int bytes = audio_purge(dev);
		put_user_ret(bytes, (int *)arg, -EFAULT);
		return 0;		
	}
	}
	
	/* invalid command */
	return -EINVAL;
}

/* Open the audio device */
static int audio_open( struct inode *inode, struct file *file )
{
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": audio_open\n");
#endif

	mixer_eq_apply();

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
	printk(AUDIO_NAME ": mixer_ioctl %08x %08lx\n", command, arg);
#endif
	switch(command)
	{
	case SOUND_MIXER_READ_DEVMASK:
		put_user_ret(MIXER_DEVMASK, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl SOUND_MIXER_READ_DEVMASK %08lx\n", arg);
#endif	
		return 0;
	case SOUND_MIXER_READ_STEREODEVS:
		put_user_ret(MIXER_STEREODEVS, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl SOUND_MIXER_READ_STEREODEVS %08lx\n", arg);
#endif	
		return 0;

	case EMPEG_MIXER_READ_LOUDNESS:
		put_user_ret(dev->loudness * 10, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_READ_LOUDNESS %d\n",
		       dev->loudness * 10);
#endif	
		return 0;
	case EMPEG_MIXER_WRITE_LOUDNESS:
	{
		int loudness_in, loudness_out;
		get_user_ret(loudness_in, (int *)arg, -EFAULT);
		if (loudness_in/10 >= LOUDNESS_TABLE_SIZE)
			return -EINVAL;
		if (loudness_in < 0)
			return -EINVAL;
		mixer_setloudness(dev, loudness_in / 10);
		loudness_out = dev->loudness * 10;
		put_user_ret(loudness_out, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_WRITE_LOUDNESS %d == %d\n",
		       loudness_in, loudness_out);
#endif	
		return 0;
	}
	case EMPEG_MIXER_READ_LOUDNESS_DB:
	{
		int db;
		db = mixer_getloudnessdb(dev);
		put_user_ret(db, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_READ_LOUDNESS_DB == %dx\n", db);
#endif	
		return 0;
	}
	case EMPEG_MIXER_WRITE_BALANCE:
	{
		int balance_in, balance_out;
		get_user_ret(balance_in, (int *)arg, -EFAULT);
		balance_out = balance_in + BALANCE_ZERO;
		if (balance_out < 0)
			return -EINVAL;
		if (balance_out >= BALANCE_TABLE_SIZE)
			return -EINVAL;
		mixer_setbalance(dev, balance_out);
		balance_out = dev->balance - BALANCE_ZERO;
		put_user_ret(balance_out, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_WRITE_BALANCE %d == %d\n",
		       balance_in, balance_out);
#endif
		return 0;
	}
	case EMPEG_MIXER_READ_BALANCE:
	{
		put_user_ret(dev->balance - BALANCE_ZERO, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_READ_BALANCE == %d\n",
		       dev->balance - BALANCE_ZERO);
#endif	
		return 0;
	}
	case EMPEG_MIXER_READ_BALANCE_DB:
	{
		int db;
		db = balance_table[dev->balance].db;
		put_user_ret(db, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_READ_BALANCE_DB == %d\n", db);
#endif	
		return 0;
	}
	case EMPEG_MIXER_WRITE_FADE:
	{
		int fade_in, fade_out;
		get_user_ret(fade_in, (int *)arg, -EFAULT);
		fade_out = fade_in + FADE_ZERO;
		if (fade_out < 0)
			return -EINVAL;
		if (fade_out >= FADE_TABLE_SIZE)
			return -EINVAL;
		mixer_setfade(dev, fade_out);
		fade_out = dev->fade - FADE_ZERO;
		put_user_ret(fade_out, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_WRITE_FADE %d == %d\n",
		       fade_in, fade_out);
#endif	
		return 0;
	}
	case EMPEG_MIXER_READ_FADE:
		put_user_ret(dev->fade - FADE_ZERO, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_READ_FADE %d\n",
		       dev->fade - FADE_ZERO);
#endif	
		return 0;
	case EMPEG_MIXER_READ_FADE_DB:
	{
		int db;
		db = fade_table[dev->fade].db;
		put_user_ret(db, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_READ_FADE_DB == %d\n", db);
#endif	
		return 0;
	}
	case MIXER_WRITE(SOUND_MIXER_VOLUME):
	{
		int vol_in, vol_out;
		get_user_ret(vol_in, (int *)arg, -EFAULT);
		/* Equalise left and right */
		vol_out = ((vol_in & 0xFF) + ((vol_in >> 8) & 0xFF))>>1;
		if (vol_out < 0 || vol_out > 100)
			return -EINVAL;
		mixer_setvolume(dev, vol_out);
		vol_out = dev->volume;
		vol_out = (vol_out & 0xFF) + ((vol_out << 8));
		put_user_ret(vol_out, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl MIXER_WRITE(SOUND_MIXER_VOLUME) %d == %d\n",
		       vol_in, vol_out);
#endif	
		return 0;
	}
	case MIXER_READ(SOUND_MIXER_VOLUME):
	{
		int vol = dev->volume;
		vol = (vol & 0xFF) + ((vol << 8));
		put_user_ret(vol, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl MIXER_READ(SOUND_MIXER_VOLUME) == %d\n",
		       vol);
#endif	
		return 0;
	}		
	case EMPEG_MIXER_READ_DB:
	{
		int db;
		db = mixer_getdb(dev);
		put_user_ret(db, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_READ_DB == %d\n", db);
#endif	
		return 0;
	}

	case EMPEG_MIXER_READ_ZERO_LEVEL:
	{
		int level = VOLUME_ZERO_DB;
		put_user_ret(level, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_READ_ZERO_LEVEL == %d\n", level);
#endif	
		return 0;
	}

	case EMPEG_MIXER_WRITE_SOURCE:
	{
		int source;
		get_user_ret(source, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_WRITE_SOURCE %d\n", source);
#endif	
		if (source & SOUND_MASK_PCM)
		{
			mixer_select_input(INPUT_PCM);
			dev->input = SOUND_MASK_PCM;
		}
		else if (source & SOUND_MASK_RADIO)
		{
			mixer_select_input(INPUT_RADIO);
			dev->input = SOUND_MASK_RADIO;
		}
		else if (source & SOUND_MASK_LINE)
		{
		        mixer_select_input(INPUT_AUX);
			dev->input = SOUND_MASK_LINE;
		}
		put_user_ret(dev->input, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_SOURCE:
		dev->input=SOUND_MASK_PCM;
		put_user_ret(dev->input, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_READ_SOURCE == %d\n", dev->input);
#endif
		return 0;

	case EMPEG_MIXER_WRITE_FLAGS:
	{
		int flags;
		get_user_ret(flags, (int *)arg, -EFAULT);
		dev->flags = flags;
		mixer_inflict_flags(dev);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_WRITE_FLAGS %d\n", flags);
#endif	
		return 0;
	}
	case EMPEG_MIXER_READ_FLAGS:
	{
		put_user_ret(dev->flags, (int *)arg, -EFAULT);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_READ_FLAGS == %d\n", dev->flags);
#endif	
		return 0;
	}
	case EMPEG_MIXER_SET_EQ:
	{
		struct empeg_eq_section_t sections[20];
		int err;

#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_SET_EQ %08lx\n", arg);
#endif	

		if((err = verify_area(VERIFY_READ, (void *) arg, sizeof(sections))) != 0)
			return(err);

		copy_from_user((void *) sections, (const void *) arg, sizeof(sections));

		mixer_eq_set(sections);
		mixer_eq_apply();
		return 0;
	}
	case EMPEG_MIXER_GET_EQ:
	{
		int err;

		if((err = verify_area(VERIFY_WRITE, (void *) arg, sizeof(eq_current))) != 0)
			return(err);

		copy_to_user((void *) arg, (const void *) eq_current, sizeof(eq_current));
		return 0;
	}
	case EMPEG_MIXER_SET_EQ_FOUR_CHANNEL:
	{
		int four_chan;
		int err;

#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_GET_EQ %08lx\n", arg);
#endif
		
		if((err = verify_area(VERIFY_READ, (void *) arg, sizeof(int))) != 0)
			return(err);

		copy_from_user((void *) &four_chan, (const void *) arg, sizeof(int));

		eq_reg_last &= 0xefff;
		eq_reg_last |= ((four_chan & 1) ^ 1) << 12;
		if(four_chan)
			dsp_write(Y_OutSwi, 0xa85);
		else
			dsp_write(Y_OutSwi, 0xa7c);
		dsp_write(0xffd, eq_reg_last);
		return 0;
	}
	case EMPEG_MIXER_GET_EQ_FOUR_CHANNEL:
	{
		int four_chan;
		int err;

#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_GET_EQ_FOUR_CHANNEL %08lx\n", arg);
#endif

		if((err = verify_area(VERIFY_WRITE, (void *) arg, sizeof(int))) != 0)
			return(err);

		four_chan = ((eq_reg_last >> 12) & 1) ^ 1;

		copy_to_user((void *) arg, (const void *) &four_chan, sizeof(int));
		return 0;
	}
	case EMPEG_MIXER_GET_COMPRESSION:
	{
	        int err;

#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_GET_COMPRESSION %08lx\n", arg);
#endif

		if((err = verify_area(VERIFY_WRITE, (void *) arg, sizeof(int))) != 0)
			return err;
		copy_to_user((void *) arg, (const void *) &mixer_compression, sizeof(int));
		return 0;
	}
	case EMPEG_MIXER_SET_COMPRESSION:
	{
		int err, onoff;

#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_SET_COMPRESSION %08lx\n", arg);
#endif

		if((err = verify_area(VERIFY_READ, (void *) arg, sizeof(int))) != 0)
			return err;

		copy_from_user((void *) &onoff, (const void *) arg, sizeof(int));
		if(onoff)
			dsp_write(Y_compry0st_28, 0x7ab);	// turn on compression
		else
			dsp_write(Y_compry0st_28, 0x5a);	// turn off compression
		mixer_compression = onoff;
		return 0;
	}
	case EMPEG_MIXER_SET_SAM:
	{
		int err, sam;

		if((err = verify_area(VERIFY_READ, (void *) arg, sizeof(int))) != 0)
			return err;

		copy_from_user((void *) &sam, (const void *) arg, sizeof(int));

       		if(sam) dsp_write(Y_switch, 0);
		else dsp_write(Y_switch, 0x5d4);	// 4.6ms
		
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": mixer_ioctl EMPEG_MIXER_SET_SAM %d\n", sam);
#endif

		return 0;
	}
	case EMPEG_MIXER_RAW_I2C_READ:
	{
		int err, reg, val;
		if((err = verify_area(VERIFY_WRITE, (void *) arg, sizeof(int))) != 0)
			return err;
		copy_from_user((void *) &reg, (const void *) arg, sizeof(int));
		
		dsp_read_yram(reg, &val);
		copy_to_user((void *) arg, (const void *) &val, sizeof(int));
		return 0;
	}
	case EMPEG_MIXER_RAW_I2C_WRITE:
	{
		int err;
		int reg, val;
		int *block = (int *) arg;
		if((err = verify_area(VERIFY_READ, (void *) block, 2 * sizeof(int))) != 0)
			return err;
		copy_from_user((void *) &reg, (const void *) block, sizeof(int));
		copy_from_user((void *) &val, (const void *) (block+1), sizeof(int));
		
		dsp_write(reg, val);
		return 0;
	}
	case EMPEG_MIXER_WRITE_SENSITIVITY:
	{
		int err;
		int val;
		if((err = verify_area(VERIFY_READ, (void *) arg, sizeof(int))) != 0)
			return err;
		copy_from_user((void *) &val, (const void *) arg, sizeof(int));
		if(val < 0 || val > 7)
			return -EINVAL;
		radio_sensitivity = (val << 1) | (val << 4) | 0x1000;
		dsp_write(0xffd, radio_sensitivity);
		return 0;
	}
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

static void __init mixer_dev_init(mixer_dev *dev, int minor)
{
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": mixer_init\n");
#endif
	memset((void *) dev, 0, sizeof(mixer_dev));
	
	dev->input = SOUND_MASK_PCM;
	dev->flags = 0;

	/* Easy programming mode 1 (needs to be done after reset) */
	dsp_write(Y_mod00,0x4ec);

    	/* Ensure the POM is on while booting. */
	mixer_mute(1);

	/* Set volume */
	mixer_setvolume(dev,90);

	// try doing this last thing
	mixer_select_input(INPUT_PCM);

	// setup Soft Audio Mute, and enable
	dsp_write(Y_samCl, 0x189);
	dsp_write(Y_samCh, 0x7a5);
	dsp_write(Y_delta, 0x07d);
	dsp_write(Y_switch, 0x5d4);
//	dsp_write(Y_switch, 0);
	
	//mixer_select_input(INPUT_AUX);
	dsp_write(Y_SrcScal, 0x400);
	
	dev->input = SOUND_MASK_PCM;
	dev->flags = 0;
	mixer_setloudness(dev, 0);
	mixer_setbalance(dev, BALANCE_ZERO);
	mixer_setfade(dev, FADE_ZERO);
	mixer_inflict_flags(dev);

	dsp_write(Y_OutSwi, 0xa7c);

	mixer_eq_reset();	// reset coefficients
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

	if(empeg_hardwarerevision() < 6)
		radio_sensitivity = 0x1000;
	else
		radio_sensitivity = 0x1048;

	/* Blank everything to start with */
	memset(dev,0,sizeof(audio_dev));
	
	/* Allocate buffers */
	if ((dev->buffers=kmalloc(sizeof(audio_buf)*AUDIO_NOOF_BUFFERS,GFP_KERNEL))==NULL) {
		/* No memory */
		printk(AUDIO_NAME ": can't get memory for buffers");
	}

	/* Clear them */
	for(a=0;a<AUDIO_NOOF_BUFFERS;a++) {
		dev->buffers[a].count=0;
	}

	/* Set up queue: note that two buffers could be DMA'ed any any time,
	   and so we use two fewer marked as "free" */
	dev->head=dev->tail=dev->used=0;
	dev->free=MAX_FREE_BUFFERS;

	/* Request appropriate interrupt line */
	if((err=request_irq(AUDIO_IRQ,audio_interrupt,SA_INTERRUPT,AUDIO_NAME,NULL))!= 0) {
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

	/* Initialise mixer */
	mixer_dev_init(&mixer_global, MIXER_MINOR);

	/* beep timeout */
	init_timer(&dev->beep_timer);
	dev->beep_timer.data = 0;
	dev->beep_timer.function = audio_beep_end;

	/* Everything OK */
	return(0);
}

void audio_beep(audio_dev *dev, int pitch, int length, int volume)
{
	/* Section 9.8 */
	unsigned long coeff;
	int low, high, vat, i;
	unsigned int beep_start_coeffs[4];
	
#if AUDIO_DEBUG
	/* Anyone really need this debug output? */
	printk(AUDIO_NAME ": BEEP %d, %d\n", length, pitch);
#endif

	volume = (volume * 0x7ff) / 100;
	if (volume < 0) volume = 0;
	if (volume > 0x7ff) volume = 0x7ff;
	
	if ((length == 0) || (volume == 0)) {		/* Turn beep off */
		/* Remove pending timers, this doesn't handle all cases */
		if (timer_pending(&dev->beep_timer))
		    del_timer(&dev->beep_timer);
		    
		/* Turn beep off */
		dsp_write(Y_sinusMode, 0x89a);
	}
	else {				/* Turn beep on */
		if((pitch < 48) || (pitch > 96)) {
			/* Don't handle any other pitches without extending
			   the table a bit */
			return;
		}
		pitch -= 48;

		/* find value in table */
		coeff = csin_table[pitch];
		/* low/high 11 bit values */
		low = coeff & 2047;
		high = coeff >> 11;

		/* write coefficients (steal volume from another table) */
		vat = 0xfff - volume_table[mixer_global.volume].vat;

		/* write volume two at a time, slightly faster */
		beep_start_coeffs[0] = volume;	/* Y_VLsin */
		beep_start_coeffs[1] = volume;	/* Y_VRsin */
		if(i2c_write(IICD_DSP, Y_VLsin, beep_start_coeffs, 2)) {
		    printk("i2c_write for beep failed\n");
		}

		/* write pitch for first beep */
		beep_start_coeffs[0] = low;
		beep_start_coeffs[1] = high;
		if(i2c_write(IICD_DSP, Y_IcoefAl, beep_start_coeffs, 2)) {
		    printk("i2c_write for beep failed\n");
		}
		/* write pitch for second beep (unused) */
		beep_start_coeffs[0] = low;
		beep_start_coeffs[1] = high;
		if(i2c_write(IICD_DSP, Y_IcoefBL, beep_start_coeffs, 2)) {
		    printk("i2c_write for beep failed\n");
		}

		/* Coefficients for channel beep volume */
		for(i=0; i<4; i++) beep_start_coeffs[i] = vat;
		if(i2c_write(IICD_DSP, Y_tfnFL, beep_start_coeffs, 4)) {
		    printk("i2c_write for beep failed\n");
		}

		{
			int t;
			if(csin_table == csin_table_38000)
				t = (length * 19) / 2;
			else
				t = (length * 441) / 40;
			dsp_write(X_plusmax, 131071);
			dsp_write(X_minmax, 262144 - t);
			dsp_write(X_stepSize, 1);
			dsp_write(X_counterX, 262144 - t);
		}
		
		/* latch new values in synchronously */
		dsp_write(Y_iSinusWant, 0x82a);
		/* turn on the oscillator, superposition mode */
		dsp_write(Y_sinusMode, 0x88d);
		if (length > 0) {
			/* schedule a beep off */

			/* minimum duration is 30ms or you get a click */
			if (timer_pending(&dev->beep_timer))
				del_timer(&dev->beep_timer);
			/* 30ms decay */
			length += 30;
			dev->beep_timer.expires = jiffies + (length * HZ)/1000;
			add_timer(&dev->beep_timer);
		}
	}
}

static void audio_beep_end_sched(void *unused)
{
	/* This doesn't handle all cases */
	/* if another thing timed, we should keep the beep on, really */
	if(timer_pending(&audio[0].beep_timer)) return;

	/* Turn beep off */
#if AUDIO_DEBUG
	/* This all works now, I'm pretty sure */
	printk(AUDIO_NAME ": BEEP off.\n");
#endif

	/* Turn off oscillator */
	dsp_write(Y_sinusMode, 0x89a);
}

static void audio_beep_end(unsigned long unused)
{
	/* We don't want to be doing this from interrupt time */
	/* Schedule back to process time -- concurrency safe(ish) */
	i2c_queue.routine = audio_beep_end_sched;
	i2c_queue.data = NULL;
	queue_task(&i2c_queue, &tq_scheduler);
}	

/*
 * MIXER
 */

static void mixer_setloudness(mixer_dev *dev, int level)
{
#if 0
	static dsp_setup louoff[]=
	{ { Y_louSwi, 0x90d },
	  { Y_statLou, 0x7ff },
	  { 0,0 } };
	
	static dsp_setup statlou100_38[]=
	{ { Y_KLCl, 0x5e2 },
	  { Y_KLCh, 0x7d6 },
	  { Y_KLBl, 0x5b6 },
	  { Y_KLBh, 0xc28 },
	  { Y_KLA0l, 0x467 },
	  { Y_KLA0h, 0x000 },
	  { Y_KLA2l, 0x000 },
	  { Y_KLA2h, 0x000 },
	  { Y_KLmid,0x200 },
	  { Y_Cllev,0x400 },
	  { Y_Ctre, 0x0fe },
	  { Y_OFFS, 0x100 },
	  { Y_statLou, 0x511 },
	  { Y_louSwi, 0x90d },
	  { 0,0 } };
	
	static dsp_setup statlou150_41[]=
	{ { Y_KLCl, 0x62a },
	  { Y_KLCh, 0x7ca },
	  { Y_KLBl, 0x284 },
	  { Y_KLBh, 0xc34 },
	  { Y_KLA0l, 0x750 },
	  { Y_KLA0h, 0x000 },
	  { Y_KLA2l, 0x000 },
	  { Y_KLA2h, 0x000 },
	  { Y_KLmid,0x200 },
	  { Y_Cllev,0x400 },
	  { Y_Ctre, 0x000 },
	  { Y_OFFS, 0x100 },
	  { Y_statLou, 0x7ff }, /* p84, 10.5678dB boost */
	  { Y_louSwi, 0x910 },
	  { 0,0 } };
	
#if AUDIO_DEBUG
	printk("Turning loudness %s\n", on ? "on" : "off");
#endif
#endif
	static dsp_setup dynlou_41[] = {
		{ Y_KLCl, 0x347 }, /* p79, 100Hz */
		{ Y_KLCh, 0x7dc },
		{ Y_KLBl, 0x171 },
		{ Y_KLBh, 0xc23 },
		{ Y_KLA0l, 0x347 },
		{ Y_KLA0h, 0x000 },
		{ Y_KLA2l, 0x000 },
		{ Y_KLA2h, 0x000 },
		{ Y_KLmid,0x200 },
		{ Y_Cllev,0x400 },
		{ Y_Ctre, 0x000 },
		{ Y_OFFS, 0x100 },
		{ Y_statLou, 0x7ff }, /* p84, 10.5678dB boost */
		{ Y_louSwi, 0x910 },
		{ 0,0 }
	};
	       
	static dsp_setup louoff[]= {
		{ Y_louSwi, 0x90d },
		{ Y_statLou, 0x7ff },
		{ 0,0 }
	};

	if (level < 0)
		level = 0;
	if (level >= LOUDNESS_TABLE_SIZE)
		level = LOUDNESS_TABLE_SIZE - 1;
		
	if (level) {
		dsp_patchmulti(dynlou_41, Y_Cllev, loudness_table[level].Cllev);
		dsp_writemulti(dynlou_41);
	} else {
		dsp_writemulti(louoff);
	}
	dev->loudness = level;
}

static inline int mixer_getloudnessdb(mixer_dev *dev)
{
    return loudness_table[dev->loudness].db;
}

static void mixer_setbalance(mixer_dev *dev, int balance)
{
	dsp_write(Y_BALL0, balance_table[balance].ball/8);
	dsp_write(Y_BALL1, balance_table[balance].ball/8);
	dsp_write(Y_BALR0, balance_table[balance].balr/8);
	dsp_write(Y_BALR1, balance_table[balance].balr/8);
	dev->balance = balance;
}

static void mixer_setfade(mixer_dev *dev, int fade)
{
	dsp_setup bal[]	= {
		{ Y_FLcof, 0x800 },
		{ Y_FRcof, 0x800 },
		{ Y_RLcof, 0x800 },
		{ Y_RRcof, 0x800 },
		{ 0, 0 }
	};

	dsp_patchmulti(bal, Y_FLcof, fade_table[fade].Fcof);
	dsp_patchmulti(bal, Y_FRcof, fade_table[fade].Fcof);
	dsp_patchmulti(bal, Y_RLcof, fade_table[fade].Rcof);
	dsp_patchmulti(bal, Y_RRcof, fade_table[fade].Rcof);
	dsp_writemulti(bal);
	dev->fade = fade;
}

static inline void mixer_mute(int on)
{
	if (on)
		GPCR = EMPEG_DSPPOM;
	else
		GPSR = EMPEG_DSPPOM;
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": mixer_mute %s\n",on?"on":"off");
#endif
}

static void mixer_select_input(int input)
{
	static dsp_setup fm_setup[]=
	{ { 0xfff, 0x5323 },
	  { 0xffe, 0x28ed },
	  // flat eric
	  //	  { 0xffd, 0x102a }, /* 2-ch eq, 65mV tuner */
	  { 0xffd, 0x1000 }, /* 2-ch eq, 65mV tuner */	// also in case statement
	  //	  { 0xffc, 0xe086 }, /* level IAC off=e080 */
	  { 0xffc, 0x0086 }, /* level IAC off=e080 */
	  { 0xffb, 0x0aed },
	  { 0xffa, 0x1048 },
	  { 0xff9, 0x0020 }, /* no I2S out */
	  { 0xff3, 0x0000 },
	  { 0,0 } };
	
	static dsp_setup mpeg_setup[]=
	{ { 0xfff, 0xd223 },
	  { 0xffe, 0x28ed },
	  //	  { 0xffd, 0x006c },
	  { 0xffd, 0x1000 },	/* 2 ch too */		// also in case statement
	  { 0xffc, 0xe086 },
	  { 0xffb, 0x0aed },
	  { 0xffa, 0x1048 },
	  { 0xff9, 0x1240 }, /* I2S input */
	  { 0xff3, 0x0000 },
	  { 0,0 } };

	// Auxillary is on CD Analogue input
	static dsp_setup aux_setup[] =
	{ { 0xfff, 0xd223 }, /*DCS_ConTRol*/
	  { 0xffe, 0x28ed }, /*DCS_DIVide*/
	  //	  { 0xffd, 0x006c }, /*AD*/
	  { 0xffd, 0x1000 }, /*AD - 2 ch */		// also in case statement
	  { 0xffc, 0x6080 }, /*LEVEL_IAC*/
	  { 0xffb, 0x0aed }, /*IAC*/
	  { 0xffa, 0x904a }, /*SEL*/
	  { 0xff9, 0x0000 }, /*HOST*/ /* no I2S out */
	  { 0xff3, 0x0000 }, /*RDS_CONTROL*/
	  { 0,0 } };

	eq_reg_last = radio_sensitivity;
	dsp_patchmulti(fm_setup, 0xffd, eq_reg_last);
	dsp_patchmulti(mpeg_setup, 0xffd, eq_reg_last);
	dsp_patchmulti(aux_setup, 0xffd, eq_reg_last);
		
	/* Ensure any beeps playing are off, because this may block for some time */
	dsp_write(Y_sinusMode, 0x89a);
	
	/* POM low - hardware mute */ 
	mixer_mute(1);

	switch(input) {
	case INPUT_RADIO: /* FM */
	  /* FM mode: see p64 */
		dsp_writemulti(fm_setup);
		
		/* Easy programming set 2, FM */
		dsp_write(X_modpntr,0x600);
		
		/* Release POM if it wasn't already released*/
		if (!(mixer_global.flags & EMPEG_MIXER_FLAG_MUTE))
			mixer_mute(0);
		
		/* Disable DSP_IN1 mute control */
		dsp_write(Y_EMute,0x000);
		dsp_write(Y_EMuteF1,0x000);
		
		/* No FM attenuation due to low level (as FM_LEVEL is not wired up) */
		dsp_write(Y_minsmtc,0x7ff);
		dsp_write(Y_minsmtcn,0x7ff);
		
		/* Select mode */
		dsp_write(X_modpntr,0x080);
		
		// signal strength, X:levn, X:leva
		// X:levn = X:leva (unfiltered) = 4(D:level*Y:p1+Y:q1)
#if RADIO_DEBUG > 0
		// this gives X:levn = X:leva (unfiltered) = D:level
		//mk2 radio test setup
		//dsp_write(Y_p1, 2011 /* 1918 */);
		//dsp_write(Y_q1, 3380 /* 3438 */);

		// To work out required p and q values, use #define RADIO_DEBUG 1
		// use Y_p1 = 256 (0.25), Y_q1 = 0
		// record low and high levels printed:

		// l_low = (lowest reading) / 131072
		// l_high = (highest reading) / 131072

		// if l_low or l_high is too low (above 131071 -- wraparound)
		//    then you need to adjust Y_q1 above, and work out these
		//    silly equations for yourself :)
		
		// p and q can then be calculated from:

		// p = 512 / (l_high - l_low)
		// q = 4096 - l_low * p

		//    0 <= p <= 2047
		// 2048 <= q <= 4095
		// dsp_write(Y_p1, 512);
		// dsp_write(Y_q1, 0);
		dsp_write(Y_p1, 2047);
		dsp_write(Y_q1, 4030);
#else
		dsp_write(Y_p1, 2047);	// coefficients
		//		dsp_write(Y_q1, 3987);
		dsp_write(Y_q1, 4012);
#endif
		
		// multipath detection, X:mlta
		//		dsp_write(Y_c1, -974);
		dsp_write(Y_c1, -2048);
		// ok let's just turn everything off
		// disable Softmute f(level)
		dsp_write(Y_p2, 0x000);
		dsp_write(Y_q2, 0x7ff);
		// disable Softmute f(noise)
		//		dsp_write(Y_p7, 0x000);
		//		dsp_write(Y_q7, 0x7ff);
		// disable Stereo f(level)
		dsp_write(Y_p3, 0x000);
		dsp_write(Y_q3, 0x7ff);
		// disable Stereo f(noise)
		//		dsp_write(Y_E_strnf_str, 0x7ff);
		// disable Stereo f(multipath)
		dsp_write(Y_E_mltp_str, 0x7ff);
		// disable Response f(level)
		dsp_write(Y_p5, 0x000);
		dsp_write(Y_q5, 0x7ff);
		// disable Response f(noise)
		//		dsp_write(Y_E_strnf_rsp, 0x7ff);
		//		dsp_write(Y_E_mltp_rsp, 0x7ff);

		// we want the 38kHz tone table
		csin_table = csin_table_38000;

		break;    
		
	case INPUT_PCM: /* MPEG */
		/* I2S input mode: see p64 */
		dsp_writemulti(mpeg_setup);

		/* Easy programming set 4, CD */
		dsp_write(X_modpntr,0x5c0);
		
		/* Release POM */
		/* Why do we do it now? Why not wait until after we've set the
		   mode? */
		if (!(mixer_global.flags & EMPEG_MIXER_FLAG_MUTE))
			mixer_mute(0);

		/* Wait for a while so that the I2S being clocked into the
		   DSP by DMA runs the initialisation code: the DSP is cycle-
		   locked to the incoming bitstream */
		{ int a=jiffies+(HZ/20); while(jiffies<a); }
		
		/* Select mode */
		dsp_write(X_modpntr,0x0200);

		// we want the 44.1kHz tone table
		csin_table = csin_table_44100;

		break;
	case INPUT_AUX:
		/* AUX mode: see p64 */
		dsp_writemulti(aux_setup);
		
		/* Easy programming set 3, AUX - see page 68 */
		dsp_write(X_modpntr,0x5c0);
		
		/* Release POM */
		GPSR=GPIO_GPIO15;
		
		/* Disable DSP_IN1 mute control */
		dsp_write(Y_EMute,0x000);
		dsp_write(Y_EMuteF1,0x000);

		/* Select mode */
		dsp_write(X_modpntr,0x200);

		// we want the 44.1kHz tone table
		csin_table = csin_table_44100;

		break;    
	}
	
	/* Setup beep coefficients for this sampling frequency */
	if(csin_table == csin_table_38000) {
		// 6ms rise/fall time, 30ms transient
		dsp_patchmulti(beep_setup, Y_samAttl, 0x623);
		dsp_patchmulti(beep_setup, Y_samAtth, 0x7dc);
		dsp_patchmulti(beep_setup, Y_samDecl, 0x623);
		dsp_patchmulti(beep_setup, Y_samDech, 0x7ea);
		dsp_patchmulti(beep_setup, Y_deltaA, 0x10e);
		dsp_patchmulti(beep_setup, Y_switchA, 0x10e);
		dsp_patchmulti(beep_setup, Y_deltaD, 0);
		dsp_patchmulti(beep_setup, Y_switchD, 0);
	}
	else {
		// 6 ms rise/fall time, 30ms transient
		dsp_patchmulti(beep_setup, Y_samAttl, 0x45e);
		dsp_patchmulti(beep_setup, Y_samAtth, 0x7e1);
		dsp_patchmulti(beep_setup, Y_samDecl, 0x45e);
		dsp_patchmulti(beep_setup, Y_samDech, 0x7e1);
		dsp_patchmulti(beep_setup, Y_deltaA, 0x0fb);
		dsp_patchmulti(beep_setup, Y_switchA, 0x0fb);
		dsp_patchmulti(beep_setup, Y_deltaD, 0);
		dsp_patchmulti(beep_setup, Y_switchD, 0);
	}

	dsp_writemulti(beep_setup);
}

static int mixer_setvolume(mixer_dev *dev, int vol)
{
	dsp_write(Y_VAT, volume_table[vol].vat);
	dsp_write(Y_VGA, volume_table[vol].vga);
	dev->volume = vol;

#if AUDIO_DEBUG
	printk(AUDIO_NAME ": volume set %d VAT:%03x VGA:%03x\n",vol,
	       volume_table[vol].vat,volume_table[vol].vga);
#endif
	return vol;
}

static inline int mixer_getdb(mixer_dev *dev)
{
	return volume_table[dev->volume].db;
}

static inline void mixer_inflict_flags(mixer_dev *dev)
{
	unsigned int flags = dev->flags;
	mixer_mute(flags & EMPEG_MIXER_FLAG_MUTE);
}

static void mixer_eq_set(struct empeg_eq_section_t *sections)
{
	int i, order;
	
	for(i=0; i<20; i++) {
		eq_current[i].word1 = sections[i].word1;
		eq_current[i].word2 = sections[i].word2;

		order = eq_section_order[i];
		eq_last[i] = sections[order].word1;
		eq_last[i+20] = sections[order].word2;
	}
}

static void mixer_eq_reset(void)
{
	mixer_eq_set(eq_init);
}

static void mixer_eq_apply(void)
{
	//	printk("mixer_eq_set() start burst\n");
	i2c_write(IICD_DSP, 0xf80, eq_last, 40);
	//	printk("mixer_eq_set() end burst\n");
}

int audio_get_fm_level(void)
{
	unsigned level;
#if RADIO_DEBUG > 0
	static unsigned min = 131071, max = 0;
#endif

	if (dsp_read_xram(X_leva, &level) < 0) {
		return -1;
	}
#if RADIO_DEBUG > 0
	if(level < min) min = level;
	if(level > max) max = level;
	//	printk("X_leva: %6u (%6u .. %6u)\n", level, min, max);
#endif
	
	return (int) level;
}

void audio_clear_stereo(void)
{
	stereo_level = 0;
}

void audio_set_stereo(int on)
{
	if(on)
		dsp_write(Y_stro, 0x7ff);
	else
		dsp_write(Y_stro, 0);
}

int audio_get_stereo(void)
{
	int stereo;
	unsigned pltd;

	if (dsp_read_xram(X_pltd, &pltd) < 0) {
		return -1;
	}

	if(pltd > 0) pltd = 131072;

	stereo_level = (7*stereo_level + pltd)>>3;    // slow filter, call often
	//	printk("stereo_level: %d\n", stereo_level);
	stereo = (stereo_level > 0) ? 1 : 0;

	return stereo;
}

int audio_get_multipath(void)
{
	static unsigned multi = 0;
	unsigned mlta;

	if (dsp_read_xram(X_mlta, &mlta) < 0) {
		return -1;
	}
	multi = (7*multi + mlta)>>3;	// slow filter, call often
	return (int) multi;
}

volume_entry volume_table[101] = {
	{ 0x000, 0xf80, -9999 }, /* Zero */
	{ 0xfff, 0xf80, -6620 }, /* -66.2 dB intensity 0.023988 */
	{ 0xffe, 0xf80, -6270 }, /* -62.7 dB intensity 0.053703 */
	{ 0xffd, 0xf80, -5820 }, /* -58.2 dB intensity 0.151356 */
	{ 0xffc, 0xf80, -5530 }, /* -55.3 dB intensity 0.295121 */
	{ 0xffb, 0xf80, -5310 }, /* -53.1 dB intensity 0.489779 */
	{ 0xffa, 0xf80, -5140 }, /* -51.4 dB intensity 0.724436 */
	{ 0xff9, 0xf80, -4990 }, /* -49.9 dB intensity 1.023293 */
	{ 0xff8, 0xf80, -4870 }, /* -48.7 dB intensity 1.348963 */
	{ 0xff7, 0xf80, -4760 }, /* -47.6 dB intensity 1.737801 */
	{ 0xff6, 0xf80, -4660 }, /* -46.6 dB intensity 2.187762 */
	{ 0xff5, 0xf80, -4580 }, /* -45.8 dB intensity 2.630268 */
	{ 0xff4, 0xf80, -4500 }, /* -45.0 dB intensity 3.162278 */
	{ 0xff3, 0xf80, -4420 }, /* -44.2 dB intensity 3.801894 */
	{ 0xff2, 0xf80, -4360 }, /* -43.6 dB intensity 4.365158 */
	{ 0xff1, 0xf80, -4290 }, /* -42.9 dB intensity 5.128614 */
	{ 0xff0, 0xf80, -4240 }, /* -42.4 dB intensity 5.754399 */
	{ 0xfef, 0xf80, -4180 }, /* -41.8 dB intensity 6.606934 */
	{ 0xfed, 0xf80, -4080 }, /* -40.8 dB intensity 8.317638 */
	{ 0xfeb, 0xf80, -3990 }, /* -39.9 dB intensity 10.232930 */
	{ 0xfe9, 0xf80, -3910 }, /* -39.1 dB intensity 12.302688 */
	{ 0xfe7, 0xf80, -3840 }, /* -38.4 dB intensity 14.454398 */
	{ 0xfe4, 0xf80, -3740 }, /* -37.4 dB intensity 18.197009 */
	{ 0xfe1, 0xf80, -3650 }, /* -36.5 dB intensity 22.387211 */
	{ 0xfde, 0xf80, -3570 }, /* -35.7 dB intensity 26.915348 */
	{ 0xfdb, 0xf80, -3490 }, /* -34.9 dB intensity 32.359366 */
	{ 0xfd8, 0xf80, -3420 }, /* -34.2 dB intensity 38.018940 */
	{ 0xfd5, 0xf80, -3360 }, /* -33.6 dB intensity 43.651583 */
	{ 0xfd2, 0xf80, -3300 }, /* -33.0 dB intensity 50.118723 */
	{ 0xfcf, 0xf80, -3250 }, /* -32.5 dB intensity 56.234133 */
	{ 0xfcb, 0xf80, -3180 }, /* -31.8 dB intensity 66.069345 */
	{ 0xfc7, 0xf80, -3110 }, /* -31.1 dB intensity 77.624712 */
	{ 0xfc3, 0xf80, -3050 }, /* -30.5 dB intensity 89.125094 */
	{ 0xfbf, 0xf80, -3000 }, /* -30.0 dB intensity 100.000000 */
	{ 0xfbb, 0xf80, -2950 }, /* -29.5 dB intensity 112.201845 */
	{ 0xfb7, 0xf80, -2900 }, /* -29.0 dB intensity 125.892541 */
	{ 0xfb3, 0xf80, -2850 }, /* -28.5 dB intensity 141.253754 */
	{ 0xfaf, 0xf80, -2810 }, /* -28.1 dB intensity 154.881662 */
	{ 0xfab, 0xf80, -2760 }, /* -27.6 dB intensity 173.780083 */
	{ 0xfa7, 0xf80, -2720 }, /* -27.2 dB intensity 190.546072 */
	{ 0xfa2, 0xf80, -2680 }, /* -26.8 dB intensity 208.929613 */
	{ 0xf9e, 0xf80, -2640 }, /* -26.4 dB intensity 229.086765 */
	{ 0xf99, 0xf80, -2600 }, /* -26.0 dB intensity 251.188643 */
	{ 0xf93, 0xf80, -2550 }, /* -25.5 dB intensity 281.838293 */
	{ 0xf8d, 0xf80, -2500 }, /* -25.0 dB intensity 316.227766 */
	{ 0xf86, 0xf80, -2450 }, /* -24.5 dB intensity 354.813389 */
	{ 0xf7f, 0xf80, -2400 }, /* -24.0 dB intensity 398.107171 */
	{ 0xf77, 0xf80, -2350 }, /* -23.5 dB intensity 446.683592 */
	{ 0xf6f, 0xf80, -2300 }, /* -23.0 dB intensity 501.187234 */
	{ 0xf66, 0xf80, -2250 }, /* -22.5 dB intensity 562.341325 */
	{ 0xf5d, 0xf80, -2200 }, /* -22.0 dB intensity 630.957344 */
	{ 0xf54, 0xf80, -2150 }, /* -21.5 dB intensity 707.945784 */
	{ 0xf49, 0xf80, -2100 }, /* -21.0 dB intensity 794.328235 */
	{ 0xf3f, 0xf80, -2050 }, /* -20.5 dB intensity 891.250938 */
	{ 0xf33, 0xf80, -2000 }, /* -20.0 dB intensity 1000.000000 */
	{ 0xf27, 0xf80, -1950 }, /* -19.5 dB intensity 1122.018454 */
	{ 0xf1a, 0xf80, -1900 }, /* -19.0 dB intensity 1258.925412 */
	{ 0xf0d, 0xf80, -1850 }, /* -18.5 dB intensity 1412.537545 */
	{ 0xefe, 0xf80, -1800 }, /* -18.0 dB intensity 1584.893192 */
	{ 0xeef, 0xf80, -1750 }, /* -17.5 dB intensity 1778.279410 */
	{ 0xedf, 0xf80, -1700 }, /* -17.0 dB intensity 1995.262315 */
	{ 0xece, 0xf80, -1650 }, /* -16.5 dB intensity 2238.721139 */
	{ 0xebb, 0xf80, -1600 }, /* -16.0 dB intensity 2511.886432 */
	{ 0xea8, 0xf80, -1550 }, /* -15.5 dB intensity 2818.382931 */
	{ 0xe94, 0xf80, -1500 }, /* -15.0 dB intensity 3162.277660 */
	{ 0xe7e, 0xf80, -1450 }, /* -14.5 dB intensity 3548.133892 */
	{ 0xe67, 0xf80, -1400 }, /* -14.0 dB intensity 3981.071706 */
	{ 0xe4f, 0xf80, -1350 }, /* -13.5 dB intensity 4466.835922 */
	{ 0xe36, 0xf80, -1300 }, /* -13.0 dB intensity 5011.872336 */
	{ 0xe1a, 0xf80, -1250 }, /* -12.5 dB intensity 5623.413252 */
	{ 0xdfe, 0xf80, -1200 }, /* -12.0 dB intensity 6309.573445 */
	{ 0xddf, 0xf80, -1150 }, /* -11.5 dB intensity 7079.457844 */
	{ 0xdbf, 0xf80, -1100 }, /* -11.0 dB intensity 7943.282347 */
	{ 0xd9d, 0xf80, -1050 }, /* -10.5 dB intensity 8912.509381 */
	{ 0xd78, 0xf80, -1000 }, /* -10.0 dB intensity 10000.000000 */
	{ 0xd52, 0xf80, -950 }, /* -9.5 dB intensity 11220.184543 */
	{ 0xd29, 0xf80, -900 }, /* -9.0 dB intensity 12589.254118 */
	{ 0xcfe, 0xf80, -850 }, /* -8.5 dB intensity 14125.375446 */
	{ 0xcd1, 0xf80, -800 }, /* -8.0 dB intensity 15848.931925 */
	{ 0xca0, 0xf80, -750 }, /* -7.5 dB intensity 17782.794100 */
	{ 0xc6d, 0xf80, -700 }, /* -7.0 dB intensity 19952.623150 */
	{ 0xc37, 0xf80, -650 }, /* -6.5 dB intensity 22387.211386 */
	{ 0xbfe, 0xf80, -600 }, /* -6.0 dB intensity 25118.864315 */
	{ 0xbc1, 0xf80, -550 }, /* -5.5 dB intensity 28183.829313 */
	{ 0xb80, 0xf80, -500 }, /* -5.0 dB intensity 31622.776602 */
	{ 0xb3c, 0xf80, -450 }, /* -4.5 dB intensity 35481.338923 */
	{ 0xaf4, 0xf80, -400 }, /* -4.0 dB intensity 39810.717055 */
	{ 0xa56, 0xf80, -300 }, /* -3.0 dB intensity 50118.723363 */
	{ 0x9a5, 0xf80, -200 }, /* -2.0 dB intensity 63095.734448 */
	{ 0x8df, 0xf80, -100 }, /* -1.0 dB intensity 79432.823472 */
	{ 0x800, 0xf80, 0 }, /* 0.0 dB intensity 100000.000000 */
	{ 0x800, 0xf70, 100 }, /* 1.0 dB intensity 125892.541179 */
	{ 0x800, 0xf5f, 200 }, /* 2.0 dB intensity 158489.319246 */
	{ 0x800, 0xf4b, 300 }, /* 3.0 dB intensity 199526.231497 */
	{ 0x800, 0xf35, 400 }, /* 4.0 dB intensity 251188.643151 */
	{ 0x800, 0xf1c, 500 }, /* 5.0 dB intensity 316227.766017 */
	{ 0x800, 0xf01, 600 }, /* 6.0 dB intensity 398107.170554 */
	{ 0x800, 0xee1, 700 }, /* 7.0 dB intensity 501187.233627 */
	{ 0x800, 0xebe, 800 }, /* 8.0 dB intensity 630957.344480 */
	{ 0x800, 0xe97, 900 }, /* 9.0 dB intensity 794328.234724 */
	{ 0x800, 0xe6c, 1000 }, /* 10.0 dB intensity 1000000.000000 */
//	{ 0x800, 0xe6c, 1000 }, /* 10.0 dB intensity 1000000.000000 */
};

loudness_entry loudness_table[LOUDNESS_TABLE_SIZE] =
{
	{ 0x0, 0 }, /* 0.0 dB */
        { 0x30, 150 }, /* 1.5 dB */
        { 0x6a, 300 }, /* 3.0 dB */
        { 0xae, 450 }, /* 4.5 dB */
        { 0xff, 600 }, /* 6.0 dB */
        { 0x15f, 750 }, /* 7.5 dB */
        { 0x1d2, 900 }, /* 9.0 dB */
        { 0x25a, 1050 }, /* 10.5 dB */
        { 0x2fb, 1200 }, /* 12.0 dB */
        { 0x378, 1300 }, /* 13.0 dB */
        { 0x400, 1400 }, /* 14.0 dB */
};

balance_entry balance_table[BALANCE_TABLE_SIZE] =
{
	{ 0x41, 0x7ff, -3000 }, /* -30.0 dB */
        { 0x102, 0x7ff, -1800 }, /* -18.0 dB */
        { 0x1ca, 0x7ff, -1300 }, /* -13.0 dB */
        { 0x393, 0x7ff, -700 }, /* -7.0 dB */
	
        { 0x4c4, 0x7ff, -450 }, /* -4.5 dB */
        { 0x5aa, 0x7ff, -300 }, /* -3.0 dB */	
        { 0x6bb, 0x7ff, -150 }, /* -1.5 dB */
	
	{ 0x7ff, 0x7ff, 0 }, /*  0.0 dB */

        { 0x7ff, 0x6bb, 150 }, /* 1.5 dB */
        { 0x7ff, 0x5aa, 300 }, /* 3.0 dB */
        { 0x7ff, 0x4c4, 450 }, /* 4.5 dB */

        { 0x7ff, 0x393, 700 }, /* 7.0 dB */
        { 0x7ff, 0x1ca, 1300 }, /* 13.0 dB */
        { 0x7ff, 0x102, 1800 }, /* 18.0 dB */
        { 0x7ff, 0x41, 3000 }, /* 30.0 dB */
};	    

fade_entry fade_table[FADE_TABLE_SIZE] = {
        { 0xfbf, 0x800, -3000 }, /* -30.0 dB */
        { 0xefe, 0x800, -1800 }, /* -18.0 dB */
        { 0xe36, 0x800, -1300 }, /* -13.0 dB */
        { 0xc6d, 0x800, -700 }, /* -7.0 dB */

        { 0xb3c, 0x800, -450 }, /* -4.5 dB */
        { 0xa56, 0x800, -300 }, /* -3.0 dB */
        { 0x945, 0x800, -150 }, /* -1.5 dB */

        { 0x800, 0x800, 0 }, /* 0.0 dB */
	
        { 0x800, 0x945, 150 }, /* 1.5 dB */
        { 0x800, 0xa56, 300 }, /* 3.0 dB */
        { 0x800, 0xb3c, 450 }, /* 4.5 dB */

        { 0x800, 0xc6d, 700 }, /* 7.0 dB */
        { 0x800, 0xe36, 1300 }, /* 13.0 dB */
        { 0x800, 0xefe, 1800 }, /* 18.0 dB */
        { 0x800, 0xfbf, 3000 }, /* 30.0 dB */                                                  
};

static unsigned long csin_table_44100[] = {
    0x3FF7F3,    // midi note 48, piano note A 4
    0x3FF6F7,    // midi note 49, piano note A#4
    0x3FF5DC,    // midi note 50, piano note B 4
    0x3FF49E,    // midi note 51, piano note C 4
    0x3FF339,    // midi note 52, piano note C#4
    0x3FF1A9,    // midi note 53, piano note D 4
    0x3FEFE7,    // midi note 54, piano note D#4
    0x3FEDEF,    // midi note 55, piano note E 4
    0x3FEBB9,    // midi note 56, piano note F 4
    0x3FE93D,    // midi note 57, piano note F#4
    0x3FE674,    // midi note 58, piano note G 4
    0x3FE353,    // midi note 59, piano note G#4
    0x3FDFD1,    // midi note 60, piano note A 5
    0x3FDBE0,    // midi note 61, piano note A#5
    0x3FD774,    // midi note 62, piano note B 5
    0x3FD27D,    // midi note 63, piano note C 5
    0x3FCCEC,    // midi note 64, piano note C#5
    0x3FC6AB,    // midi note 65, piano note D 5
    0x3FBFA7,    // midi note 66, piano note D#5
    0x3FB7C7,    // midi note 67, piano note E 5
    0x3FAEF1,    // midi note 68, piano note F 5
    0x3FA506,    // midi note 69, piano note F#5
    0x3F99E5,    // midi note 70, piano note G 5
    0x3F8D68,    // midi note 71, piano note G#5
    0x3F7F64,    // midi note 72, piano note A 6
    0x3F6FAA,    // midi note 73, piano note A#6
    0x3F5E04,    // midi note 74, piano note B 6
    0x3F4A38,    // midi note 75, piano note C 6
    0x3F3401,    // midi note 76, piano note C#6
    0x3F1B14,    // midi note 77, piano note D 6
    0x3EFF1E,    // midi note 78, piano note D#6
    0x3EDFC1,    // midi note 79, piano note E 6
    0x3EBC92,    // midi note 80, piano note F 6
    0x3E951C,    // midi note 81, piano note F#6
    0x3E68DB,    // midi note 82, piano note G 6
    0x3E373A,    // midi note 83, piano note G#6
    0x3DFF96,    // midi note 84, piano note A 7
    0x3DC134,    // midi note 85, piano note A#7
    0x3D7B47,    // midi note 86, piano note B 7
    0x3D2CE9,    // midi note 87, piano note C 7
    0x3CD518,    // midi note 88, piano note C#7
    0x3C72B8,    // midi note 89, piano note D 7
    0x3C0489,    // midi note 90, piano note D#7
    0x3B8929,    // midi note 91, piano note E 7
    0x3AFF0F,    // midi note 92, piano note F 7
    0x3A6485,    // midi note 93, piano note F#7
    0x39B7A9,    // midi note 94, piano note G 7
    0x38F663,    // midi note 95, piano note G#7
    0x381E65,    // midi note 96, piano note A 8
};

static unsigned long csin_table_38000[] = {
    0x3FF529,    // midi note 48, piano note A 4
    0x3FF3D5,    // midi note 49, piano note A#4
    0x3FF258,    // midi note 50, piano note B 4
    0x3FF0AC,    // midi note 51, piano note C 4
    0x3FEECB,    // midi note 52, piano note C#4
    0x3FECB0,    // midi note 53, piano note D 4
    0x3FEA53,    // midi note 54, piano note D#4
    0x3FE7AB,    // midi note 55, piano note E 4
    0x3FE4B1,    // midi note 56, piano note F 4
    0x3FE159,    // midi note 57, piano note F#4
    0x3FDD99,    // midi note 58, piano note G 4
    0x3FD962,    // midi note 59, piano note G#4
    0x3FD4A8,    // midi note 60, piano note A 5
    0x3FCF5A,    // midi note 61, piano note A#5
    0x3FC966,    // midi note 62, piano note B 5
    0x3FC2B7,    // midi note 63, piano note C 5
    0x3FBB38,    // midi note 64, piano note C#5
    0x3FB2CD,    // midi note 65, piano note D 5
    0x3FA95B,    // midi note 66, piano note D#5
    0x3F9EC1,    // midi note 67, piano note E 5
    0x3F92DC,    // midi note 68, piano note F 5
    0x3F8583,    // midi note 69, piano note F#5
    0x3F7688,    // midi note 70, piano note G 5
    0x3F65B9,    // midi note 71, piano note G#5
    0x3F52DD,    // midi note 72, piano note A 6
    0x3F3DB4,    // midi note 73, piano note A#6
    0x3F25F7,    // midi note 74, piano note B 6
    0x3F0B54,    // midi note 75, piano note C 6
    0x3EED73,    // midi note 76, piano note C#6
    0x3ECBEF,    // midi note 77, piano note D 6
    0x3EA658,    // midi note 78, piano note D#6
    0x3E7C2E,    // midi note 79, piano note E 6
    0x3E4CE6,    // midi note 80, piano note F 6
    0x3E17E2,    // midi note 81, piano note F#6
    0x3DDC71,    // midi note 82, piano note G 6
    0x3D99CF,    // midi note 83, piano note G#6
    0x3D4F20,    // midi note 84, piano note A 7
    0x3CFB6E,    // midi note 85, piano note A#7
    0x3C9DAA,    // midi note 86, piano note B 7
    0x3C34A1,    // midi note 87, piano note C 7
    0x3BBF02,    // midi note 88, piano note C#7
    0x3B3B54,    // midi note 89, piano note D 7
    0x3AA7F5,    // midi note 90, piano note D#7
    0x3A0315,    // midi note 91, piano note E 7
    0x394AB5,    // midi note 92, piano note F 7
    0x387C9D,    // midi note 93, piano note F#7
    0x37965D,    // midi note 94, piano note G 7
    0x369548,    // midi note 95, piano note G#7
    0x35766D,    // midi note 96, piano note A 8
};

static struct empeg_eq_section_t eq_init[20] = {
	{ 0x00a8, 0x6f40 },
	{ 0x0147, 0x6e40 },
	{ 0x0276, 0x6540 },
	{ 0x04f5, 0x6440 },
	{ 0x09e4, 0x6340 }, 
	{ 0x13b3, 0x5a40 },
	{ 0x26f2, 0x5140 },
	{ 0x4af1, 0x4040 },
	{ 0x7ff0, 0x5840 },
	{ 0x8010, 0x0740 },
	{ 0x00a8, 0x6f40 },
	{ 0x0147, 0x6e40 },
	{ 0x0276, 0x6540 },
	{ 0x04f5, 0x6440 },
	{ 0x09e4, 0x6340 },
	{ 0x13b3, 0x5a40 },
	{ 0x26f2, 0x5140 },
	{ 0x4af1, 0x4040 },
	{ 0x7ff0, 0x5840 },
	{ 0x8010, 0x0740 },
};

static int eq_section_order[20] = {
	0, 10, 5, 15, 1, 11, 6, 16, 2, 12,
	7, 17, 3, 13, 8, 18, 4, 14, 9, 19
};

static dsp_setup beep_setup[] = {
	/* Timing generator scaling coefficients */
	{ Y_scalS1_,	0 },		/* 1-a scale = 0 */
	{ Y_scalS1,	0x7ff },	/* a scale   = 1 */

	/* Timing generator copy locations */
	{ Y_cpyS1,	0x8f9 },	/* copy a*S1 to c1 */
	{ Y_cpyS1_,	0x8fb },	/* nothing */

	{ Y_c0sin,	0 },		/* nothing */
	{ Y_c1sin,	0 },		/* controlled by a*S1 */
	{ Y_c2sin,	0 },		/* nothing */
	{ Y_c3sin,	0 },		/* nothing */
		
	/* Full volume */
	{ Y_VLsin,	0x7ff },	/* volume left  = 1 */
	{ Y_VRsin,	0x7ff },	/* volume right = 1 */
		
	{ Y_IClipAmax,	0 },		/* no output */
	{ Y_IClipAmin,	0 },		/* no output */
	{ Y_IClipBmax,	0x100 },	/* 50% clipping */
	{ Y_IClipBmin,	0x100 },	/* 50% clipping */
		
	/* Tone frequency */
	{ Y_IcoefAl,	0 },		/* written as required */
	{ Y_IcoefAh,	0 },
	{ Y_IcoefBL,	0 },
	{ Y_IcoefBH,	0 },

	/* Coefficients for channel beep volume */
	{ Y_tfnFL,	0x800 },	/* yes the manual says -1 */
	{ Y_tfnFR,	0x800 },	/* but that only causes */
	{ Y_tfnBL,	0x800 },	/* the wave to invert */
	{ Y_tfnBR,	0x800 },	/* which is ok */

	/* Attack / decay */
	{ Y_samAttl,	0 },		/* written when changing */
	{ Y_samAtth,	0 },		/* channels */
	{ Y_deltaA,	0 },
	{ Y_switchA,	0 },
	{ Y_samDecl,	0 },
	{ Y_samDech,	0 },
	{ Y_deltaD,	0 },
	{ Y_switchD,	0 },

	/* wave routing select */
	{ Y_iSinusWant,	0x82a },
	{ Y_sinusMode,	0x89a },	/* off */

	{ 0,0 }
};
