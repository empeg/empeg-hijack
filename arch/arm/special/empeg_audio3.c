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

/* For the userspace interface */
#include <linux/empeg.h>

#include "empeg_dsp.h"
#include "empeg_dsp_i2c.h"
#include "empeg_audio3.h"
#include "empeg_mixer.h"

#ifdef CONFIG_EMPEG_DAC
#error empeg DAC driver cannot be coexist with DSP driver
#endif

/* options */
#define OPTION_DEBUGHOOK		0

/* debug */
#define AUDIO_DEBUG			0
#define AUDIO_DEBUG_VERBOSE		0
#define AUDIO_DEBUG_STATS		1 //AUDIO_DEBUG | AUDIO_DEBUG_VERBOSE

/* Names */
#define AUDIO_NAME			"audio-empeg"
#define AUDIO_NAME_VERBOSE		"empeg dsp audio"

/* interrupt numbers */
#define AUDIO_IRQ			IRQ_DMA0 /* DMA channel 0 IRQ */

/* Client parameters */
#define AUDIO_NOOF_BUFFERS		8	/* Number of audio buffers */
#define AUDIO_BUFFER_SIZE		4608	/* User buffer chunk size */

/* Number of audio buffers that can be in use at any one time. This is
   two less since the inactive two are actually still being used by
   DMA while they look like being free. */
#define MAX_FREE_BUFFERS		(AUDIO_NOOF_BUFFERS - 2)


#ifndef __KERNEL__
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif


/* Volume adjustment
 * By richard.lovejoy@ericsson.com.au
*/

/*
 * The interesting stuff in this file is voladj_*
 * The main function gives an indication of how to use
 * the functions.  Basically, call voladj_init with the arguments
 * you want, and then pass the address of the returned structure
 * into all subsequent calls.
 *
 * To look ahead, call voladj_check() on the future buffer, and then
 * voladj_scale on the current buffer.
 *
 * To not look ahead, call voladj_check() on the current buffer, and
 * then voladj_scale on the current buffer.
 *
 * If we decide that the look ahead doesn't improve the sound at all,
 * then we can pretty much remove voladj_check, and it becomes
 * even more trivial.
 * 
 * To figure out the desired multiplier, first we figure out the
 * desired output from the given input, and then calculate the
 * multiplier that would give that output.
 *
 * Currently, we use
 *   desired_out = exp( log( input ) / constant )
 * which has the property that 
 *   if in_a:in_b == in_b:in_c
 *   then out_a:out_b == out_b:out_c
 * i.e.  The dynamic range is stretched, but not otherwise distorted.
 *
 * To calculate the constant above, we use the fact that we
 * have a specified minimum output volume, and a specified
 * minimum input volume to scale to that value.
 *
 * This gives us:
 *   exp( log( fake_silence ) / factor ) = minvol;
 * or
 *   factor = log( fake_silence ) / log( minvol );
 *
 * Simply from our fixed point routines, we require that
 *   minvol / fake_silence < 1 << MULT_INTBITS;
 * Otherwise, we can't make our multiplier big enough!
 */ 

#define AUDIO_FILLSZ   4608
#define BLOCKSIZE   AUDIO_FILLSZ

#define MAXSAMPLES 32767

#define MULT_POINT 12
#define SHORT_FRAC_POINT MULT_POINT
#define SHORT_FRAC_VAL ((double)(1 << SHORT_FRAC_POINT))
#define MULT_INTBITS (30 - (MULT_POINT + SHORT_FRAC_POINT))

#define MULT_TO_16 (MULT_POINT + MULT_INTBITS - 16)
#define RESULT_SHIFT MULT_POINT

struct voladj_state {
  int output_multiplier;
  int desired_multiplier;
  int buf_size;
  short real_silence;
  short fake_silence;
  short log_scale;
  short headroom;
  short minvol;
  short increase;
  short decrease;
  unsigned short max_sample;
};

int voladj_exp(int inval) {
  int result = (1 << MULT_POINT);
  int i = 1;
  int curval = (1 << MULT_POINT);
  for (i = 1; curval != 0; i++) {
    curval = ((curval * inval) / i) >> RESULT_SHIFT;
    result += curval;
  }
  /*
  printf("expiters: %d\n",i);
  */
  return result;
}

