/*
 * SA1100/empeg mixer device driver
 *
 * (C) 2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Hugo Fiennes, <hugo@empeg.com>
 *   John Ripley, <john@empeg.com>
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

#include <asm/arch/hijack.h>
#include <linux/empeg.h>

#ifdef	CONFIG_PROC_FS
#include <linux/stat.h>
#include <linux/proc_fs.h>
#endif

#include "empeg_dsp.h"
#include "empeg_dsp_i2c.h"
#include "empeg_audio3.h"
#include "empeg_mixer.h"

#ifdef CONFIG_EMPEG_DAC
#error empeg DAC driver cannot be coexist with DSP driver
#endif

#define MIXER_DEBUG			0
#define RADIO_DEBUG			0
#define EQ_DEBUG			0
#define HIJACK_VOLBOOST_DEBUG		0

#define MIXER_NAME			"mixer-empeg"
#define MIXER_NAME_VERBOSE		"empeg dsp mixer"

/* Defaults */
#define MIXER_DEVMASK			SOUND_MASK_VOLUME
#define MIXER_STEREODEVS		MIXER_DEVMASK

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

#define LOUDNESS_TABLE_SIZE		11
#define BALANCE_TABLE_SIZE		15
#define BALANCE_ZERO			7
#define	FADE_TABLE_SIZE			15
#define FADE_ZERO			7

/* The level that corresponds to 0dB */
#define VOLUME_ZERO_DB			90

static volume_entry volume_table[101];
static loudness_entry loudness_table[LOUDNESS_TABLE_SIZE];
static balance_entry balance_table[BALANCE_TABLE_SIZE];
static fade_entry fade_table[FADE_TABLE_SIZE];
static struct empeg_eq_section_t eq_init[20];
static struct empeg_eq_section_t eq_current[20];
static int eq_section_order[20];
static int mixer_compression = 0;
static unsigned int eq_last[40];
static unsigned int eq_reg_last = 0;
static unsigned int radio_sensitivity;
static int radio_fm_level_p1 = 512, radio_fm_level_q1 = 0;
static int radio_fm_deemphasis = 50;
       int hijack_current_mixer_input;
       int hijack_current_mixer_volume;
extern int hijack_volboost[];
static int hijack_current_vol_request;
static int hijack_current_vol_boost;
extern int hijack_disable_bassboost_FM;
static struct empeg_eq_section_t hijack_eq_real[20];
static int eq_hijacked = 0;   
/* stereo detect */
static unsigned stereo_level = 0;
static unsigned sampling_rate = 44100;

static mixer_dev	mixer_global;

static int empeg_mixer_release(struct inode *inode, struct file *file);
static int empeg_mixer_ioctl(struct inode *inode, struct file *file,
			     uint command, ulong arg);

       void empeg_mixer_select_input(int input);
static void empeg_mixer_setloudness(mixer_dev *dev, int level);
static int empeg_mixer_setvolume(mixer_dev *dev, int vol);
static int empeg_mixer_getdb(mixer_dev *dev);
static void empeg_mixer_inflict_flags(mixer_dev *dev);
static void empeg_mixer_setbalance(mixer_dev *dev, int balance);
static int empeg_mixer_getloudnessdb(mixer_dev *dev);
static void empeg_mixer_setbalance(mixer_dev *dev, int balance);
static void empeg_mixer_setfade(mixer_dev *dev, int fade);
static void empeg_mixer_mute(int on);
static void empeg_mixer_eq_reset(void);
static void empeg_mixer_eq_set(struct empeg_eq_section_t *sections);
static void empeg_mixer_set_fm_level(unsigned int p1, unsigned int q1);
static int empeg_mixer_get_fm_noise(void);
static int empeg_mixer_set_fm_deemphasis(int which);
static int hijack_volume_boost_w(int);
static int hijack_volume_boost_r(int);
static void hijack_volume_boost_reapply(mixer_dev *dev);
static void empeg_mixer_set_sampling_rate(mixer_dev *dev, unsigned rate);
extern void hijack_tone_init(void); // hijack.c

static struct file_operations mixer_fops =
{
	ioctl:		empeg_mixer_ioctl,
	open:		empeg_mixer_open,
	release:	empeg_mixer_release
};

