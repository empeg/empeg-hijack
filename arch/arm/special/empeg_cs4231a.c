/* empeg audio input support
 *
 * (C) 1999/2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Hugo Fiennes <hugo@empeg.com>
 *
 * The Crystal CS4231A is used for input from the microphone on the empeg
 * board (rev6+). This is a soundblaster-like type thing, with 16-bit audio
 * I/O and an ISA-style interface.
 *
 * It's hooked up to the outputs of the DSP (so we can sample anything the DSP
 * is outputting - eg for doing visuals on the radio or aux inputs), to the
 * mono microphone input, and to the aux inputs (allowing us to sample the aux
 * in even when playing other things through the DSP).
 *
 * Version 0.01 991103 hugo@empeg.com
 * Version 0.02 991113 hugo@empeg.com
 *   Made critical region as small as possible to improve IRQ latency
 * Version 0.03 000304 hugo@empeg.com
 *   RTC support (RTC hooked to cs4231's gpio) plus __init stuff
 * Version 0.04 000611 hugo@empeg.com
 *   Added ioctls to get/set channel, stereoness & gain
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
#include <linux/empeg.h>

#include "empeg_cs4231a.h"

/* Only one audio channel */
struct cs4231_dev cs4231_devices[1];

/* Logging for proc */
static char log[256];
static int log_size=0;

static inline void LOG(char x)
{
	if (log_size < sizeof(log))
		log[log_size++]=x;
}

static inline void LOGS(const char *x)
{
	while (*x) {
		LOG(*x);
		x++;
	}
}

/* FIQs stuff */
static struct fiq_handler fh= { NULL, "cs4231dma", NULL, NULL };

static struct file_operations cs4231_fops = {
  NULL, /* lseek */
  cs4231_read,
  NULL,
  NULL, /* readdir */
  cs4231_poll,
  cs4231_ioctl,
  NULL, /* mmap */
  cs4231_open,
  NULL, /* flush */
  cs4231_release,
};

/* Registers */
static volatile unsigned char *reg_cs4231_index=(unsigned char*)0xe0000040;
static volatile unsigned char *reg_cs4231_data=(unsigned char*)0xe0000044;
//static volatile unsigned char *reg_cs4231_status=(unsigned char*)0xe0000048;
//static volatile unsigned char *reg_cs4231_piodata=(unsigned char*)0xe000004c;
static volatile unsigned char *reg_cs4231_dma=(unsigned char*)0xe0000060;
extern unsigned long get_rtc_time(void);

#define STATUS() 		(*reg_cs4231_status)
#define READDATA() 		(*reg_cs4231_data)
#define WRITEDATA(x)		*reg_cs4231_data=(x)
#define INDEX(x)		*reg_cs4231_index=(x)
#define READINDEX()		(*reg_cs4231_index)
#define READDMA()		(*reg_cs4231_dma)
#define WRITEDMA()		*reg_cs4231_dma=(x)

/**********************************************************************/
/* This is the interrupt service routine for audio capture operations */
/**********************************************************************/
void cs4231_irq(int irq,void *dev_id, struct pt_regs *iregs)
{
	int a=0; unsigned char d,*p;
	struct cs4231_dev *dev=cs4231_devices;
	struct pt_regs regs;
	unsigned long flags;

	/* Disable FIQs: this should be a formality as we're the only
	   FIQ user on the empeg */
	save_flags_clif(flags);

	/* Next interrupt will be a FIQ */
	ICLR|=EMPEG_CRYSTALDRQ;	

	/* Clear edge */
	GEDR=EMPEG_CRYSTALDRQ;

	/* Buffer bytes which FIQ has fetched */
	get_fiq_regs(&regs);
	p=(unsigned char*)dev->fiq_buffer;
	a=(regs.ARM_r8-((int)dev->fiq_buffer));
	dev->samples+=a/2;
	if (a>dev->rx_free) a=dev->rx_free;
	dev->rx_used+=a;
	dev->rx_free-=a;
	while(a--) {
		dev->rx_buffer[dev->rx_head++]=*p++;
		if (dev->rx_head==CS4231_BUFFER_SIZE) dev->rx_head=0;
	}

	/* Read bytes until DRQ low */
	do {
		d=READDMA();
		if (dev->rx_free) {
			dev->rx_buffer[dev->rx_head++]=d;
			if (dev->rx_head==CS4231_BUFFER_SIZE) dev->rx_head=0;
			dev->rx_used++;
			dev->rx_free--;
		}
		dev->samples++;
	} while((GPLR&EMPEG_CRYSTALDRQ)==0);

	/* Wait up waiters */
	wake_up_interruptible(&dev->rx_wq);

	/* Set up registers for next bufferload */
	regs.ARM_r8=(int)dev->fiq_buffer;
	regs.ARM_r9=((int)dev->fiq_buffer)+512;
	set_fiq_regs(&regs);

	/* Re-enable FIQs - may cause an instant FIQ */
	restore_flags(flags);
}