/*
 * These two define ln(2) and 1/ln(2) in our fixed point scheme.
 * They are used because then it's much easier to scale our
 * input number for voladj_log() to a number close to 1
 *
 */
#define LOGTWO (0x0000b172 >> (16 - MULT_POINT))
#define INVLOGTWO (0x00017154 >> (16 - MULT_POINT))

int voladj_log(int inval) {
  int aftercount = 0;

  int result = 0;
  int x = 0;
  int i = 1;
  int curval = 1 << MULT_POINT;

  while (inval < (1 << (MULT_POINT - 1)) ) {
    inval = inval << 1;
    aftercount ++;
  }
  while (inval > (1 << MULT_POINT)) {
    inval = inval >> 1;
    aftercount --;
  }
    /*
    printf("inval: %d\n",inval);
    */

  x = (1 << MULT_POINT) - inval;
  for (i = 1; curval != 0; i++) {
    result += ((curval * x) / i) >> RESULT_SHIFT;
    curval = (curval * x) >> RESULT_SHIFT;
  }
  /*
  printf("logiters: %d\n",i);
  */
  result = ((((result * INVLOGTWO) >> RESULT_SHIFT) + 
    (aftercount << MULT_POINT)) * LOGTWO) >> RESULT_SHIFT;
  result = -1 * result;
  return result;
}



/*
 * Initialise all our stuff.  
 * This converts easy to understand parameters into easy to use ones.
 * It also attempts to fix any parameters that are set badly, given
 * our range of precision.
 *
        voladj_intinit( &(dev->voladj),
            AUDIO_BUFFER_SIZE,              buffersize
            ((1 << MULT_POINT) * 2),        factor_per_second 
            ((1 << MULT_POINT) / 10),       minvol
            (((1 << MULT_POINT) * 3) / 4),  headroom
            30,                             real_silence
            80                              fake_silence
            );
 */


struct voladj_state* voladj_intinit( 
  struct voladj_state *initial,

  int buf_size,         /* (Fixed) size of blocks in bytes to process */
  int factor_per_second, /* Maximum rate of volume change
                            (ratio/sec << MULT_POINT) */
  int minvol,        /* Minimum volume to attempt to maintain 
                        (0 - 1 << MULT_POINT) */
  int headroom,       /* Headroom multiplier 
                         (0 - 1 << MULT_POINT) */
  int real_silence,   /* Threshold below which we gradually return to normal 
                         (Number of samples) */
  int fake_silence   /* Threshold below which we do no further scaling 
                        (Number of samples) */
  ) {

  int log_scale;
  int temp_fake;
  unsigned long flags;

  save_flags_cli(flags);

  initial->output_multiplier = 0x1 << MULT_POINT;
  initial->desired_multiplier = initial->output_multiplier;
  initial->buf_size = buf_size;
  initial->headroom = 3 << (SHORT_FRAC_POINT - 2);
  initial->real_silence = real_silence;
  initial->fake_silence = fake_silence;

  initial->increase = voladj_exp( (voladj_log( factor_per_second )
        *buf_size) / (4 * 44100));
  initial->decrease = (1 << (MULT_POINT * 2)) / initial->increase;

  temp_fake = (MAXSAMPLES * minvol) >> (MULT_INTBITS + MULT_POINT);
  if (initial->fake_silence < temp_fake) {
    initial->fake_silence = temp_fake;
  }

  if (minvol > (1 << SHORT_FRAC_POINT)) {
    initial->minvol = 1 << SHORT_FRAC_POINT;
  } else if (minvol < 0) {
    initial->minvol = 0;
  } else {
    initial->minvol = minvol;
  }
  if (initial->minvol == 0) {
    initial->log_scale = (1 << MULT_POINT);
    log_scale = 1.0;
  } else {
    log_scale = -1 * (voladj_log( (MAXSAMPLES << MULT_POINT) / 
        initial->fake_silence ) << MULT_POINT)
      / voladj_log( minvol );
    initial->log_scale = log_scale;
  }

  if (headroom > (1 << SHORT_FRAC_POINT)) {
    initial->headroom = 1 << SHORT_FRAC_POINT;
  } else if (headroom < 0) {
    initial->headroom = 0;
  } else {
    initial->headroom = headroom;
  }

  initial->max_sample = 0;
  restore_flags(flags);

  /*
  fprintf(stderr, 
      "minvol: %x headroom: %x increase: %x decrease %x\n", 
      initial->minvol, initial->headroom, initial->increase, initial->decrease);
  fprintf(stderr, 
      "MULT_TO_16: %d MULT_POINT: %d MULT_INTBITS: %d SHORT_FRAC_VAL: %f \n", 
      MULT_TO_16, MULT_POINT, MULT_INTBITS, SHORT_FRAC_VAL);
  fprintf(stderr, 
      "fake_silence: %d LOG_SCALE: %d \n", 
      initial->fake_silence, log_scale >> MULT_POINT );
  fprintf(stderr, 
      "factor_per_second: %x minvol: %x fake: %x log(minvol) %x\n", 
      factor_per_second, initial->minvol, initial->fake_silence, voladj_log( (MAXSAMPLES << MULT_POINT) / initial->fake_silence ));
  */

  return initial;

};