int __init empeg_mixer_init(void)
{
	mixer_dev *dev = &mixer_global;
    
#if MIXER_DEBUG
	printk(MIXER_NAME ": mixer_init\n");
#endif
	memset((void *) dev, 0, sizeof(mixer_dev));
	
	if(empeg_hardwarerevision() < 6)
		radio_sensitivity = 0;
	else
/*		radio_sensitivity = 2; */
		/*
		 * actually, it seems that:
		 * stereo detect triggers at 4kHz out of 75kHz stereo pilot
		 * pilot is normally at 10%
		 * so triggering mV must mean total deviation is 40kHz
		 * so you can adjust sensitivity to 40/75 of trigger
		 *
		 * pilot triggers between sensitivity 2 and 3
		 * so between 93mV and 111mV
		 * worst case is 111mV,
		 * so lowest sensitivity is 40/75 * 111 = 59mV
		 * sensitivity 0 is 65mV
		 */
		radio_sensitivity = 2;

	dev->input = SOUND_MASK_PCM;
	dev->flags = 0;

	/* Easy programming mode 1 (needs to be done after reset) */
	dsp_write(Y_mod00, 0x4ec);

    	/* Ensure the POM is on while booting. */
	empeg_mixer_mute(1);

	/* Set volume */
	empeg_mixer_setvolume(dev,VOLUME_ZERO_DB);

	/* try doing this last thing */
	empeg_mixer_select_input(INPUT_PCM);

	/* setup Soft Audio Mute, and enable */
	dsp_write(Y_samCl, 0x189);	/* 4ms */
	dsp_write(Y_samCh, 0x7a5);
	dsp_write(Y_delta, 0x07d);
	dsp_write(Y_switch, 0x5d4);
/*	dsp_write(Y_switch, 0); */
	
	/* mixer_select_input(INPUT_AUX); */
	dsp_write(Y_SrcScal, 0x400);
#if 0
	
#else
      dsp_write(Y_OutSwi, 0xa49);
#endif
	dev->input = SOUND_MASK_PCM;
	dev->flags = 0;
	empeg_mixer_setloudness(dev, 0);
	empeg_mixer_setbalance(dev, BALANCE_ZERO);
	empeg_mixer_setfade(dev, FADE_ZERO);
	empeg_mixer_inflict_flags(dev);

	dsp_write(Y_OutSwi, 0xa7c);

	empeg_mixer_eq_reset();	// reset coefficients

	printk(MIXER_NAME_VERBOSE " initialised\n");

	return 0;
}

int empeg_mixer_open(struct inode *inode, struct file *file)
{
	file->f_op = &mixer_fops;
	
#if MIXER_DEBUG
	printk(MIXER_NAME ": mixer_open\n");
#endif
	file->private_data = (void *)&mixer_global;

	return 0;
}

static int empeg_mixer_release(struct inode *inode, struct file *file)
{
#if MIXER_DEBUG
	printk(MIXER_NAME ": mixer_release\n");
#endif
	
	file->private_data = NULL;
	return 0;
}

