/* empeg-car radio driver
 * (C) 1999-2000 empeg Ltd
 *
 * Based on the radiotrack driver distributed with Linux 2.2.0pre9.
 *
 * Authors:
 *   Mike Crowe <mac@empeg.com>
 *   John Ripley <john@empeg.com>
 *   Hugo Fiennes <hugo@empeg.com>
 */

#include <linux/module.h>	/* Modules 			*/
#include <linux/init.h>		/* Initdata			*/
#include <linux/ioport.h>	/* check_region, request_region	*/
#include <linux/delay.h>	/* udelay			*/
#include <asm/io.h>		/* outb, outb_p			*/
#include <asm/uaccess.h>	/* copy to/from user		*/
#include <linux/videodev.h>	/* kernel radio structs		*/
#include <linux/config.h>
#include <linux/empeg.h>

#include "../arch/arm/special/empeg_mixer.h"

#define RADIO_DEBUG			1

#define FLAG_SEARCH			(1<<24)
#define FLAG_SEARCH_UP			(1<<23)
#define FLAG_SEARCH_DOWN		0
#define FLAG_MONO			(1<<22)
#define FLAG_STEREO			0
#define FLAG_BAND_FM			0
#define FLAG_LOCAL			0
#define FLAG_DX				(1<<19)
#define FLAG_SENSITIVITY_HIGH		0
#define FLAG_SENSITIVITY_MEDIUM		(1<<17)
#define FLAG_SENSITIVITY_LOW		(1<<16)
#define FLAG_SENSITIVITY_VERYLOW	((1<<16)|(1<<17))
#define FREQUENCY_MASK			0x7fff
/* Sonja has an I2Cesque radio data interface */
#define RADIO_DATAOUT			GPIO_GPIO10
#define RADIO_DATAIN			GPIO_GPIO14
#define RADIO_WRITE			GPIO_GPIO13
#define RADIO_CLOCK			GPIO_GPIO12

struct empeg_radio_device;

/* external functions */
void empeg_mixer_clear_stereo(void);
void empeg_mixer_set_stereo(int on);

int __init empeg_radio_init(struct video_init *v);

static int empeg_radio_getsigstr(struct empeg_radio_device *dev);
static int empeg_radio_getstereo(struct empeg_radio_device *dev);

#ifdef MODULE
MODULE_AUTHOR("Mike Crowe");
MODULE_DESCRIPTION("A driver for the empeg-car FM radio.");
MODULE_SUPPORTED_DEVICE("radio");
int init_module(void);
void cleanup_module(void);
#endif

/* non-exported */
static void empeg_radio_philips_write_bit(int data);
static void empeg_radio_philips_write_word(int data);
static int empeg_radio_philips_update(struct empeg_radio_device *dev,
				      int search, int direction);
static int empeg_radio_philips_ioctl(struct video_device *dev,
				     unsigned int cmd, void *arg);
static int empeg_radio_philips_open(struct video_device *dev, int flags);
static void empeg_radio_philips_close(struct video_device *dev);

struct empeg_radio_device
{
	/* Eventually we need to put lock strength, stereo/mono
	 * etc. in here. */
	unsigned long freq;
	int mono;
	int dx;
	int sensitivity;
};


static int use_count = 0;


/**********************************************************************
 * Global stuff
 **********************************************************************/

static int empeg_radio_getsigstr(struct empeg_radio_device *dev)
{
	unsigned signal = empeg_mixer_get_fm_level();
	if ((signal < 0) || (signal > 131071)) signal = 0;
	else {
		signal >>= 1;	/* 18 bits signed -> 16 bits unsigned */
	}
	return (int) signal;
}

int empeg_radio_getstereo(struct empeg_radio_device *dev)
{
	return empeg_mixer_get_stereo();
}


/**********************************************************************
 * Philips DSP based I2C interface
 **********************************************************************/

static void empeg_radio_philips_write_bit(int data)
{
	if (data)
		GPCR=EMPEG_RADIODATA;
	else
		GPSR=EMPEG_RADIODATA;
	GPSR=EMPEG_RADIOCLOCK;
	udelay(7);
	GPCR=EMPEG_RADIOCLOCK;
	udelay(7);
}