/*
 * Here we figure out what multiplier we want, based on the minimum
 * volume that we are aiming for, and the maximum sample that we
 * can see.
 */

int voladj_get_multiplier( struct voladj_state *state, 
    unsigned short max_sample ) {
  int max_frac,desired_out, desired_mult;
  max_frac = (max_sample << MULT_POINT) / MAXSAMPLES;
  if (max_frac == 0) max_frac = 1;
  if (max_sample == 0) max_sample = 1;
  /*
   * This is the old linear way
  desired_out = ((int)(state->minvol) * (int)MAXSAMPLES)  +
    (((int)max_frac) * ( (1 << SHORT_FRAC_POINT) - (int)(state->minvol) )) ;
  */
  desired_out = MAXSAMPLES * 
    voladj_exp( (voladj_log( max_frac ) << MULT_POINT) / state->log_scale);
  desired_mult = desired_out / max_sample ;
  desired_mult = (desired_mult  * (int)state->headroom) >> RESULT_SHIFT;
  if (desired_mult > (1 << (MULT_POINT + MULT_INTBITS))) {
    desired_mult = (1 << (MULT_POINT + MULT_INTBITS));
  }
  if (desired_mult < (1 << MULT_POINT)) {
    desired_mult = (1 << MULT_POINT);
  }
  return desired_mult;
}

/* 
 * This bit of code searches for any samples that will cause clipping
 * in the future.  The reason we read ahead is so that we never
 * have to do abrupt volume changes.  I'm not so sure that this is
 * necessary, because even gradual volume changes over 4k of data
 * are pretty abrupt to the ear.  Still, I have this nagging feeling
 * that suddenly changing the scaling factor from 100 to 1 would
 * produce some high frequency components.
*/

int voladj_check( 
  struct voladj_state *state, 
  short *lookaheadbuf ) {

  int outmult;
  int desired_multiplier;
  int upmult;
  int downmult;
  unsigned short max_sample;
  unsigned short cur_sample;
  unsigned short max_la_sample;

  int num_samples = state->buf_size / 2;
  int i, outputvalue;

  max_la_sample = 0;
  for( i = 0; i < num_samples; i++ ) {
    cur_sample = abs( lookaheadbuf[ i ] );
    if (cur_sample > max_la_sample) {
      max_la_sample = cur_sample;
    }
  }

  max_sample = max_la_sample;
  if (state->max_sample > max_sample) {
    max_sample = state->max_sample;
  }
  
  outmult = state->output_multiplier;
  upmult = (outmult * state->increase) >> RESULT_SHIFT;
  downmult = (outmult * state->decrease) >> RESULT_SHIFT;

  outputvalue = ( ( (upmult >> MULT_TO_16 ) * 
    max_sample ) >>  (MULT_POINT - MULT_TO_16 ) );

  desired_multiplier = 0;

  if (outputvalue > MAXSAMPLES ) {
    desired_multiplier = (((MAXSAMPLES + 1) << MULT_POINT) / max_sample);
    /*
    fprintf(stderr, 
      "avoiding clipping, output_multiplier: %x, new desired_multiplier: %x, max_sample: %x\n", 
      state->output_multiplier, desired_multiplier, max_sample);
    */
  } else if (max_sample < state->real_silence) {
    desired_multiplier = downmult;
    if (desired_multiplier < (1 << MULT_POINT)) {
      desired_multiplier = (1 << MULT_POINT);
    }
  } else if (max_sample < state->fake_silence) {
    max_sample = state->fake_silence;
  }
  if (desired_multiplier == 0) {
    desired_multiplier = voladj_get_multiplier( state, max_sample );
    if ((desired_multiplier > (1 << (MULT_POINT + MULT_INTBITS)))) {
      desired_multiplier = downmult;
    } else {
      if (desired_multiplier > upmult) {
        desired_multiplier = upmult;
      }
      if (desired_multiplier < downmult) {
        desired_multiplier = downmult;
      }
    }
  }

  /*
  printk("sampes prv: %x la: %x comb: %x desmult: %x\n",
      state->max_sample, max_la_sample, max_sample, desired_multiplier);
  printk("up: %x down: %x outval: %x\n",
      state->increase, state->decrease, outputvalue);
      */


  state->desired_multiplier = desired_multiplier;
  state->max_sample = max_la_sample;

  return desired_multiplier;
}