static int empeg_mixer_ioctl(struct inode *inode, struct file *file,
			     uint command, ulong arg)
{
	mixer_dev *dev = file->private_data;
#if MIXER_DEBUG
	printk(MIXER_NAME ": mixer_ioctl %08x %08lx\n", command, arg);
#endif
	switch(command)
	{
	case SOUND_MIXER_READ_DEVMASK:
		put_user_ret(MIXER_DEVMASK, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl SOUND_MIXER_READ_DEVMASK %08lx\n",
		       arg);
#endif	
		return 0;
	case SOUND_MIXER_READ_STEREODEVS:
		put_user_ret(MIXER_STEREODEVS, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl SOUND_MIXER_READ_STEREODEVS %08lx\n",
		       arg);
#endif	
		return 0;

	case EMPEG_MIXER_READ_LOUDNESS:
		put_user_ret(dev->loudness * 10, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_READ_LOUDNESS %d\n",
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
		empeg_mixer_setloudness(dev, loudness_in / 10);
		loudness_out = dev->loudness * 10;
		put_user_ret(loudness_out, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_WRITE_LOUDNESS %d == %d\n",
		       loudness_in, loudness_out);
#endif	
		return 0;
	}
	case EMPEG_MIXER_READ_LOUDNESS_DB:
	{
		int db;
		db = empeg_mixer_getloudnessdb(dev);
		put_user_ret(db, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_READ_LOUDNESS_DB == %dx\n",
		       db);
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
		empeg_mixer_setbalance(dev, balance_out);
		balance_out = dev->balance - BALANCE_ZERO;
		put_user_ret(balance_out, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_WRITE_BALANCE %d == %d\n",
		       balance_in, balance_out);
#endif
		return 0;
	}
	case EMPEG_MIXER_READ_BALANCE:
		put_user_ret(dev->balance - BALANCE_ZERO, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_READ_BALANCE == %d\n",
		       dev->balance - BALANCE_ZERO);
#endif	
		return 0;

	case EMPEG_MIXER_READ_BALANCE_DB:
	{
		int db;
		db = balance_table[dev->balance].db;
		put_user_ret(db, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_READ_BALANCE_DB == %d\n",
		       db);
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
		empeg_mixer_setfade(dev, fade_out);
		fade_out = dev->fade - FADE_ZERO;
		put_user_ret(fade_out, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_WRITE_FADE %d == %d\n",
		       fade_in, fade_out);
#endif	
		return 0;
	}
	case EMPEG_MIXER_READ_FADE:
		put_user_ret(dev->fade - FADE_ZERO, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_READ_FADE %d\n",
		       dev->fade - FADE_ZERO);
#endif	
		return 0;
	case EMPEG_MIXER_READ_FADE_DB:
	{
		int db;
		db = fade_table[dev->fade].db;
		put_user_ret(db, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_READ_FADE_DB == %d\n",
		       db);
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
		vol_out = hijack_volume_boost_w(vol_out);       // calculate boost volume
		empeg_mixer_setvolume(dev, vol_out);
		vol_out = dev->volume;
		vol_out = hijack_volume_boost_r(vol_out);	// 'de'calculate hijack volume boost. player sees what it asked for.
		vol_out = (vol_out & 0xFF) + ((vol_out << 8));
		put_user_ret(vol_out, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl MIXER_WRITE VOLUME %d == %d\n",
		       vol_in, vol_out);
#endif	
		return 0;
	}
	case MIXER_READ(SOUND_MIXER_VOLUME):
	{
		int vol = dev->volume;
		vol = hijack_volume_boost_r(vol);
		vol = (vol & 0xFF) + ((vol << 8));
		put_user_ret(vol, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl MIXER_READ VOLUME == %d\n",
		       vol);
#endif	
		return 0;
	}		
	case EMPEG_MIXER_READ_DB:
	{
		int db;
		db = empeg_mixer_getdb(dev);
		put_user_ret(db, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_READ_DB == %d\n",
		       db);
#endif	
		return 0;
	}

	case EMPEG_MIXER_READ_ZERO_LEVEL:
	{
		int level = VOLUME_ZERO_DB;
		put_user_ret(level, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_READ_ZERO_LEVEL == %d\n",
		       level);
#endif	
		return 0;
	}

	case EMPEG_MIXER_WRITE_SOURCE:
	{
		int source;
		get_user_ret(source, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_WRITE_SOURCE %d\n",
		       source);
#endif	
		if (source & SOUND_MASK_PCM)
		{
			empeg_mixer_select_input(INPUT_PCM);
			dev->input = SOUND_MASK_PCM;
		}
		else if (source & SOUND_MASK_RADIO)
		{
			empeg_mixer_select_input(INPUT_RADIO_FM);
			dev->input = SOUND_MASK_RADIO;
		}
		else if (source & SOUND_MASK_LINE1)
		{
			empeg_mixer_select_input(INPUT_RADIO_AM);
			dev->input = SOUND_MASK_LINE1;
		}
		else if (source & SOUND_MASK_LINE)
		{
		        empeg_mixer_select_input(INPUT_AUX);
			dev->input = SOUND_MASK_LINE;
		}
		put_user_ret(dev->input, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_SOURCE:
		put_user_ret(dev->input, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_READ_SOURCE == %d\n",
		       dev->input);
#endif
		return 0;

	case EMPEG_MIXER_WRITE_FLAGS:
	{
		int flags;
		get_user_ret(flags, (int *)arg, -EFAULT);
		dev->flags = flags;
		empeg_mixer_inflict_flags(dev);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_WRITE_FLAGS %d\n",
		       flags);
#endif	
		return 0;
	}
	case EMPEG_MIXER_READ_FLAGS:
		put_user_ret(dev->flags, (int *)arg, -EFAULT);
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_READ_FLAGS == %d\n",
		       dev->flags);
#endif	
		return 0;

	case EMPEG_MIXER_SET_EQ:
	{
		struct empeg_eq_section_t sections[20];
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_SET_EQ %08lx\n",
		       arg);
#endif	

		copy_from_user_ret((void *) sections, (const void *) arg,
				   sizeof(sections), -EFAULT);

		// set the sections - tone_init() needs eq_current.
		empeg_mixer_eq_set(sections);
		// don't apply yet - causes pops with tone controls active.
		// tone_init() applys.
		// empeg_mixer_eq_apply();
		eq_hijacked = 0;
		hijack_tone_init();
		return 0;
	}
	case EMPEG_MIXER_GET_EQ:
		copy_to_user_ret((void *) arg, (const void *) eq_current,
				 sizeof(eq_current), -EFAULT);
		return 0;

	case EMPEG_MIXER_SET_EQ_FOUR_CHANNEL:
	{
		int four_chan;
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_GET_EQ %08lx\n",
		       arg);
#endif
		
		copy_from_user_ret((void *) &four_chan, (const void *) arg,
				   sizeof(int), -EFAULT);

		eq_reg_last &= ~0x1000;
		eq_reg_last |= ((four_chan ^ 1) & 1) << 12;
		if(four_chan)
			dsp_write(Y_OutSwi, 0xa50);
		else
			dsp_write(Y_OutSwi, 0xa49);
		dsp_write(0xffd, eq_reg_last);
		return 0;
	}
	case EMPEG_MIXER_GET_EQ_FOUR_CHANNEL:
	{
		int four_chan;
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_GET_EQ_FOUR_CHANNEL %08lx\n",
		       arg);
#endif

		four_chan = ((eq_reg_last >> 12) & 1) ^ 1;
		copy_to_user_ret((void *) arg, (const void *) &four_chan,
				 sizeof(int), -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_GET_COMPRESSION:
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_GET_COMPRESSION %08lx\n",
		       arg);
#endif

		copy_to_user_ret((void *) arg,
				 (const void *) &mixer_compression,
				 sizeof(int), -EFAULT);
		return 0;

	case EMPEG_MIXER_SET_COMPRESSION:
	{
		int onoff;

#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_SET_COMPRESSION %08lx\n",
		       arg);
