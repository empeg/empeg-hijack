/*
 * SA1100 SSP Audio Device Driver
 * Copyright (c) 1999 Nicolas Pitre <nico@cam.org>
 *
 * This driver assumes the DAC on the SSP port is able to do 
 * 16 bits stereo samples at 44100 Hz only.  A simple resampling is 
 * performed by software for other audio formats based on a linear 
 * interpolation.
 *
 * The SA1100's DMA channel 0 is used for data transfer to the SSP port.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tqueue.h>
#include <linux/errno.h>
#include <linux/soundcard.h>

#include <asm/uaccess.h>
#include <asm/segment.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/proc-fns.h>
#include <asm/arch/hardware.h>



/* Definitions */

// #define DEBUG

#define AUDIO_NAME		"audio-sa1100-ssp"
#define AUDIO_NAME_VERBOSE	"SA1100 SSP audio driver"
#define AUDIO_VERSION_STRING	"version 0.5"

#define AUDIO_FMT_MASK		(AFMT_U8 | AFMT_S16_LE)
#define AUDIO_FMT_DEFAULT	(AFMT_U8)
#define AUDIO_CHANNELS_DEFAULT	1		/* default: mono */
#define AUDIO_RATE_DEFAULT	8000
#define AUDIO_NBFRAGS_DEFAULT	8
#define AUDIO_FRAGSIZE_DEFAULT	8192

#define AUDIO_MAJOR		42
#define AUDIO_DSP_MINOR		0
#define AUDIO_MIXER_MINOR	1
#define AUDIO_IRQ		IRQ_DMA0


/* Global variables */

struct audio_buf {
	volatile enum { S_FREE, S_INPUT, S_OUTPUT } state;  /* buffer states */
	unsigned int rate;		/* sample rate for this buffer */
	int stereo;			/* == 1 if this buffer is stereo */
	int fmt;			/* sample format for this buffer */
	unsigned int size;		/* buffer size */
	char *data;			/* points to actual buffer */
	char *pout;			/* current outgoing data position */
};

static struct audio_buf *audio_buffers;	/* pointer to audio buffer structures */
static struct audio_buf *inbuf, *outbuf;  /* current buffer being filled and drained */
static int inbuf_index, outbuf_index;	/* ... in the buffer array */
static char *audio_pool;		/* buffer data space */

/* Current specs for incoming audio data */
static unsigned int audio_rate;		
static int audio_channels;
static int audio_fmt;

static unsigned int audio_fragsize;	/* current fragment i.e. buffer size */
static unsigned int audio_nbfrags;	/* current nbr of gragments i.e. buffers */

static struct wait_queue *audio_waitq;	/* ... to wait for DMA completion */
static int volatile audio_dma_active;	/* == 1 if DMA transfer activated */
static int volatile audio_dma_sync;	/* == 1 to sync partial dma buffer */
static int audio_refcount;		/* nbr of concurrent open() */

static char *audio_dmabuf;		/* points to the DMA buffer memory */
static unsigned int audio_dmacount;	/* data count in current DMA buffer */
static struct tq_struct audio_task;	/* ... for bottom half DMA processing */

static unsigned int audio_volume;	/* current volume level */
static unsigned int audio_shift1;	/* first shift value for volume atenuation */
static unsigned int audio_shift2;	/* second shift value for volume atenuation */
static unsigned int audio_tone = 7;	/* current tone level */


/* This function frees all buffers */

static void audio_clear_buf( void )
{
    kfree( audio_pool );
    kfree( audio_buffers );
    audio_pool = 0;
    audio_buffers = inbuf = outbuf = 0;
    inbuf_index = outbuf_index = 0;
    audio_dmacount = 0;
}


/* This function allocates the buffer structure array and buffer data space
 * according to the current number of fragments and fragment size.
 */

static int audio_setup_buf( void )
{
    int i;

    if( audio_pool ) {
	printk( AUDIO_NAME ": audio_pool already exists\n" );
	return -1;
    }

    audio_pool = (char *)kmalloc( audio_fragsize * audio_nbfrags, GFP_KERNEL );
    audio_buffers = (struct audio_buf *)kmalloc( 
	sizeof( struct audio_buf ) * audio_nbfrags, GFP_KERNEL );
    if( !audio_pool || !audio_buffers ) {
	printk( AUDIO_NAME ": unable to allocate audio memory\n ");
	audio_clear_buf();
	return -1;
    }

    for( i = 0; i < audio_nbfrags; i++ ) {
	audio_buffers[i].state = S_FREE;
	audio_buffers[i].data = audio_pool + i * audio_fragsize;
    }
    inbuf = outbuf = audio_buffers;
    inbuf_index = outbuf_index = 0;

    return 0;
}