void voladj_scale( 
  struct voladj_state *state, 
  int desired_multiplier,
  short *scalebuf 
  ) {

  int outmult = state->output_multiplier;
  int output_value,i;
  short output_sample;
  int num_samples = state->buf_size / 2;

  /* If there is no scaling to be done, just return immediately.
   * There's no point going through multiplying every sample by 1!
   */
  if ((outmult == (1 << MULT_POINT)) &&
    (desired_multiplier == (1 << MULT_POINT))) {
      return;
  }

  /*
   * In the previous call to voladj_check we made sure that the
   * output multiplier was set to a value that will not cause
   * clipping for any of the samples, so we don't have to worry
   * about that here.
   */

  for( i = 0; i < num_samples; i++ ) {
    if (desired_multiplier != outmult) {
      outmult = outmult + 
        ((desired_multiplier - outmult) / (num_samples - i));
    } 
    output_value = ((outmult >> MULT_TO_16) 
      * scalebuf[ i ]) >> (MULT_POINT - MULT_TO_16 ) ;

    /*
    if ((output_value > MAXSAMPLES)||(output_value < (-1 * (MAXSAMPLES+1)))) {
      fprintf(stderr, 
        "Arghh! Got clipping, output_multiplier: %x, insamp: %x, outsamp %x\n", 
        outmult, scalebuf[i], output_value);
      fprintf(stderr, 
        "  output_multiplier: %x, abs(insamp): %x, abs(outsamp) %x\n", 
        outmult, abs(scalebuf[i]), abs(output_value));
    }
    */
    output_sample = (short)(0x0000ffff & output_value);
    scalebuf[ i ] = output_sample;
  }

  state->output_multiplier = outmult;
}