static struct { short samplerate; char setup; } samplerates[]={
	{ 11025, 0x0d },
	{ 22050, 0x0c },
	{ 29400, 0x0a },
        { 0, 0 } };

static int setmode(struct cs4231_dev *dev, int channel, int rate, int stereo,
		   int gain)
{
	int a=0,timeout=jiffies+HZ;
	int stopped = 0;

	if (stereo >= 0 || rate >= 0)
	{
		stopped = 1;
		/* Stop the sampling */
		INDEX(INTERFACE_CONFIG);
		WRITEDATA(0x00);
	}

	if (channel>=0) {
		/* Select channel */
		int chbyte=(channel<<6),gain;

		/* Valid? */
		if (channel>EMPEG_AUDIOIN_CHANNEL_MIC) return -EINVAL;

		/* Add 20dB of gain for mic input */
		if (channel==EMPEG_AUDIOIN_CHANNEL_MIC) chbyte|=0x20;

		INDEX(LEFT_INPUT);
		gain=READDATA();
		WRITEDATA(chbyte|(gain&0x1f));	
		INDEX(RIGHT_INPUT);
		WRITEDATA(chbyte|(gain&0x1f));

		/* Update structure */
		dev->channel=channel;
	}

	/* Setting gain? */
	if (gain>=0) {
		/* Set gain on left/right: each increment is 1.5dB gain */
		if (gain>15) return -EINVAL;

		INDEX(LEFT_INPUT);
		WRITEDATA((READDATA()&0xf0)|gain);
		INDEX(RIGHT_INPUT);
		WRITEDATA((READDATA()&0xf0)|gain);

		/* Update structure */
		dev->gain=gain;
	}

	/* Setting stereo? */
	if (stereo>=0) {
		/* Turn on CMCE */
		INDEX(ALT_FEATURE_I);
		WRITEDATA(0x20);

		/* Set capture mode */
		INDEX(CAPTURE_FORMAT);
		WRITEDATA(stereo?0x50:0x40);
		udelay(10);

		/* Wait for it to take */
		while(READINDEX()==0x80 && jiffies<timeout);
		if (READINDEX()==0x80) {
			printk("cs4231 format select failed!\n");
			return -EIO;
		}

		/* Turn off CMCE */
		INDEX(ALT_FEATURE_I);
		WRITEDATA(0x00);

		/* Update structure */
		dev->stereo=stereo?1:0;
	}

	/* Setting samplerate? */
	if (rate>=0) {
		/* Select right clock divisor, etc */
		while(samplerates[a].samplerate && samplerates[a].samplerate!=rate) a++;
		
		/* Valid? */
		if (!samplerates[a].samplerate) return -EINVAL;
		INDEX(FS_AND_DATAFORMAT|INDEX_MCE);
		WRITEDATA(samplerates[a].setup);
		udelay(10);
		
		/* Wait for it to finish, then turn off MCE */
		while(READINDEX()==0x80 && jiffies<timeout);
		if (READINDEX()==0x80) {
			printk("cs4231 rate select failed!\n");
			return -EIO;
		}
		INDEX(FS_AND_DATAFORMAT);

		/* Update struct */
		dev->samplerate=rate;
	}

	/* Reset capture if device is open */
	if (stopped && dev->open) {
		unsigned long flags;
		struct pt_regs regs;
		int a;

		/* Dump buffer */
		save_flags_clif(flags);
		get_fiq_regs(&regs);
		regs.ARM_r8=(int)dev->fiq_buffer;
		regs.ARM_r9=((int)dev->fiq_buffer)+512;
		set_fiq_regs(&regs);
		
		/* Nothing in our buffer */
		dev->rx_head=dev->rx_tail=0;
		dev->rx_used=0;
		dev->rx_free=CS4231_BUFFER_SIZE;
		restore_flags(flags);
		
		/* Sync up fifo by emptying it */
		for(a=0;a<64;a++) READDMA();
		
		/* Restart it */
		INDEX(INTERFACE_CONFIG);
		WRITEDATA(0x02);
	}

	return(0);
}