/* Tone (bass/treble) filter and friends... */

#if 1

#define FRACBITSCOEFF	14
#define NR_BIT_SAMPLE	16
#define INTBITSAMPLE	1
#define FRACBITSAMPLE	(32-(NR_BIT_SAMPLE+INTBITSAMPLE+FRACBITSCOEFF+1))
#define NUMCOEFF	5

static const int filter_coeff[][NUMCOEFF] = {
    {0x16, 0x2D, 0x16, 0x7914, 0xFFFFC691},
    {0x3F, 0x7E, 0x3F, 0x7441, 0xFFFFCAC2},
    {0xAC, 0x159, 0xAC, 0x6C1D, 0xFFFFD130},
    {0x1C6, 0x38C, 0x1C6, 0x5E87, 0xFFFFDA61},
    {0x470, 0x8E1, 0x470, 0x4844, 0xFFFFE5F8},
    {0xA83, 0x1507, 0xA83, 0x24AD, 0xFFFFF144},
    {0x183A, 0x3075, 0x183A, 0xFFFFEB42, 0xFFFFF3D4},
    {0x3F5B, 0xFFFF8149, 0x3F5B, 0x7EB5, 0xFFFFC147},
    {0x3EE1, 0xFFFF823D, 0x3EE1, 0x7DBE, 0xFFFFC238},
    {0x3E68, 0xFFFF8330, 0x3E68, 0x7CC6, 0xFFFFC325}
};

static unsigned int CurrentToneLevel = 7;
static const int *ToneFilterCoeff = NULL;

/*********************************************************************
 * Function:	SetToneLevel
 * Author:	Stephane Dalton
 * History:	1999/02/04 ->	creation
 * Parameters:	in->	the required tone level: 0..10, 7 = off
 *********************************************************************/
static void SetToneLevel( int level )
{
	if(level == 7){
		CurrentToneLevel = 7;
		ToneFilterCoeff = NULL;
	}else{
		if(level < 7){
			CurrentToneLevel = level;
		}else if(level <= 10){
			CurrentToneLevel = level-1;
		}else return;
		ToneFilterCoeff = filter_coeff[CurrentToneLevel];
	}
}

/*********************************************************************
 * Function:	Tone
 * Author:	Stephane Dalton
 * History:	1999/02/03 ->	creation
 * 		1999/02/04 ->	Changed the input parameters
 * 				structure to become a buffer.
 * 		1999/02/09 ->	Clamp possible overflowing samples.
 * Parameters:	in->	pointer to a previous samples buffer
 * 		out->	the current output sample
 * Abstract:	This function computes the differential equation of
 * 		a filter given its coefficients and the samples to
 * 		which they're applied
 * Note:	If the tone function is off, the returned sample is 
 * 		simply the current input sample.
 *********************************************************************/
inline int Tone( int *prev_samples )
{
	int output, ov, i;

	if( !ToneFilterCoeff )
		return *prev_samples;

	for( i=0, output=0; i<NUMCOEFF; i++, prev_samples++ ){
		output += ToneFilterCoeff[i] * (*prev_samples);
	}

/*
 * The following is used to determine a eventual overflow in the addition
 * because of the value of coefficient, the multiplication cannot overflow
 */
	ov = output & 0xe0000000;	/* keep only interesting bits */

	if( ov == 0 || ov == 0xe0000000 )	/* no overflow */
		return (output >> FRACBITSCOEFF);

/*
 * From now we know there's an overflow (negative or positive).
 * Clamp the output value.
 */
	if( (ov & 0x80000000) ) {
		return -32768;
	}else{
		return 32767;
	}
}

#define TONE( samp, filterdata )					\
	filterdata[0] = samp >> 16;					\
	samp = Tone( filterdata );					\
	filterdata[4] = filterdata[3];					\
	filterdata[3] = samp;						\
	filterdata[2] = filterdata[1];					\
	filterdata[1] = filterdata[0];					\
	samp <<= 16;

#else
#define NUMCOEFF 1
#define SetToneLevel( x )
#define TONE( samp, filterdata )
#endif

/* backsample buffers for filters */
static int filter_left[NUMCOEFF];
static int filter_right[NUMCOEFF];