static void empeg_radio_philips_write_word(int data)
{
	int a;
	
	/* Put radio module in write mode */
	GPSR=RADIO_WRITE;
	udelay(10);
	
	/* Clock out a 25-bit frame */
	for(a=24;a>=0;a--) empeg_radio_philips_write_bit(data&(1<<a));
	
	/* Ensure radio data is floating again */
	GPCR=RADIO_DATAOUT;
	
	/* Out of write mode */
	GPCR=RADIO_WRITE;
}

static int empeg_radio_philips_update(struct empeg_radio_device *dev,
				      int search, int direction)
{
	int word = 0;

	int a, divisor = 102400000;
        unsigned long freq = dev->freq;
	if (dev->mono)
		word |= FLAG_MONO;
	else
		word |= FLAG_STEREO;

	if (dev->dx)
		word |= FLAG_DX;
	else
		word |= FLAG_LOCAL;

	switch (dev->sensitivity)
	{
	case 0:
		word |= FLAG_SENSITIVITY_VERYLOW;
		break;
	case 1:
		word |= FLAG_SENSITIVITY_LOW;
		break;
	case 2:
		word |= FLAG_SENSITIVITY_MEDIUM;
		break;
	case 3:
	default:
		word |= FLAG_SENSITIVITY_HIGH;
		break;
	}
	
	if (search)
	{
		word |= FLAG_SEARCH;
		if (direction)
			word |= FLAG_SEARCH_UP;
		else
			word |= FLAG_SEARCH_DOWN;
	}
	
	/* Add 10.7Mhz IF */
	freq+=10700000;
	
	/* Set bits */
	for(a = 13; a >= 0; a--)
	{
		if (freq>=divisor)
		{
			word|=(1<<a);
			freq-=divisor;
		}
		divisor>>=1;
	}
	
	empeg_radio_philips_write_word(word);

	return 0;
}

static int empeg_radio_philips_ioctl(struct video_device *dev,
				     unsigned int cmd, void *arg)
{
	struct empeg_radio_device *radio=dev->priv;
	
	switch(cmd)
	{
	case VIDIOCGCAP:
	{
		struct video_capability v;
		v.type=VID_TYPE_TUNER;
		v.channels=1;
		v.audios=1;
		/* No we don't do pictures */
		v.maxwidth=0;
		v.maxheight=0;
		v.minwidth=0;
		v.minheight=0;
		strcpy(v.name, "empeg-car radio");
		if(copy_to_user(arg,&v,sizeof(v)))
			return -EFAULT;
		return 0;
	}
	case VIDIOCGTUNER:
	{
		struct video_tuner v;
		if(copy_from_user(&v, arg,sizeof(v))!=0) 
			return -EFAULT;
		if(v.tuner)	/* Only 1 tuner */ 
			return -EINVAL;
		v.rangelow = 87500000;
		v.rangehigh = 108000000;
		v.flags = 0;
		if(empeg_radio_getstereo(radio))
			v.flags |= VIDEO_TUNER_STEREO_ON;
		v.mode = VIDEO_MODE_AUTO;
		v.signal = empeg_radio_getsigstr(radio);

		if(copy_to_user(arg,&v, sizeof(v)))
			return -EFAULT;
		return 0;
	}
	case VIDIOCSTUNER:
	{
		struct video_tuner v;
		if(copy_from_user(&v, arg, sizeof(v)))
			return -EFAULT;
		if(v.tuner!=0)
			return -EINVAL;
		/* Only 1 tuner so no setting needed ! */
		return 0;
	}
	case VIDIOCGFREQ:
		if(copy_to_user(arg, &radio->freq, sizeof(radio->freq)))
			return -EFAULT;
		return 0;
	case VIDIOCSFREQ:
		if(copy_from_user(&radio->freq, arg,sizeof(radio->freq)))
			return -EFAULT;
		empeg_radio_philips_update(radio, FALSE, 0);
		empeg_mixer_clear_stereo(); /* reset stereo level to 0 */
		return 0;
	case VIDIOCGAUDIO:
	{	
		struct video_audio v;
		memset(&v,0, sizeof(v));
		/*v.flags|=VIDEO_AUDIO_MUTABLE|VIDEO_AUDIO_VOLUME; */
		/*v.volume=radio->curvol * 6554;*/
		/*v.step=6554;*/
		strcpy(v.name, "Radio");
		if(copy_to_user(arg,&v, sizeof(v)))
			return -EFAULT;
		return 0;			
	}
	case VIDIOCSAUDIO:
	{
		struct video_audio v;
		if(copy_from_user(&v, arg, sizeof(v))) 
			return -EFAULT;	
		if(v.audio) 
			return -EINVAL;
		
		/* Check the mode for stereo/mono */
		if (v.mode == VIDEO_SOUND_MONO)
			radio->mono = TRUE;
		else
			radio->mono = FALSE;
		empeg_radio_philips_update(radio, FALSE, 0);
		return 0;
	}
	case EMPEG_RADIO_READ_MONO:
		copy_to_user_ret(arg, &(radio->mono), sizeof(radio->mono),
				 -EFAULT);
		return 0;

	case EMPEG_RADIO_WRITE_MONO:
		copy_from_user_ret(&(radio->mono), arg, sizeof(radio->mono),
				   -EFAULT);
		empeg_radio_philips_update(radio, FALSE, 0);
		return 0;

	case EMPEG_RADIO_READ_DX:
		copy_to_user_ret(arg, &(radio->dx), sizeof(radio->dx),
				 -EFAULT);
		return 0;

	case EMPEG_RADIO_WRITE_DX:
		copy_from_user_ret(&(radio->dx), arg, sizeof(radio->dx),
				   -EFAULT);
		empeg_radio_philips_update(radio, FALSE, 0);
		return 0;

	case EMPEG_RADIO_READ_SENSITIVITY:
		copy_to_user_ret(arg, (&radio->sensitivity),
				 sizeof(radio->sensitivity), -EFAULT);
		return 0;

	case EMPEG_RADIO_WRITE_SENSITIVITY:
		copy_from_user_ret(&(radio->sensitivity), arg,
				   sizeof(radio->sensitivity), -EFAULT);
		empeg_radio_philips_update(radio, FALSE, 0);
		return 0;

	case EMPEG_RADIO_SEARCH:
	{
		int direction;
		copy_from_user_ret(&direction, arg, sizeof(direction),
				   -EFAULT);
		empeg_radio_philips_update(radio, TRUE, direction);
		return 0;
	}
	case EMPEG_RADIO_GET_MULTIPATH: {
		unsigned multi = empeg_mixer_get_multipath();
		if(multi == -1) return -EIO;
		copy_to_user_ret(arg, &multi, sizeof(unsigned), -EFAULT);
		return 0;
	}
	case EMPEG_RADIO_SET_STEREO: {
		int stereo;
		copy_from_user_ret(&stereo, arg, sizeof(int), -EFAULT);
		empeg_mixer_set_stereo(stereo);
		return 0;
	}
	default:
		return -ENOIOCTLCMD;
	}
}