static int cs4231_read_procmem(char *buf, char **start, off_t offset, int len, int unused)
{
	struct cs4231_dev *dev = cs4231_devices;
	len = 0;

	len+=sprintf(buf+len,"samples: %d\n",dev->samples);

	LOG(0);
	len+=sprintf(buf+len,"Log: %s",log);
	log_size=0;
	
	return len;
}

struct proc_dir_entry cs4231_proc_entry = {
	0,			/* inode (dynamic) */
	12, "empeg_cs4231",  	/* length and name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	0, 			/* size */
	NULL, 			/* use default operations */
	&cs4231_read_procmem, 	/* function used to read data */
};

/* Device initialisation */
void __init empeg_cs4231_init(void)
{
        struct cs4231_dev *dev=cs4231_devices;
	int result,version;

	MECR=(MECR&0xffff0000)|0x0007;

	/* Put chip into CS4231 mode */
	INDEX(MODE_ID);
	WRITEDATA(0xca);

	/* Read version */
	INDEX(VERSION);
	version=(READDATA()&0xe7);

	/* Check version */
	if (version!=0xa0) {
		printk(KERN_WARNING "Could not find CS4231A (version=%02x)\n",
		       version);
		return;
	}
		
	/* Set SDC bit (single DMA channel) */
	INDEX(INTERFACE_CONFIG|INDEX_MCE);
	WRITEDATA(0x00);

	/* Crystals on all the time */
	INDEX(ALT_FEATURE_II);
	WRITEDATA(0x02);

	/* Enable capture mode change */
	INDEX(ALT_FEATURE_I);
	WRITEDATA(0x20);

	/* 22kHz, aux input, mono, no gain */
	setmode(dev,EMPEG_AUDIOIN_CHANNEL_AUXIN,22050,0,0);

	/* Allocate buffers */
	dev->rx_buffer=vmalloc(CS4231_BUFFER_SIZE);
	if (!dev->rx_buffer) {
		printk(KERN_WARNING "Could not allocate memory for audio input buffer\n");
		return;
	}

	/* Initialise buffer bits */
	dev->rx_head=dev->rx_tail=0;
	dev->rx_used=0; dev->rx_free=CS4231_BUFFER_SIZE;
	dev->rx_wq = NULL;
	
	/* Claim IRQ: this gets called when the DMA (FIQ) buffer is full
	   and does the less time-critical work */
       	result=request_irq(2,cs4231_irq,0,"empeg_cs4231",dev);
	
	/* Got it ok? */
	if (result==0) {
                struct pt_regs regs;
		int length=((int)&empeg_fiq4231_end)-
			((int)&empeg_fiq4231_start);
		unsigned long flags;

		/* Install DMA handler */
		regs.ARM_r8=(int)dev->fiq_buffer;
		regs.ARM_r9=((int)dev->fiq_buffer)+512;
		regs.ARM_r10=0xe0000060; /* Where to read from */
		regs.ARM_fp=(int)&GPLR; /* r11 */
		set_fiq_regs(&regs);
		set_fiq_handler(&empeg_fiq4231_start,length);
		claim_fiq(&fh);
		
		/* Enable FIQs on falling edge only of DREQ (it's inverted) */
		GFER|=EMPEG_CRYSTALDRQ;
                GRER&=~EMPEG_CRYSTALDRQ;
		
		/* Clear edge detect */
		GEDR=EMPEG_CRYSTALDRQ;
		
		/* It's a FIQ not an IRQ */
		ICLR|=EMPEG_CRYSTALDRQ;

		/* Enable FIQs: there should be a neater way of doing
		   this... */
		save_flags(flags);
		flags&=~(0x40);
		restore_flags(flags);

		/* Dad's home! */
		printk("empeg audio-in initialised, CS4231A revision %02x\n",
		       version);

		/* RTC is on CS4231 dataports, initialise now & grab time.
		   I know this is a strange place to find this sort of stuff,
		   but it'll move at some point */
		xtime.tv_sec=get_rtc_time();
	}
	else {
		printk(KERN_ERR "Can't get empeg cs4231 DRQ %d.\n",
		       EMPEG_CRYSTALDRQ);
		return;
	}

	/* Get the device */
	result=register_chrdev(EMPEG_AUDIOIN_MAJOR,"empeg_cs4231",
			       &cs4231_fops);
	if (result<0) {
		printk(KERN_WARNING "empeg_cs4231: Major number %d unavailable.\n",
		       EMPEG_AUDIOIN_MAJOR);
		return;
	}

#ifdef CONFIG_PROC_FS
	proc_register(&proc_root, &cs4231_proc_entry);
#endif

	/* Start capture */
	INDEX(CAPTURE_FORMAT|INDEX_MCE);
	WRITEDATA(0x40);

	/* DMA length */
	INDEX(CDMA_HIGH);
	WRITEDATA(0xff);
	INDEX(CDMA_LOW);
	WRITEDATA(0xff);

	/* Don't start to sample yet */
       	INDEX(INTERFACE_CONFIG);
	WRITEDATA(0x00);
}