/* This function returns the linear interpolation between two points a and b.
 * The step parameter specifies how many points will be needed. If
 * step = 2 it means two points between a and b i.e one at 1/3 and one 
 * at 2/3.  We return only the first point and the second will be found
 * when this function is called again with this newly found point as a
 * and step = 1.
 * We use series of division by power of 2 for performance reasons.
 * These divisions are replaced by shifts so the assembler is prettyer.
 * This cause rounding to be downwards, even for negative numbers i.e. 
 * -1/2 = -1 but we can live with that.
 */

static inline int interpol( int a, int b, int step )
{
    int d = (b - a);
    switch( step ) {
      case 1:	/* (a + b) / 2 */
	return a + (d >> 1);
      case 2:	/* a + (b - a)/3 */
	return a + (d >> 2) + (d >> 4) + (d >> 6) + (d >> 8) + (d >> 10);
      case 3:	/* a + (b - a)/4 */
	return a + (d >> 2);
      case 4:	/* a + (b - a)/5 */
	return a + (d >> 3) + (d >> 4) + (d >> 7) + (d >> 8) + (d >> 11);
      case 5:	/* a + (b - a)/6 */
	return a + (d >> 3) + (d >> 5) + (d >> 7) + (d >> 9) + (d >> 11);
    }
    return a;	/* should never get here */
}


static int audio_setvol( int x )
{
   x &= 0xFF;
   if( x > 100 ) x = 100;
   if( x < 0 ) x = 0;
   audio_shift1 = 10 - x/10;
   audio_shift2 = ((x/5) % 2) ? audio_shift1 + 1 : 0;
   audio_volume = x;
   //printk( "audio_setvol: v %d, s1 %d, s2 %d\n", x, audio_shift1, audio_shift2 );
   return x;
}


/* This produces the right data word with left and right sample values.
 * Those values are assumed to be in the 16 most significant bits of an int.
 * While being there, we stick the volume control to it.
 */
#define DMADATA( L, R ) 						\
  (((unsigned long)(((L) >> audio_shift1) + (audio_shift2 ? ((L) >> audio_shift2) : 0) ) >> 16) | 	\
   ((unsigned long)(((R) >> audio_shift1) + (audio_shift2 ? ((R) >> audio_shift2) : 0) ) & 0xFFFF0000))


static int prev_sampL = 0, prev_sampR = 0;  /* ... for interpolation */
static long intime = 0, outtime = 0;	/* weight of transient samples */
static int undersamp = 0;		/* undersampling state */


/* The next two functions are doing all necessary processing for each 
 * audio samples i.e. interpolation, filtering, etc.
 */

static inline unsigned long * process_stereo_sample( int sampL, int sampR,
						     unsigned long *buf )
{

    /* Check if resampling is required */
    if( intime < outtime ){
	int interp = 0;

	/* Oversampling : find out interpolation step value */
	do{
	    intime += outbuf->rate;
	    interp++;
	}while( intime < outtime );

	/* add interpolated samples */
	do{
	    int resL, resR;

	    resL = interpol( prev_sampL, sampL, interp );
	    prev_sampL = resL;
	    TONE( resL, filter_left );

	    resR = interpol( prev_sampR, sampR, interp );
	    prev_sampR = resR;
	    TONE( resR, filter_right );

	    *buf++ = DMADATA( resL, resR );
	}while( --interp );
    }else if( intime >= outtime + 44100 ){
	/* Undersampling needed : we merge two consecutive samples into one.
	 * For instance, keep the current sample.
	 */
	prev_sampL = sampL;
	prev_sampR = sampR;
	outtime += 44100;
	undersamp = 1;
	return buf;
    }else if( undersamp != 0 ){
	/* sample = (sample + previous_sample)/2 */
	sampL = (sampL >> 1) + (prev_sampL >> 1);
	sampR = (sampR >> 1) + (prev_sampR >> 1);
    }

    /* now add current sample */
    prev_sampL = sampL;
    TONE( sampL, filter_left );
    prev_sampR = sampR;
    TONE( sampR, filter_right );
    *buf++ = DMADATA( sampL, sampR );

    /* adjust sample weight */
    intime += outbuf->rate;
    outtime += 44100;

    return buf;
}