/* statistics */
typedef struct
{
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

typedef struct
{
	/* Buffers */
	audio_buf *buffers;
	int used,free,head,tail,prevhead;

        /* current state of volume adjuster */
        struct voladj_state voladj;   

	/* Buffer management */
	struct wait_queue *waitq;

	/* Statistics */
	audio_stats stats;

	/* beep timeout */
	struct timer_list beep_timer;

	/* Are we sending "good" data? */
	int good_data;
} audio_dev;

/* cosine tables for beep parameters */
static unsigned long csin_table_44100[];
static unsigned long csin_table_38000[];
static unsigned long *csin_table = csin_table_44100;
/* setup stuff for the beep coefficients (see end of file) */
static dsp_setup beep_setup[];

/* Devices in the system; just the one channel at the moment */
static audio_dev 	audio[1];

static struct proc_dir_entry *proc_audio;

/* Lots of function things */
int __init empeg_audio_init(void);
static int empeg_audio_write(struct file *file,
			     const char *buffer, size_t count, loff_t *ppos);
static int empeg_audio_purge(audio_dev *dev);
static int empeg_audio_ioctl(struct inode *inode, struct file *file,
			     uint command, ulong arg);

static void empeg_audio_beep(audio_dev *dev,
			     int pitch, int length, int volume);
static void empeg_audio_beep_end(unsigned long);
static void empeg_audio_beep_end_sched(void *unused);
static void empeg_audio_interrupt(int irq, void *dev_id, struct pt_regs *regs);
static void empeg_audio_emit_action(void *);
#ifdef	CONFIG_PROC_FS
static int empeg_audio_read_proc(char *buf, char **start, off_t offset,
				 int length, int *eof, void *private);
#endif

static struct tq_struct emit_task =
{
	routine:	empeg_audio_emit_action
};

static struct tq_struct i2c_queue =
{
	routine:	empeg_audio_beep_end_sched
};

static struct file_operations audio_fops =
{
	write:		empeg_audio_write,
	ioctl:		empeg_audio_ioctl,
	open:		empeg_audio_open,
};

void hijack_voladj_intinit (	int factor_per_second, int minvol, int headroom, int real_silence, int fake_silence)
{
	(void)voladj_intinit(&audio[0].voladj, AUDIO_BUFFER_SIZE, factor_per_second, minvol, headroom, real_silence, fake_silence);
}

void hijack_beep (int pitch, int duration_msecs, int vol_percent)
{
	(void)empeg_audio_beep(&audio[0], pitch, duration_msecs, vol_percent); // parameters:  dev, pitch:48-96, duration:msec, vol:0-100
}

int __init empeg_audio_init(void)
{
	int i, err;
	audio_dev *dev = &audio[0];
	
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_sa1100_init\n");
#endif

	/* Blank everything to start with */
	memset(dev, 0, sizeof(audio_dev));
	
	/* Allocate buffers */
	if ((dev->buffers = kmalloc(sizeof(audio_buf) * AUDIO_NOOF_BUFFERS,
				    GFP_KERNEL)) == NULL) {
		/* No memory */
		printk(AUDIO_NAME ": can't get memory for buffers");
		return -ENOMEM;
	}

	/* Clear them */
	for(i = 0; i < AUDIO_NOOF_BUFFERS; i++)
		dev->buffers[i].count = 0;

        /* Initialise volume adjustment */
        voladj_intinit( &(dev->voladj),
            AUDIO_BUFFER_SIZE,              /* buffersize */
            ((1 << MULT_POINT) * 2),        /* factor_per_second */
            ((1 << MULT_POINT) / 10),       /* minvol */
            (((1 << MULT_POINT) * 4) / 4),  /* headroom */
            30,                             /* real_silence */
            80                              /* fake_silence */
            );

	/* Set up queue: note that two buffers could be DMA'ed any any time,
	   and so we use two fewer marked as "free" */
	dev->head = dev->tail = dev->used = 0;
	dev->free = MAX_FREE_BUFFERS;

	/* Request appropriate interrupt line */
	if((err = request_irq(AUDIO_IRQ, empeg_audio_interrupt, SA_INTERRUPT,
			      AUDIO_NAME,NULL)) != 0) {
		/* fail: unable to acquire interrupt */
		printk(AUDIO_NAME ": request_irq failed: %d\n", err);
		return err;
	}

	/* Setup I2S clock on GAFR */
	GAFR |= GPIO_GPIO19;

	/* Setup SSP */
	Ser4SSCR0 = 0x8f; //SSCR0_DataSize(16)|SSCR0_Motorola|SSCR0_SSE;
	Ser4SSCR1 = 0x30; //SSCR1_ECS|SSCR1_SP;
	Ser4SSSR = SSSR_ROR; /* ...baby one more time */

	/* Start DMA: Clear bits in DCSR0 */
	ClrDCSR0 = DCSR_DONEA | DCSR_DONEB | DCSR_IE | DCSR_RUN;
	
	/* Initialise DDAR0 for SSP */
	DDAR0 = 0x81c01be8;

	/* Start both buffers off with zeros */
	DBSA0 = (unsigned char*) _ZeroMem;
	DBTA0 = AUDIO_BUFFER_SIZE;
	DBSB0 = (unsigned char*) _ZeroMem;
	DBTB0 = AUDIO_BUFFER_SIZE;
	SetDCSR0 = DCSR_STRTA | DCSR_STRTB | DCSR_IE | DCSR_RUN;

#ifdef	CONFIG_PROC_FS
	/* Register procfs devices */
	proc_audio = create_proc_entry("audio", 0, 0);
	if (proc_audio)
		proc_audio->read_proc = empeg_audio_read_proc;
#endif	/* CONFIG_PROC_FS */
	
	/* Log device registration */
	printk(AUDIO_NAME_VERBOSE " initialised\n");

	/* beep timeout */
	init_timer(&dev->beep_timer);
	dev->beep_timer.data = 0;
	dev->beep_timer.function = empeg_audio_beep_end;

	/* Everything OK */
	return 0;
}

int empeg_audio_open(struct inode *inode, struct file *file)
{
	file->f_op = &audio_fops;

#if AUDIO_DEBUG
	printk(AUDIO_NAME ": audio_open\n");
#endif

	/* Make sure old EQ settings apply */
	empeg_mixer_eq_apply();

        return 0;
}

static int empeg_audio_write(struct file *file,
			     const char *buffer, size_t count, loff_t *ppos)
{
	audio_dev *dev = &audio[0];
	int total = 0;
	int ret;
        int thisbufind=0;
	
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_write: count=%d\n", count);
#endif

	/* Check the user isn't trying to murder us */
	if((ret = verify_area(VERIFY_READ, buffer, count)) != 0)
		return ret;
	
	/* Count must be a multiple of the buffer size */
	if (count % AUDIO_BUFFER_SIZE) {
	        printk("non-4608 byte write (%d)\n", count);
		return -EINVAL;
	}

	if (count == 0) {
		printk("zero byte write\n");
		return 0;
	}

	/* Any space left? (No need to disable IRQs: we're just checking for a
	   full buffer condition) */
	/* This version doesn't have races, see p209 of Linux Device Drivers */
	if (dev->free == 0) {
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
	while(count > 0 && dev->free > 0) {
		unsigned long flags;
                int multiplier;
		extern void hijack_voladj_update_history(int);
		extern int  hijack_voladj_enabled;

		/* Critical sections kept as short as possible to give good
		   latency for other tasks */
		save_flags_cli(flags);
		dev->free--;
		restore_flags(flags);

                thisbufind = dev->head;
		/* Copy chunk of data from user-space. We're safe updating the
		   head when not in cli() as this is the only place the head
		   gets twiddled */

                dev->head++;
		if (dev->head == AUDIO_NOOF_BUFFERS)
			dev->head = 0;

		copy_from_user(dev->buffers[thisbufind].data,buffer,AUDIO_BUFFER_SIZE);
		total += AUDIO_BUFFER_SIZE;
		/* Oops, we missed this in previous versions */
		buffer += AUDIO_BUFFER_SIZE;
		dev->stats.samples += AUDIO_BUFFER_SIZE;
		count -= AUDIO_BUFFER_SIZE;

              if (hijack_voladj_enabled) {
                multiplier = voladj_check( &(dev->voladj), 
                        (short *) (dev->buffers[thisbufind].data) );
              } else {
                multiplier = (1 << MULT_POINT);
              }
              dev->voladj.desired_multiplier = multiplier;
              hijack_voladj_update_history(multiplier);
#if AUDIO_DEBUG_VERBOSE
	printk("mults: des=%x,out=%x\n", dev->voladj.desired_multiplier, dev->voladj.output_multiplier);
#endif


		save_flags_cli(flags);
              if (hijack_voladj_enabled) {
                if (dev->used > 1) {
                    dev->used--;
                    restore_flags(flags);
                    voladj_scale( &(dev->voladj), 
                        dev->voladj.desired_multiplier,
                        (short *) (dev->buffers[ dev->prevhead ].data) );
                    save_flags_cli(flags);
                    dev->used++;
                } else {
                    dev->voladj.output_multiplier = 1 << MULT_POINT;
                }
              }

		/* Now the buffer is ready, we can tell the IRQ section
		   there's new data */
		dev->used++;
		restore_flags(flags);

                dev->prevhead = thisbufind;
	}

	/* Update hwm */
	if (dev->used > dev->stats.buffer_hwm)
		dev->stats.buffer_hwm=dev->used;

	/* We have data (houston) */
	dev->good_data = 1;

	/* Write complete */
	return total;
}

/* Throw away all complete blocks waiting to go out to the DAC and return how
   many bytes that was. */
static int empeg_audio_purge(audio_dev *dev)
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

static int empeg_audio_ioctl(struct inode *inode, struct file *file,
			     uint command, ulong arg)
{
	audio_dev *dev = &audio[0];

	switch (command) {
	case EMPEG_DSP_BEEP:
	{
		int pitch, length, volume;
		int *ptr = (int *)arg;
		get_user_ret(pitch, ptr, -EFAULT);
		get_user_ret(length, ptr + 1, -EFAULT);
		get_user_ret(volume, ptr + 2, -EFAULT);
		empeg_audio_beep(dev, pitch, length, volume);
		return 0;
	}
	
	case EMPEG_DSP_PURGE:
	{
		int bytes = empeg_audio_purge(dev);
		put_user_ret(bytes, (int *)arg, -EFAULT);
		return 0;		
	}
	case EMPEG_DSP_GRAB_OUTPUT:
	{
	        int pretail = dev->tail - 1;
	        if( pretail < 0 )
	            pretail += AUDIO_NOOF_BUFFERS;

		return copy_to_user((char *) arg,
		                    dev->buffers[pretail].data,
				    AUDIO_BUFFER_SIZE);
        }	
	}

	/* invalid command */
	return -EINVAL;
}

static void empeg_audio_beep(audio_dev *dev, int pitch, int length, int volume)
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
		vat = 0xfff - empeg_mixer_get_vat();

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

static void empeg_audio_beep_end(unsigned long unused)
{
	/* We don't want to be doing this from interrupt time */
	/* Schedule back to process time -- concurrency safe(ish) */
	queue_task(&i2c_queue, &tq_scheduler);
}	

static void empeg_audio_beep_end_sched(void *unused)
{
	/* This doesn't handle all cases */
	/* if another thing timed, we should keep the beep on, really */
	if(timer_pending(&audio[0]. beep_timer)) return;

	/* Turn beep off */
#if AUDIO_DEBUG
	/* This all works now, I'm pretty sure */
	printk(AUDIO_NAME ": BEEP off.\n");
#endif

	/* Turn off oscillator */
	dsp_write(Y_sinusMode, 0x89a);
}

/*                      
 * Interrupt processing 
 */
static void empeg_audio_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	audio_dev *dev = &audio[0];
	int status = RdDCSR0, dofirst = -1;

	/* Update statistics */
#if AUDIO_DEBUG_STATS
	dev->stats.interrupts++;
#endif

	/* Work out which DMA buffer we need to attend to first */
	dofirst = ( ((status & DCSR_BIU) && (status & DCSR_STRTB)) ||
		    (!(status & DCSR_BIU) && !(status & DCSR_STRTA)))
		? 0 : 1;

	/* Fill the first buffer */
        if (dofirst== 0) {
		ClrDCSR0 = DCSR_DONEB;
		
		/* Any data to get? */
		if (dev->used == 0) {
			DBSA0 = (unsigned char *) _ZeroMem;
			DBTA0 = AUDIO_BUFFER_SIZE;


			/* If we've underrun, take note */
			if (dev->good_data) {
				dev->good_data = 0;
				dev->stats.user_underruns++;
			}
		}
		else {
		        DBSA0 =	(unsigned char *)
				virt_to_phys(dev->buffers[dev->tail].data);
			DBTA0 = AUDIO_BUFFER_SIZE;
			if (++dev->tail == AUDIO_NOOF_BUFFERS) dev->tail = 0;
			dev->used--;
			dev->free++;
		}
		
		if (!(status & DCSR_STRTB)) {
			/* Filling both buffers: possible IRQ underrun */
			dev->stats.irq_underruns++;

			if (dev->used == 0) {
				DBSB0 = (unsigned char *) _ZeroMem;
				DBTB0 = AUDIO_BUFFER_SIZE;
			}
			else {
				DBSB0 = (unsigned char *) virt_to_phys(
					dev->buffers[dev->tail].data);
				DBTB0 = AUDIO_BUFFER_SIZE;
				if (++dev->tail == AUDIO_NOOF_BUFFERS)
					dev->tail = 0;
				dev->used--;
				dev->free++;
			}
			
			/* Start both channels */
			SetDCSR0 =
				DCSR_STRTA | DCSR_STRTB | DCSR_IE | DCSR_RUN;
		}
		else {
			SetDCSR0 = DCSR_STRTA | DCSR_IE | DCSR_RUN;
		}
	}
	else {
		ClrDCSR0 = DCSR_DONEA;

		/* Any data to get? */
		if (dev->used == 0) {
			DBSB0 = (unsigned char *) _ZeroMem;
			DBTB0 = AUDIO_BUFFER_SIZE;

			/* If we've underrun, take note */
			if (dev->good_data) {
				dev->good_data = 0;
				dev->stats.user_underruns++;
			}
		}
		else {
			DBSB0 = (unsigned char *)
				virt_to_phys(dev->buffers[dev->tail].data);
			DBTB0 = AUDIO_BUFFER_SIZE;
			if (++dev->tail == AUDIO_NOOF_BUFFERS)
				dev->tail=0;
			dev->used--;
			dev->free++;
		}
		
		if (!(status & DCSR_STRTA)) {
			/* Filling both buffers: possible IRQ underrun */
			dev->stats.irq_underruns++;

			if (dev->used == 0) {
				DBSA0 = (unsigned char *) _ZeroMem;
				DBTA0 = AUDIO_BUFFER_SIZE;
			}
			else {
				DBSA0 = (unsigned char*) virt_to_phys(
					dev->buffers[dev->tail].data);
				DBTA0 = AUDIO_BUFFER_SIZE;
				if (++dev->tail == AUDIO_NOOF_BUFFERS)
					dev->tail=0;
				dev->used--;
				dev->free++;
			}

			/* Start both channels */
			SetDCSR0 =
				DCSR_STRTA | DCSR_STRTB | DCSR_IE | DCSR_RUN;
		}
		else {
			SetDCSR0 = DCSR_STRTB | DCSR_IE | DCSR_RUN;
		}
	}

	/* Run the audio buffer emmitted action */
	queue_task(&emit_task, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
	
	/* Wake up waiter */
	wake_up_interruptible(&dev->waitq);
}

static void empeg_audio_emit_action(void *p)
{
#ifdef CONFIG_EMPEG_DISPLAY
	audio_emitted_action();
#endif
}

void empeg_audio_beep_setup(int rate)
{
	/* Page 156 */
    
	/* Setup beep coefficients for this sampling frequency */
	if(rate == 38000) {
		csin_table = csin_table_38000;
	    
		// 6ms rise/fall time, 30ms transient
		dsp_patchmulti(beep_setup, Y_samAttl, 0x312);
		dsp_patchmulti(beep_setup, Y_samAtth, 0x7dc);
		dsp_patchmulti(beep_setup, Y_samDecl, 0x312);
		dsp_patchmulti(beep_setup, Y_samDech, 0x7dc);
		dsp_patchmulti(beep_setup, Y_deltaA, 0x10e);
		dsp_patchmulti(beep_setup, Y_switchA, 0x10e);
		dsp_patchmulti(beep_setup, Y_deltaD, 0);
		dsp_patchmulti(beep_setup, Y_switchD, 0);
	}
	else if(rate == 44100) {
		csin_table = csin_table_44100;
	    
		// 6 ms rise/fall time, 30ms transient
		dsp_patchmulti(beep_setup, Y_samAttl, 0x22f);
		dsp_patchmulti(beep_setup, Y_samAtth, 0x7e1);
		dsp_patchmulti(beep_setup, Y_samDecl, 0x22f);
		dsp_patchmulti(beep_setup, Y_samDech, 0x7e1);
		dsp_patchmulti(beep_setup, Y_deltaA, 0x0fb);
		dsp_patchmulti(beep_setup, Y_switchA, 0x0fb);
		dsp_patchmulti(beep_setup, Y_deltaD, 0);
		dsp_patchmulti(beep_setup, Y_switchD, 0);
	}
	else {
		printk(AUDIO_NAME
		       ": unsupported rate for beeps: %d\n", rate);
	}

	dsp_writemulti(beep_setup);
}

#ifdef	CONFIG_PROC_FS
static struct proc_dir_entry *proc_audio;
static int empeg_audio_read_proc(char *buf, char **start, off_t offset,
				 int length, int *eof, void *private )
{
	audio_dev *dev = &audio[0];

	length = 0;
	length += sprintf(buf + length,
			  "samples   : %ld\n"
			  "interrupts: %ld\n"
			  "wakeups   : %ld\n"
			  "fifo errs : %ld\n"
			  "buffer hwm: %ld\n"
			  "usr undrrn: %ld\n"
			  "irq undrrn: %ld\n",
			  dev->stats.samples,
			  dev->stats.interrupts,
			  dev->stats.wakeups,
			  dev->stats.fifo_err,
			  dev->stats.buffer_hwm,
			  dev->stats.user_underruns,
			  dev->stats.irq_underruns);
	
	return length;
}
#endif	/* CONFIG_PROC_FS */


static unsigned long csin_table_44100[] =
{
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

static unsigned long csin_table_38000[] =
{
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

static dsp_setup beep_setup[] =
{
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