#endif

		copy_from_user_ret((void *) &onoff, (const void *) arg,
				   sizeof(int), -EFAULT);
		if(onoff)
			dsp_write(Y_compry0st_28, 0x7ab);// turn on compression
		else
			dsp_write(Y_compry0st_28, 0x5a);// turn off compression
		mixer_compression = onoff;
		return 0;
	}
	case EMPEG_MIXER_SET_SAM:
	{
		int sam;

		copy_from_user_ret((void *) &sam, (const void *) arg,
				   sizeof(int), -EFAULT);

       		if(sam) dsp_write(Y_switch, 0);
		else dsp_write(Y_switch, 0x5d4);	// 4.6ms
		
#if MIXER_DEBUG
		printk(MIXER_NAME
		       ": mixer_ioctl EMPEG_MIXER_SET_SAM %d\n",
		       sam);
#endif

		return 0;
	}
	case EMPEG_MIXER_RAW_I2C_READ:
	{
		int reg, val;

		copy_from_user_ret((void *) &reg, (const void *) arg,
				   sizeof(int), -EFAULT);
		if(reg < 0x800)
			dsp_read_xram(reg, &val);
		else
			dsp_read_yram(reg, &val);
		copy_to_user_ret((void *) arg, (const void *) &val,
				 sizeof(int), -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_RAW_I2C_WRITE:
	{
		int reg, val;
		int *block = (int *) arg;

		copy_from_user_ret((void *) &reg, (const void *) block,
				   sizeof(int), -EFAULT);
		copy_from_user_ret((void *) &val, (const void *) (block+1),
				   sizeof(int), -EFAULT);
		
		dsp_write(reg, val);
		return 0;
	}
	case EMPEG_MIXER_WRITE_SENSITIVITY:
	{
		int val;

		copy_from_user_ret((void *) &val, (const void *) arg,
				   sizeof(int), -EFAULT);
		if(val < 0 || val > 7)
			return -EINVAL;
		radio_sensitivity = val;
		eq_reg_last &= 0x1000; /* Preserve EQ num bands bit */
		eq_reg_last |= (radio_sensitivity << 1) | (radio_sensitivity << 4);
		dsp_write(0xffd, eq_reg_last);
		return 0;
	}
	case EMPEG_MIXER_READ_SIGNAL_STRENGTH:
	{
		int strength = empeg_mixer_get_fm_level();
		copy_to_user_ret((int *) arg, &strength, sizeof(int),
				 -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_SIGNAL_STRENGTH_FAST:
	{
		int strength = empeg_mixer_get_fm_level_fast();
		copy_to_user_ret((int *) arg, &strength, sizeof(int),
				 -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_SIGNAL_STEREO:
	{
		int stereo = empeg_mixer_get_stereo();
		copy_to_user_ret((int *) arg, &stereo, sizeof(int),
				 -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_SIGNAL_NOISE:
	{
		int noise = empeg_mixer_get_fm_noise();
		copy_to_user_ret((int *) arg, &noise, sizeof(int),
				 -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_SIGNAL_MULTIPATH:
	{
		int multipath = empeg_mixer_get_multipath();
		copy_to_user_ret((int *) arg, &multipath, sizeof(int),
				-EFAULT);
		return 0;
	}  
	case EMPEG_MIXER_READ_LEVEL_ADJUST:
	{
		int adjust[2];
		adjust[0] = radio_fm_level_p1;
		adjust[1] = radio_fm_level_q1;
		copy_to_user_ret((int *) arg, (int *) &adjust[0],
				 2 * sizeof(int), -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_WRITE_LEVEL_ADJUST:
	{
		int adjust[2];
		copy_from_user_ret((int *) &adjust[0], (int *) arg,
				   2 * sizeof(int), -EFAULT);
		empeg_mixer_set_fm_level(adjust[0], adjust[1]);
		return 0;
	}
#if 0
	case EMPEG_MIXER_READ_FM_AM_SELECT:
	{
		int select = empeg_mixer_get_fm_am_select();
		copy_to_user_ret((int *) arg, &select, sizeof(int), -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_WRITE_FM_AM_SELECT:
	{
		int select;
		copy_from_user_ret(&select, (int *) arg, sizeof(int), -EFAULT);
		if(select < 0 || select > 1)
			return -EINVAL;
		empeg_mixer_set_fm_am_select(select);
		return 0;
	}
#endif
	case EMPEG_MIXER_READ_FM_DEEMPHASIS:
		copy_to_user_ret((int *) arg, &radio_fm_deemphasis,
				 sizeof(int), -EFAULT);
		return 0;
	case EMPEG_MIXER_WRITE_FM_DEEMPHASIS:
	{
		int setting;
		copy_from_user_ret(&setting, (int *) arg, sizeof(int),
				   -EFAULT);
		return empeg_mixer_set_fm_deemphasis(setting);
	}
	default:
		return -EINVAL;
	}
}

static void empeg_mixer_setloudness(mixer_dev *dev, int level)
{
	static dsp_setup dynlou_44100[] = {
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
	       
	static dsp_setup dynlou_38000[] = {
		{ Y_KLCl, 0x5e2 }, /* p78, 100Hz */
		{ Y_KLCh, 0x7d6 },
		{ Y_KLBl, 0x5b6 },
		{ Y_KLBh, 0xc28 },
		{ Y_KLA0l, 0x467 },
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

	dsp_setup *table;

	if(sampling_rate == 38000)
		table = dynlou_38000;
	else
		table = dynlou_44100;
	       
	if (level < 0)
		level = 0;
	if (level >= LOUDNESS_TABLE_SIZE)
		level = LOUDNESS_TABLE_SIZE - 1;
		
	if (level) {
		dsp_patchmulti(table, Y_Cllev,
			       loudness_table[level].Cllev);
		dsp_writemulti(table);
	}
	else {
		dsp_writemulti(louoff);
	}
	dev->loudness = level;
}

static int empeg_mixer_getloudnessdb(mixer_dev *dev)
{
	return loudness_table[dev->loudness].db;
}

static void empeg_mixer_setbalance(mixer_dev *dev, int balance)
{
	dsp_write(Y_BALL0, balance_table[balance].ball/8);
	dsp_write(Y_BALL1, balance_table[balance].ball/8);
	dsp_write(Y_BALR0, balance_table[balance].balr/8);
	dsp_write(Y_BALR1, balance_table[balance].balr/8);
	dev->balance = balance;
}

static void empeg_mixer_setfade(mixer_dev *dev, int fade)
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

static void empeg_mixer_mute(int on)
{
	if (on)
		GPCR = EMPEG_DSPPOM;
	else
		GPSR = EMPEG_DSPPOM;
#if MIXER_DEBUG
	printk(MIXER_NAME ": mixer_mute %s\n",on?"on":"off");
#endif
}

void empeg_mixer_select_input(int input)
{
	static dsp_setup fm_setup[]=
	{ { 0xfff, 0x5323 },
	  { 0xffe, 0x28ed },
	  { 0xffd, 0x1000 }, /* also in case statement */
	  { 0xffc, 0x0086 }, /* level IAC off=e080 */
	  { 0xffb, 0x0aed },
	  { 0xffa, 0x1048 },
	  { 0xff9, 0x0020 }, /* no I2S out */
	  { 0xff3, 0x0000 },
	  { 0,0 } };
	
	static dsp_setup am_setup[]=
	{ { 0xfff, 0xd223 },
	  { 0xffe, 0x28ed },
	  { 0xffd, 0x1000 }, /* also in case statement */
	  { 0xffc, 0x0000 }, /* level IAC off=e080 */
	  { 0xffb, 0x0aed },
	  { 0xffa, 0x1044 },
	  { 0xff9, 0x0000 }, /* no I2S out */
	  { 0xff3, 0x0000 },
	  { 0,0 } };
	
	static dsp_setup mpeg_setup[]=
	{ { 0xfff, 0xd223 },
	  { 0xffe, 0x28ed },
	  { 0xffd, 0x1000 }, /* also in case statement */
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
	  { 0xffd, 0x1000 }, /* also in case statement */
	  { 0xffc, 0x6080 }, /*LEVEL_IAC*/
	  { 0xffb, 0x0aed }, /*IAC*/
	  { 0xffa, 0x904a }, /*SEL*/
	  { 0xff9, 0x0000 }, /*HOST*/ /* no I2S out */
	  { 0xff3, 0x0000 }, /*RDS_CONTROL*/
	  { 0,0 } };

	mixer_dev *dev = &mixer_global;

	eq_reg_last &= 0x1000; /* Preserve EQ num bands bit */
	eq_reg_last |= (radio_sensitivity << 1) | (radio_sensitivity << 4);
	dsp_patchmulti(fm_setup, 0xffd, eq_reg_last);
	dsp_patchmulti(mpeg_setup, 0xffd, eq_reg_last);
	dsp_patchmulti(aux_setup, 0xffd, eq_reg_last);
		
	/* Ensure any beeps playing are off,
	   because this may block for some time */
	dsp_write(Y_sinusMode, 0x89a);
	
	/* POM low - hardware mute */ 
	empeg_mixer_mute(1);

	hijack_current_mixer_input = input;
	hijack_volume_boost_reapply (dev); // apply boost whilst muted to avoid pops.

	switch(input) {
	case INPUT_RADIO_FM: /* FM */
	  /* FM mode: see p64 */
		dsp_writemulti(fm_setup);
		
		/* Easy programming set 2, FM */
		dsp_write(X_modpntr,0x600);
		
		/* Release POM if it wasn't already released*/
		if (!(mixer_global.flags & EMPEG_MIXER_FLAG_MUTE))
			empeg_mixer_mute(0);
		
		/* Disable DSP_IN1 mute control */
		dsp_write(Y_EMute,0x000);
		dsp_write(Y_EMuteF1,0x000);
		
		/* Select mode */
		dsp_write(X_modpntr,0x080);
		
		/* signal strength, X:levn, X:leva
		   X:levn = X:leva (unfiltered) = 4(D:level*Y:p1+Y:q1)
		*/
		if(empeg_hardwarerevision() < 6) {
			/* FM Level adjustment */
			empeg_mixer_set_fm_level(2047, -84);
			
			/* ok let's just turn everything off */

			/* disable Softmute f(level) */
			dsp_write(Y_p2, 0x000);
			dsp_write(Y_q2, 0x7ff);

			/* No FM attenuation due to low level */
			dsp_write(Y_minsmtc, 0x7ff);
			dsp_write(Y_minsmtcn, 0x7ff);

			// multipath detection, X:mlta
			//		dsp_write(Y_c1, -974);
			dsp_write(Y_c1, -2048);
			// disable Softmute f(noise)
			dsp_write(Y_p7, 0x000);
			dsp_write(Y_q7, 0x7ff);
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
			dsp_write(Y_E_strnf_rsp, 0x7ff);
			dsp_write(Y_E_mltp_rsp, 0x7ff);
		}
		else {
			/* FM Level adjustment */
			empeg_mixer_set_fm_level(1129, -120);

			/* Level -> Softmute */
			dsp_write(Y_p2, 559);
			dsp_write(Y_q2, 47);

			/* Level -> Stereo */
			dsp_write(Y_p3, 1078);
			dsp_write(Y_q3, -373);

			/* Level -> Response */
			dsp_write(Y_p5, 904);
			dsp_write(Y_q5, -135);
			
			/* Max attenuation from level */
			/* Defaults */
			
			/* Max attenuation from noise */
			/* Defaults */
			
			/* Noise -> Stereo and Response */
			dsp_write(Y_E_strnf_str, 0);
			dsp_write(Y_E_strnf_rsp, 0);

			/* Multipath -> Stereo and Response */
			dsp_write(Y_E_mltp_str, 0);
			dsp_write(Y_E_mltp_rsp, 0);

			/* Load bank1 with bass boost:
			   Values calculated by cdsp.exe
                           +6db bass, 0db treble, first order cutoff 125Hz */
			dsp_write(Y_Ctl1,0x055);
			dsp_write(Y_Cth1,0x3ee);
			dsp_write(Y_Btl1,0x000);
			dsp_write(Y_Bth1,0x000);
			dsp_write(Y_At01,0x008);
			dsp_write(Y_At11,0x008);
			dsp_write(Y_At21,0x000);
			dsp_write(Y_KTrt1,0x4c4);
			dsp_write(Y_KTft1,0x662);
			dsp_write(Y_KTmid1,0x402);
			dsp_write(Y_KTbas1,0x47a);
			dsp_write(Y_KTtre1,0x000);

			if (!hijack_disable_bassboost_FM)
				/* Enable bank1 tone control */
				dsp_write(X_audioc, 0x2f80);
			else
				/* Disable Tone Control (selet bank0) */
				dsp_write(X_audioc, 0x2c00);
		}

		/* use str_corr method of de-emphasis/adaptation */
		dsp_write(Y_sdr_d_c, 0x7ff);
		/* defaults to -12dB at 10kHz max attenuation */

		/* set for last known de-emphasis */
		empeg_mixer_set_fm_deemphasis(radio_fm_deemphasis);

		empeg_mixer_set_sampling_rate(dev, 38000);
		break;    
		
	case INPUT_RADIO_AM: /* AM */
		/* AM mode: see p64 */
		dsp_writemulti(am_setup);
		
		/* Easy programming set 3, AM */
		dsp_write(X_modpntr, 0x640);
		
		/* Release POM if it wasn't already released*/
		if (!(mixer_global.flags & EMPEG_MIXER_FLAG_MUTE))
			empeg_mixer_mute(0);
		
		/* Disable DSP_IN1 mute control */
		dsp_write(Y_EMute, 0x000);
		dsp_write(Y_EMuteF1, 0x000);
		
		/* Select mode */
		dsp_write(X_modpntr, 0x300);
		
		/* FM Level adjustment */
		empeg_mixer_set_fm_level(1129, -120);
		
#if 0
		dsp_write(Y_AMb02 +  0, 0x000);
		dsp_write(Y_AMb02 +  1, 0x400);
		dsp_write(Y_AMb02 +  2, 0x000);
		dsp_write(Y_AMb02 +  3, 0x000);
		dsp_write(Y_AMb02 +  4, 0x000);
		dsp_write(Y_AMb02 +  5, 0x000);
		dsp_write(Y_AMb02 +  6, 0x400);
		dsp_write(Y_AMb02 +  7, 0x000);
		dsp_write(Y_AMb02 +  8, 0x000);
		dsp_write(Y_AMb02 +  9, 0x000);
		dsp_write(Y_AMb02 + 10, 0x000);
		dsp_write(Y_AMb02 + 11, 0x400);
		dsp_write(Y_AMb02 + 12, 0x000);
		dsp_write(Y_AMb02 + 13, 0x000);
		dsp_write(Y_AMb02 + 14, 0x000);
#else
		dsp_write(Y_p12, 0x000);
		dsp_write(Y_q12, 0x7ff);
		dsp_write(Y_p13, 0x000);
		dsp_write(Y_p13, 0x7ff);
#endif

		/* Disable tone control (select bank0) */
		dsp_write(X_audioc, 0x2c00);
		
		empeg_mixer_set_sampling_rate(dev, 38000);
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
			empeg_mixer_mute(0);

		/* Wait for a while so that the I2S being clocked into the
		   DSP by DMA runs the initialisation code: the DSP is cycle-
		   locked to the incoming bitstream */
		{ int a=jiffies+(HZ/20); while(jiffies<a); }
		
		/* Select mode */
		dsp_write(X_modpntr,0x0200);

		/* Disable tone control (select bank0) */
		dsp_write(X_audioc, 0x2c00);
		
		empeg_mixer_set_sampling_rate(dev, 44100);
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

		/* Disable tone control (select bank0) */
		dsp_write(X_audioc, 0x2c00);
		
		empeg_mixer_set_sampling_rate(dev, 44100);
		break;    
	}
}

static int hijack_volume_boost_w(int requested) 
{
	int boosted;
	hijack_current_vol_request = requested;    // keep tabs on what the player thinks the volume is
	boosted = requested + hijack_volboost[hijack_current_mixer_input];
	if ( boosted > 100 ) 
	{  // desired volume exceeds max. Limit volume, and keep tabs on what the applied boost actually is.
		hijack_current_vol_boost = 100 - requested;  
#if HIJACK_VOLBOOST_DEBUG
		printk("VOLBOOST: Req: %d,  Boosted: %d,  Appl. Boost (++): %d,  Appl. Vol: 100 \n", requested, boosted, hijack_current_vol_boost);
#endif
		return 100;    
	} else if ( boosted < 0 )
		{  // desired volume is less than min. Make volume 0, and keep tabs on what the applied boost actually is.
			hijack_current_vol_boost = 0 - requested;  // Limit volume if it is less than 0; keep tabs on current boost.
#if HIJACK_VOLBOOST_DEBUG
			printk("VOLBOOST: Req: %d,  Boosted: %d,  Appl. Boost (--): %d,  Appl. Vol: 0\n", requested, boosted, hijack_current_vol_boost);
#endif
			return 0;
	} else {  // desired volume is within allowable range.
		hijack_current_vol_boost = hijack_volboost[hijack_current_mixer_input];
#if HIJACK_VOLBOOST_DEBUG
		printk("VOLBOOST: Req: %d,  Boosted: %d,  Appl. Boost: %d,  Appl. Vol: %d\n", requested, boosted, hijack_current_vol_boost, boosted);
#endif
		return boosted;
	}
}


static int hijack_volume_boost_r(int real)
{
	int requested;
	requested = real - hijack_current_vol_boost;
#if HIJACK_VOLBOOST_DEBUG
	printk("Boosted: %d,   Requested: %d,   Current Boost: %d \n", real, requested, hijack_current_vol_boost);
#endif
	return requested;  
}
		
static void hijack_volume_boost_reapply(mixer_dev *dev)
{
	// We call this when we change the current input.
	// Calculate the desired boost for the new input.
	int vol;
	vol = hijack_volume_boost_w(hijack_current_vol_request);
	(void) empeg_mixer_setvolume(dev, vol);
}

static int empeg_mixer_setvolume(mixer_dev *dev, int vol)
{
	hijack_current_mixer_volume = vol;
	dsp_write(Y_VAT, volume_table[vol].vat);
	dsp_write(Y_VGA, volume_table[vol].vga);
	dev->volume = vol;

#if MIXER_DEBUG
	printk(MIXER_NAME ": volume set %d VAT:%03x VGA:%03x\n",vol,
	       volume_table[vol].vat,volume_table[vol].vga);
#endif
	return vol;
}

void
hijack_tone_set (int bass_value, int bass_freq, int bass_q, int treble_value, int treble_freq, int treble_q)
{
	struct empeg_eq_section_t sections[20];

	if (eq_hijacked == 0) {
		// Save the real eq whenever the player has changed it.
		memcpy(hijack_eq_real, eq_current, sizeof(eq_current));
		eq_hijacked = 1;
	}

	// load in the current eq.
	memcpy(sections, eq_current, sizeof(sections));

	sections[8].word1  = bass_freq;
	sections[18].word1 = bass_freq;
	sections[8].word2  = bass_q | bass_value;
	sections[18].word2 = bass_q | bass_value;

	sections[9].word1  = treble_freq;
	sections[19].word1 = treble_freq;
	sections[9].word2  = treble_q | treble_value;
	sections[19].word2 = treble_q | treble_value;

	(void)empeg_mixer_eq_set(sections);
	(void)empeg_mixer_eq_apply();

	if (bass_value == 64 || treble_value == 64) {
		// one or both are flat:  apply original eq
		if (bass_value == 64) {
			sections[8]  = hijack_eq_real[8];
			sections[18] = hijack_eq_real[18];
		}
		if (treble_value == 64) {
			sections[9]  = hijack_eq_real[9];
			sections[19] = hijack_eq_real[19];
		}
		(void)empeg_mixer_eq_set(sections);
		(void)empeg_mixer_eq_apply();
	}
}

static int empeg_mixer_getdb(mixer_dev *dev)
{
	return volume_table[(dev->volume-hijack_current_vol_boost)].db;  // Only return the dBs that the player set, not the boosted.
}

static void empeg_mixer_inflict_flags(mixer_dev *dev)
{
	unsigned int flags = dev->flags;
	empeg_mixer_mute(flags & EMPEG_MIXER_FLAG_MUTE);
}

static void empeg_mixer_eq_set(struct empeg_eq_section_t *sections)
{
	int i, order;
#if EQ_DEBUG 
	printk ("\n****** Mixer EQ apply ******\n" );
	printk ("****** Bass words: %04x %04x - %04x %04x ******\n", sections[8].word1, sections[8].word2, sections[18].word1, sections[18].word2 );
	printk ("****** Treble words: %04x %04x - %04x %04x ******\n", sections[9].word1, sections[9].word2, sections[19].word1, sections[19].word2 );
#endif
	
	for(i=0; i<20; i++) {
		eq_current[i].word1 = sections[i].word1;
		eq_current[i].word2 = sections[i].word2;

		order = eq_section_order[i];
		eq_last[i] = sections[order].word1;
		eq_last[i+20] = sections[order].word2;
	}
}

static void empeg_mixer_eq_reset(void)
{
	empeg_mixer_eq_set(eq_init);
}

void empeg_mixer_eq_apply(void)
{
	i2c_write(IICD_DSP, 0xf80, eq_last, 40);
}

int empeg_mixer_get_fm_level_fast(void)
{
	unsigned level;

	if (dsp_read_xram(X_levn, &level) < 0) {
		return -1;
	}

	level >>= 1;
	if(level >= 65536) level = 0;	// negative
	
	return (int) level;
}

int empeg_mixer_get_fm_level(void)
{
	unsigned level;

	if (dsp_read_xram(X_leva, &level) < 0) {
		return -1;
	}

	level >>= 1;
	if(level >= 65536) level = 0;	// negative
	
	return (int) level;
}

static int empeg_mixer_get_fm_noise(void)
{
	unsigned noise;

	if (dsp_read_xram(X_noisflt, &noise) < 0)
		return -1;

	/* Convert 18 bit signed to 16 bit unsigned abs */
	noise >>= 1;
	if(noise >= 65536) noise = (-noise) & 65535;

	return (int) noise;
}

static void empeg_mixer_set_fm_level(unsigned int p1, unsigned int q1)
{
	// sign extend
	if(p1 & 2048)
		p1 = - (((~p1) & 2047) + 1);
	if(q1 & 2048)
		q1 = - (((~q1) & 2047) + 1);

	radio_fm_level_p1 = p1;
	radio_fm_level_q1 = q1;

	dsp_write(Y_p1, p1);
	dsp_write(Y_q1, q1);
}

void empeg_mixer_clear_stereo(void)
{
	stereo_level = 0;
}

void empeg_mixer_set_stereo(int on)
{
	if(on)
		dsp_write(Y_stro, 0x7ff);
	else
		dsp_write(Y_stro, 0);
}

int empeg_mixer_get_stereo(void)
{
	unsigned pltd;

	if (dsp_read_xram(X_pltd, &pltd) < 0)
		return -1;

	return pltd;
}

int empeg_mixer_get_multipath(void)
{
	unsigned mlta;

	if (dsp_read_xram(X_mlta, &mlta) < 0) {
		return -1;
	}

	/* Convert 18 bit signed to 16 bit unsigned abs */
	mlta >>= 1;
	if(mlta >= 65536) mlta = (-mlta) & 65535;

	return (int) mlta;
}

int empeg_mixer_get_vat(void)
{
	return volume_table[mixer_global.volume].vat;
}

static int empeg_mixer_set_fm_deemphasis(int which)
{
    unsigned p6, q6, c61, c91;
    if(which == 50) {
	    p6 = 0x11e;
	    q6 = 0x228;
	    c61 = 0x4b8;
	    c91 = 0x347;
    }
    else if(which == 75) {
	    p6 = 0x0e5;
	    q6 = 0x17b;
	    c61 = 0x59e;
	    c91 = 0x260;
    }
    else
	    return -EINVAL;

    dsp_write(Y_p6, p6);
    dsp_write(Y_q6, q6);
    dsp_write(Y_c61, c61);
    dsp_write(Y_c91, c91);
    radio_fm_deemphasis = which;
    return 0;
}

static void empeg_mixer_set_sampling_rate(mixer_dev *dev, unsigned rate)
{
	if(rate != 38000 && rate != 44100)
	{
		panic("Can't set sampling rate %u\n", rate);
	}
	sampling_rate = rate;

	/* setup beep table */
	empeg_audio_beep_setup(sampling_rate);

	/* setup loudness coefficients */
	empeg_mixer_setloudness(dev, dev->loudness);
}

static volume_entry volume_table[101] =
{
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
/*	{ 0x800, 0xe6c, 1000 }, */ /* 10.0 dB intensity 1000000.000000 */
};

static loudness_entry loudness_table[LOUDNESS_TABLE_SIZE] =
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

static balance_entry balance_table[BALANCE_TABLE_SIZE] =
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

static fade_entry fade_table[FADE_TABLE_SIZE] =
{
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

static struct empeg_eq_section_t eq_init[20] =
{
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

static int eq_section_order[20] =
{
	0, 10, 5, 15, 1, 11, 6, 16, 2, 12,
	7, 17, 3, 13, 8, 18, 4, 14, 9, 19
};