static inline unsigned long * process_mono_sample( int sample, 
						   unsigned long *buf )
{

    /* Check if resampling is required */
    if( intime < outtime ){
	int interp = 0;

	/* Oversampling : find out interpolation step value */
	do{
	    intime += outbuf->rate;
	    interp++;
	}while( intime < outtime );

	/* add interpolated samples */
	do{
	    int res = interpol( prev_sampL, sample, interp );
	    prev_sampL = res;
	    TONE( res, filter_left );
	    *buf++ = DMADATA( res, res );
	}while( --interp );
    }else if( intime >= outtime + 44100 ){
	/* Undersampling needed : we merge two consecutive samples into one.
	 * For instance, keep the current sample.
	 */
	prev_sampL = sample;
	outtime += 44100;
	undersamp = 1;
	return buf;
    }else if( undersamp != 0 ){
	/* sample = (sample + previous_sample)/2 */
	sample = (sample >> 1) + (prev_sampL >> 1);
    }

    /* now add current sample */
    prev_sampL = sample;
    TONE( sample, filter_left );
    *buf++ = DMADATA( sample, sample );

    /* adjust sample weight */
    intime += outbuf->rate;
    outtime += 44100;

    return buf;
}


/* number of samples per dma buffer */
#define DMA_SZ 2047



/* These functions process all DMA buffers */

static void audio_start_dma( int use_bufa, long * buf, int size )
{
    if( use_bufa ){
	ClrDCSR0 = DCSR_DONEA|DCSR_STRTA;
	DBSA0 = (void*)virt_to_phys(buf);
	DBTA0 = size*4;
	SetDCSR0 = DCSR_STRTA|DCSR_IE|DCSR_RUN;
    }else{
	ClrDCSR0 = DCSR_DONEB|DCSR_STRTB;
	DBSB0 = (void*)virt_to_phys(buf);
	DBTB0 = size*4;
	SetDCSR0 = DCSR_STRTB|DCSR_IE|DCSR_RUN;
    }
#ifdef CONFIG_SA1100_VICTOR
    GPSR = GPIO_GPIO21;		/* audio amp on */
#endif
}