static int empeg_radio_philips_open(struct video_device *dev, int flags)
{
	if(use_count)
		return -EBUSY;
	use_count++;
	MOD_INC_USE_COUNT;
	return 0;
}

static void empeg_radio_philips_close(struct video_device *dev)
{
	use_count--;
	MOD_DEC_USE_COUNT;
}


/**********************************************************************
 * Generic stuff
 **********************************************************************/

static struct empeg_radio_device empeg_unit;

static struct video_device empeg_radio_philips = {
	"empeg Philips FM radio",
	VID_TYPE_TUNER,
	VID_HARDWARE_RTRACK,
	empeg_radio_philips_open,
	empeg_radio_philips_close,
	NULL,	/* Can't read  (no capture ability) */
	NULL,	/* Can't write */
	NULL,	/* No poll */
	empeg_radio_philips_ioctl,
	NULL,
	NULL
};


int __init empeg_radio_init(struct video_init *v)
{
	/* silly run-time check */
	if (empeg_hardwarerevision() >= 6) {
		/* Silently drop Mk.2 */
		return 0;
	}

	empeg_radio_philips.priv = &empeg_unit;
		
	empeg_unit.freq = 87500000;
	empeg_unit.dx = 1;
	empeg_unit.mono = 0;
	empeg_unit.sensitivity = 10;
	
	if(video_register_device(&empeg_radio_philips,
				 VFL_TYPE_RADIO) == -1)
		return -EINVAL;
	
	printk(KERN_INFO "empeg FM radio driver (Philips).\n");
		
	return 0;
}

#ifdef MODULE

EXPORT_NO_SYMBOLS;

int init_module(void)
{
	return empeg_radio_init(NULL);
}

void cleanup_module(void)
{
	video_unregister_device(&empeg_radio);
}

#endif