static int cs4231_open(struct inode *inode, struct file *flip)
{
	struct cs4231_dev *dev=cs4231_devices;

	MOD_INC_USE_COUNT;
	flip->private_data=dev;

	/* Device is open */
	dev->open=1;

	/* Reset params & start sampling */
	setmode(dev,dev->channel,dev->samplerate,dev->stereo,dev->gain);

	return 0;
}

static int cs4231_release(struct inode *inode, struct file *flip)
{
	struct cs4231_dev *dev=cs4231_devices;

	/* Stop the sampling */
       	INDEX(INTERFACE_CONFIG);
	WRITEDATA(0x00);

	/* Closed */
	dev->open=0;
	
	MOD_DEC_USE_COUNT;
	return 0;
}

/* Read data from audio buffer */
static ssize_t cs4231_read(struct file *flip, char *dest, size_t count, loff_t *ppos)
{
	struct cs4231_dev *dev=flip->private_data;
	unsigned long flags;
	size_t bytes;

	while (dev->rx_used==0) {
		if (flip->f_flags & O_NONBLOCK)
			return -EAGAIN;
      
		interruptible_sleep_on(&dev->rx_wq);
		/* If the sleep was terminated by a signal give up */
		if (signal_pending(current))
			return -ERESTARTSYS;
	}

	/* We can copy data out of the sound buffer without disabling IRQs, as
	   we're the only people who will be fiddling with the tail */
	if (count>dev->rx_used) count=dev->rx_used;
	bytes=count;
	while (bytes--) {
		*dest++ = dev->rx_buffer[dev->rx_tail++];
		if (dev->rx_tail==CS4231_BUFFER_SIZE)
			dev->rx_tail=0;
	}

	/* After the time consuming stuff has been done, we can now let the
	   rest of the world (ie, the IRQ routine) know that there's more
	   room in the buffer */
	save_flags_cli(flags);
	dev->rx_free+=count;
	dev->rx_used-=count;
	restore_flags(flags);

	return count;
}

static unsigned int cs4231_poll(struct file *filp, poll_table *wait)
{
	struct cs4231_dev *dev = filp->private_data;
	unsigned int mask = 0;

	poll_wait(filp, &dev->rx_wq, wait);

	/* Is there stuff in the read buffer? */
	if (dev->rx_used)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static int cs4231_ioctl(struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct cs4231_dev *dev = filp->private_data;
	int parm;

	switch(cmd) {
	case EMPEG_AUDIOIN_READ_SAMPLERATE:
		/* Read samplerate */
		put_user_ret(dev->samplerate, (int*)arg, -EFAULT);
		return 0;

	case EMPEG_AUDIOIN_WRITE_SAMPLERATE:
		/* Set samplerate */
		get_user_ret(parm, (int*)arg, -EFAULT);
		return setmode(dev,-1,parm,-1,-1);

	case EMPEG_AUDIOIN_READ_CHANNEL:
		/* Read channel */
		put_user_ret(dev->channel, (int*)arg, -EFAULT);
		return 0;

	case EMPEG_AUDIOIN_WRITE_CHANNEL:
		/* Set samplerate */
		get_user_ret(parm, (int*)arg, -EFAULT);
		return setmode(dev,parm,-1,-1,-1);

	case EMPEG_AUDIOIN_READ_STEREO:
		/* Read stereoness */
		put_user_ret(dev->stereo, (int*)arg, -EFAULT);
		return 0;

	case EMPEG_AUDIOIN_WRITE_STEREO:
		/* Set stereoness */
		get_user_ret(parm, (int*)arg, -EFAULT);
		return setmode(dev,-1,-1,parm,-1);

	case EMPEG_AUDIOIN_READ_GAIN:
		/* Read gain */
		put_user_ret(dev->gain, (int*)arg, -EFAULT);
		return 0;

	case EMPEG_AUDIOIN_WRITE_GAIN:
		/* Set gain */
		get_user_ret(parm, (int*)arg, -EFAULT);
		return setmode(dev,-1,-1,-1,parm);
	}

	return -EINVAL;
}