static void audio_feed_dma( void *dummy )
{
    unsigned long *buf;
    int use_bufa;

new_dma:
    {
      long status = RdDCSR0;

      /* If both DMA channels are started, there's nothing else we can do. */
      if( (status & DCSR_STRTA) && (status & DCSR_STRTB) )
      {
	/* Bug???  Here we return because both dma channels are started.
	 * Obviously they should generate an interrupt when they are done 
	 * and we'll be called again.  It seems like if once in a while
	 * maybe once every one hour of contigous audio, we aren't called
	 * as it should and everything stall.  This is a hack... Force a
	 * callback through the timer task queue.
	 */
	queue_task( &audio_task, &tq_timer );
	return;
      }

      /* Which DMA channel can be used... */
      use_bufa = ( ((status & DCSR_BIU) && (status & DCSR_STRTB)) ||
		   (!(status & DCSR_BIU) && !(status & DCSR_STRTA)) );
    }
    buf = (unsigned long *)audio_dmabuf;
    if( !use_bufa ) buf += 2048;

    for(;;) {
	if( outbuf->state != S_OUTPUT ) {
	    /* no more data available */
	    if( audio_dmacount && audio_dma_sync ) {
		audio_start_dma( use_bufa, buf, audio_dmacount );
		audio_dmacount = 0;
	    }
	    if( (RdDCSR0 & DCSR_STRTA) || (RdDCSR0 & DCSR_STRTB) )
		audio_dma_active = 0;
	    wake_up_interruptible( &audio_waitq );	/* for sync */
	    return;
	}

	{
	    unsigned long *b, *b0;	/* pointer in current DMA buffer */
	    unsigned int ic;		/* sample input count */

	    b = b0 = &buf[audio_dmacount];

	    /* This is the input cycles value.  Here we compute the number
	     * of input samples required to fill the current dma buffer.  We
	     * must take care of the expansion factor due to resampling and
	     * interpolated samples.  This value will be clamped later if
	     * input data required exceed what's available in current
	     * data buffer.
	     */
	    ic = (DMA_SZ - 1 - audio_dmacount) * outbuf->rate / 44100;
	    if( ((DMA_SZ - audio_dmacount) * outbuf->rate % 44100) ) {
		/* We are susceptible to get an overflow because 
		 * we can't guess easily if truncated decimals will 
		 * show in this buffer or not.  Play safe!
		 */
		if( ic > 8 ) ic -= 8;
		else ic = 0;
	    }

	    /* Here goes processing for each specific sample formats */
	    if( outbuf->stereo ) {
	      if( outbuf->fmt == AFMT_S16_LE ) {
		/* 16 bits stereo samples */
		if( ic > outbuf->size/4 ) ic = outbuf->size/4;
		outbuf->size -= ic*4;
		while( ic-- ) {
		  int sampL = (*((short*)outbuf->pout)++) << 16;
		  int sampR = (*((short*)outbuf->pout)++) << 16;
		  b = process_stereo_sample( sampL, sampR, b );
		}
	      }else{
		/* 8 bits stereo samples */
		if( ic > outbuf->size/2 ) ic = outbuf->size/2;
		outbuf->size -= ic*2;
		while( ic-- ) {
		  int sampL = (*outbuf->pout++ << 24) ^ 0x80000000;
		  int sampR = (*outbuf->pout++ << 24) ^ 0x80000000;
		  b = process_stereo_sample( sampL, sampR, b );
		}
	      }
	    }else{
	      /* mono samples */
	      if( outbuf->fmt == AFMT_S16_LE ) {
		/* 16 bits mono samples */
		if( ic > outbuf->size/2 ) ic = outbuf->size/2;
		outbuf->size -= ic*2;
		while( ic-- ) {
		  unsigned long samp = (*((short*)outbuf->pout)++) << 16;
		  b = process_mono_sample( samp, b );
		}
	      }else{
		/* 8 bits mono samples */
		if( ic > outbuf->size ) ic = outbuf->size;
		outbuf->size -= ic;
		while( ic-- ) {
		  int samp = (*outbuf->pout++ << 24) ^ 0x80000000;
		  b = process_mono_sample( samp, b );
		}
	      }
	      /* We used only left filter. Copy its state to the right
	       * filter in case next buffer is stereo
	       */
	      prev_sampR = prev_sampL;
	      memcpy( filter_right, filter_left, sizeof(filter_right) );
	    }
	    audio_dmacount += b - b0;

	    /* to prevent wraparound */
	    intime -= outtime;
	    outtime = 0;

	    if( audio_dmacount > DMA_SZ ) {
		printk( AUDIO_NAME ": busted dma buffer with %d\n", audio_dmacount );
		audio_dmacount = DMA_SZ;
	    }

#ifdef DEBUG
    printk( "audio_feed_dma(): buf %d, dmabuf %d, count %d remain %d\n",
	outbuf_index, use_bufa, audio_dmacount, outbuf->size );
#endif

	    /* Check if current dma buffer is full.  If so, launch it now. */
	    if( audio_dmacount >= DMA_SZ-48 ) {
		audio_start_dma( use_bufa, buf, audio_dmacount );
		audio_dmacount = 0;
		goto new_dma;	/* Mmmm. I know... :)  Can't do otherwise. */
	    }
	}

	/* current buffer is empty: recycle it and wake up any sleeping 
	 * process waiting for a free buffer.
	 */
	outbuf->state = S_FREE;
	outbuf_index++;
	outbuf_index %= audio_nbfrags;
	outbuf = &audio_buffers[outbuf_index];
	wake_up_interruptible( &audio_waitq );
    }
}


static void audio_irq(int irq, void *dev_id, struct pt_regs *regs)
{
    ClrDCSR0 = DCSR_ERROR|DCSR_IE;
    queue_task( &audio_task, &tq_immediate );
    mark_bh(IMMEDIATE_BH);
}


static int audio_flush( struct file *file )
{
    audio_dma_sync = 1;
    while( audio_dma_active ){
	interruptible_sleep_on( &audio_waitq );
	if( signal_pending(current) )
	    return -EINTR;
    }
    return 0;
}


static int audio_write( struct file *file, const char *buffer, 
			size_t count, loff_t *ppos )
{
    unsigned int minor = MINOR(file->f_dentry->d_inode->i_rdev);
    const char *buffer0 = buffer;
    int chunk = 0;

    if( minor != AUDIO_DSP_MINOR ) return -EIO;

#ifdef DEBUG
    printk(AUDIO_NAME ": audio_write: count=%d\n", count);
#endif

    if( !audio_pool && audio_setup_buf() ) return -ENOMEM;

    audio_dma_sync = 0;
    while( count > 0 ) {
	switch( inbuf->state ) {
	  case S_FREE:
	    inbuf->state = S_INPUT;
	    inbuf->rate = audio_rate;
	    inbuf->stereo = audio_channels - 1;
	    inbuf->fmt = audio_fmt;
	    inbuf->size = 0;
	    inbuf->pout = inbuf->data;
	    /* no break here */

	  case S_INPUT:
	    if( inbuf->rate == audio_rate &&
		inbuf->stereo == audio_channels - 1 &&
		inbuf->fmt == audio_fmt )
	    {
		/* feed the current buffer */
		chunk = audio_fragsize - inbuf->size;
		if( chunk > count ) chunk = count;
		copy_from_user( inbuf->data + inbuf->size, buffer, chunk );
		buffer += chunk;
		count -= chunk;
		inbuf->size += chunk;
		if( inbuf->size < audio_fragsize ) break;
	    }
	    /* Mark current buffer for output */
#ifdef DEBUG
	    printk( AUDIO_NAME ": buf %d sz %d fmt %d ra %d st %d\n",
			inbuf_index, inbuf->size, inbuf->fmt, inbuf->rate, inbuf->stereo ); 
#endif
	    inbuf->state = S_OUTPUT;
	    audio_dma_active = 1;
	    queue_task( &audio_task, &tq_immediate );
	    mark_bh(IMMEDIATE_BH);
	    inbuf_index++;
	    inbuf_index %= audio_nbfrags;
	    inbuf = &audio_buffers[inbuf_index];
	    break;

	  default:
	    /* No buffer space available */
	    /* don't wait if non-blocking */
	    if( file->f_flags & O_NONBLOCK )
		return( (buffer - buffer0) ? (buffer - buffer0) : -EAGAIN);
	    interruptible_sleep_on( &audio_waitq );
	    if( signal_pending(current) )
		return( (buffer - buffer0) ? (buffer - buffer0) : -EINTR);
	    break;
	}
    }

    return( buffer - buffer0 );
}


#if 0
static void print_debug( void )
{
    int i;

    printk( "Audio driver debug:\n" );
    printk( "Current: rate = %d, channels = %d, fmt = %d\n",
	audio_rate, audio_channels, audio_fmt );
    printk( "dma: active = %d, count = %d\n",  
	audio_dma_active, audio_dmacount );
    printk( "inbuf = %d, outbuf = %d, fragsize = %d, nbfrags = %d\n",
	inbuf_index, outbuf_index, audio_fragsize, audio_nbfrags );
    for( i = 0; i < audio_nbfrags; i++ ){
	printk( "Buf %d: state = %d, rate = %d, stereo = %d, fmt = %d, size = %d\n",
		i, 
		audio_buffers[i].state,
		audio_buffers[i].rate,
		audio_buffers[i].stereo,
		audio_buffers[i].fmt,
		audio_buffers[i].size );
    }
}
#endif


static int audio_ioctl( struct inode *inode, struct file *file,
			uint cmd, ulong arg )
{
    long val;

    /* dispatch based on command */
    switch( cmd ) {
      case SNDCTL_DSP_SETFMT:
	get_user( val, (long*) arg );
	if( val & AUDIO_FMT_MASK ) {
	    audio_fmt = val;
	    break;
	}else return -EINVAL;

      case SNDCTL_DSP_CHANNELS:
      case SNDCTL_DSP_STEREO:
	get_user( val, (long*) arg );
	if( cmd == SNDCTL_DSP_STEREO ) val = val ? 2 : 1;
	if( val == 1 || val == 2 ) {
	    audio_channels = val;
	    break;
	}else return -EINVAL;

      case SNDCTL_DSP_SPEED:
	get_user( val, (long*) arg );
	if( val > 48000 ) val = 48000;
	if( val < 8000 ) val = 8000;
	audio_rate = val;
	break;

      case SNDCTL_DSP_GETFMTS:
	put_user( AUDIO_FMT_MASK, (long*)arg );
	break;

      case SNDCTL_DSP_GETBLKSIZE:
	put_user( 4096, (long*)arg );
	break;

      case SNDCTL_DSP_SETFRAGMENT:
	if( audio_pool ) return -EBUSY;
	get_user( val, (long*)arg );
	audio_fragsize = 1 << (val & 0xFFFF);
	if( audio_fragsize < 16 ) audio_fragsize = 16;
	if( audio_fragsize > 16384 ) audio_fragsize = 16384;
	audio_nbfrags = (val >> 16) & 0x7FFF;
	if( audio_nbfrags < 2 ) audio_nbfrags = 2;
	if( audio_nbfrags * audio_fragsize > 128*1024 ) 
	    audio_nbfrags = 128*1024 / audio_fragsize;
	if( audio_setup_buf() ) return -ENOMEM;
	break;

      case SNDCTL_DSP_SYNC:
	return audio_flush( file );

      case SNDCTL_DSP_GETOSPACE:
	{
	   audio_buf_info *inf = (audio_buf_info *)arg;
	   int err = verify_area( VERIFY_READ, inf, sizeof(*inf) );
	   int i;
	   int f = 0, b = 0;

	   if( err ) return err;
	   for( i = 0; i < audio_nbfrags; i++ ){
	      switch( audio_buffers[i].state ){
		case S_FREE:
		   f++;
		   b += audio_fragsize;
		   break;
		case S_INPUT:
		   b += audio_fragsize - audio_buffers[i].size;
		   break;
	      }
	   }
	   put_user( f, &inf->fragments );
	   put_user( audio_nbfrags, &inf->fragstotal );
	   put_user( audio_fragsize, &inf->fragsize );
	   put_user( b, &inf->bytes );
	}
	break;

      case SNDCTL_DSP_RESET:
      case SNDCTL_DSP_POST:
      case SNDCTL_DSP_SUBDIVIDE:
      case SNDCTL_DSP_GETISPACE:
      case SNDCTL_DSP_NONBLOCK:
      case SNDCTL_DSP_GETCAPS:
      case SNDCTL_DSP_GETTRIGGER:
      case SNDCTL_DSP_SETTRIGGER:
      case SNDCTL_DSP_GETIPTR:
      case SNDCTL_DSP_GETOPTR:
      case SNDCTL_DSP_MAPINBUF:
      case SNDCTL_DSP_MAPOUTBUF:
      case SNDCTL_DSP_SETSYNCRO:
      case SNDCTL_DSP_SETDUPLEX:
	return -EINVAL;

      /* mixer stuff */
      case SOUND_MIXER_READ_DEVMASK:
	val = ((1<<SOUND_MIXER_TREBLE)|(1<<SOUND_MIXER_VOLUME));
	put_user( val, (long*)arg );
	break;
      case SOUND_MIXER_READ_RECMASK:
	put_user( 0, (long*)arg );
	break;
      case SOUND_MIXER_READ_STEREODEVS:
	put_user( 0, (long*)arg );
	break;
      case SOUND_MIXER_READ_CAPS:
	put_user( 0, (long*)arg );
	break;

      case SOUND_MIXER_WRITE_VOLUME:
	get_user( val, (long*) arg );	
	audio_setvol( val );
	break;
      case SOUND_MIXER_READ_VOLUME:
	val = audio_volume * 10;
	val |= val << 8;
	put_user( val, (long*)arg );
	break;
      case SOUND_MIXER_WRITE_TREBLE:
	get_user( val, (long*) arg );	
	audio_tone = (val & 0xFF) / 10;
	if( audio_tone > 10 ) audio_tone = 10;
	SetToneLevel( audio_tone );
	if( audio_tone == 7 ){
	    memset( filter_left, 0, sizeof(filter_left) );
	    memset( filter_right, 0, sizeof(filter_right) );
	}
	break;
      case SOUND_MIXER_READ_TREBLE:
	val = audio_tone * 10;
	val |= val << 8;
	put_user( val, (long*)arg );
	break;

      default:
	return -EINVAL;
    }
    return 0;
}


static int audio_open( struct inode *inode, struct file *file )
{
#ifdef DEBUG
    printk(AUDIO_NAME ": audio_open\n");
#endif

    if( MINOR( inode->i_rdev ) == AUDIO_MIXER_MINOR ) return 0;
    if( MINOR( inode->i_rdev ) != AUDIO_DSP_MINOR ) return -ENODEV;

    if( audio_refcount > 0 ) return -EBUSY;

    audio_dmabuf = (char *)kmalloc( 16384, GFP_KERNEL );
    if( !audio_dmabuf ) return -ENOMEM;

    /* The SA1100 manual says: cache misses on write never allocate 
     * the cache.  Since we will only be writing to this memory, we
     * flush it once right here.
     */
    processor.u.armv3v4._flush_cache_area( 
	(unsigned long)audio_dmabuf, (unsigned long)(audio_dmabuf + 16384), 0 );

    audio_refcount++;
    MOD_INC_USE_COUNT;

    audio_rate = AUDIO_RATE_DEFAULT;
    audio_channels = AUDIO_CHANNELS_DEFAULT;
    audio_fmt = AUDIO_FMT_DEFAULT;
    audio_fragsize = AUDIO_FRAGSIZE_DEFAULT;
    audio_nbfrags = AUDIO_NBFRAGS_DEFAULT;
    audio_clear_buf();

    return 0;
}


static int audio_release( struct inode *inode, struct file *file )
{

#if AUDIO_DEBUG_VERBOSE
    printk(AUDIO_NAME ": audio_release\n");
#endif

    if( MINOR( inode->i_rdev ) != AUDIO_DSP_MINOR ) return 0;

    audio_flush( file );
    audio_clear_buf();
    kfree( audio_dmabuf );
#ifdef CONFIG_SA1100_VICTOR
    GPCR = GPIO_GPIO21;		/* audio amp off */
#endif
    audio_refcount--;
    MOD_DEC_USE_COUNT;
    return 0;
}


static struct file_operations audio_fops =
{
   NULL,		/* lseek */
   NULL,		/* read */
   audio_write,		/* write */
   NULL,		/* readdir */
   NULL,		/* poll */
   audio_ioctl,		/* ioctl */
   NULL,		/* mmap */
   audio_open,		/* open */
   audio_flush,		/* flush */
   audio_release	/* release */
};


static inline void audio_ssp_init( void )
{

#ifdef CONFIG_SA1100_VICTOR
    /* set GPIO registers.. */
#ifdef CONFIG_VICTOR_BOARD1
    GPDR |=  (GPIO_GPIO( 20 ) | GPIO_GPIO( 21 ) | GPIO_GPIO( 22 ));
#else
    GPDR |=  GPIO_GPIO21;	/* amp control */
#endif
    GPDR &= ~GPIO_GPIO( 19 );
    GAFR |=  GPIO_GPIO( 19 );
    GPSR =   GPIO_GPIO( 21 );
#ifdef CONFIG_VICTOR_BOARD1
    GPCR =   (GPIO_GPIO( 20 ) | GPIO_GPIO( 22 ));
#endif
#endif	/* CONFIG_SA1100_VICTOR */

#ifdef CONFIG_SA1100_CITYGO
    /* set GPIO registers.. */
    GPDR |=  GPIO_GPIO( 20 );
    GPDR &= ~GPIO_GPIO( 19 );
    GAFR |=  GPIO_GPIO( 19 );
    GPSR =   GPIO_GPIO( 20 );
#endif

    /* disable MCP */
    Ser4MCCR0 &= ~MCCR0_MCE;

    /* turn on the SSP */
    Ser4SSCR0 = (SSCR0_DataSize(16) | SSCR0_TI | SSCR0_SerClkDiv(2) |
		 SSCR0_SSE);
    Ser4SSCR1 = (SSCR1_SClkIactL | SSCR1_SClk1P | SSCR1_ExtClk);

#ifdef CONFIG_SA1100_VICTOR
    /* Left and right channels aren't correctly synchronized at start.
     * We send a single dummy sample to  swap them so future samples will
     * be OK.
     */
    Ser4SSDR = 0;
#endif
}


int __init audio_sa1100_ssp_init( void )
{
    int err;
   
    /* register device */
    if(( err = register_chrdev( AUDIO_MAJOR, AUDIO_NAME, &audio_fops )) != 0)
    {
	/* log error and fail */
	printk(AUDIO_NAME ": unable to register major device %d\n", AUDIO_MAJOR);
	return(err);
    }

    audio_ssp_init();

    if( (err = request_irq( AUDIO_IRQ, audio_irq, SA_INTERRUPT, "audio", NULL)) != 0) {
	printk( AUDIO_NAME ": unable to acquire irq %d\n", AUDIO_IRQ );
	unregister_chrdev( AUDIO_MAJOR, AUDIO_NAME );
	return err;
    }

    /* initialize DMA */
    audio_task.routine = audio_feed_dma;
    ClrDCSR0 = (DCSR_DONEA|DCSR_DONEB|DCSR_STRTA|DCSR_STRTB|DCSR_IE|DCSR_ERROR|DCSR_RUN);
    DDAR0 = DDAR_Ser4SSPWr;

    printk( AUDIO_NAME_VERBOSE " initialized\n" );

    return(0);
}


#ifdef  MODULE

int init_module( void )
{
#ifdef DEBUG
    printk(AUDIO_NAME ": init_module\n");
#endif

    return( audio_sa1100_ssp_init() );
}


void cleanup_module( void )
{
    /* unregister driver and IRQ */
    free_irq( AUDIO_IRQ, NULL );
    unregister_chrdev( AUDIO_MAJOR, AUDIO_NAME );

    /* log device unload */
    printk(AUDIO_NAME_VERBOSE " unloaded\n");
}

#endif

