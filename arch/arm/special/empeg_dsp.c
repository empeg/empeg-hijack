/*
 * SA1100/empeg DSP multiplexor
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
#include <linux/init.h>
#include <linux/fs.h>
#include <asm/arch/hardware.h>
#include <linux/empeg.h>

#include "empeg_audio3.h"
#include "empeg_mixer.h"

#ifdef CONFIG_EMPEG_DAC
#error empeg DAC driver cannot be coexist with DSP driver
#endif

#define EMPEG_DSP_NAME			"dspaudio"
#define EMPEG_DSP_NAME_VERBOSE		"empeg dsp"

/* device numbers */
#define EMPEG_DSP_MAJOR			245

#define EMPEG_MIXER_MINOR		0
#define EMPEG_MIC_MINOR			1
#define EMPEG_DSP_MINOR			3
#define EMPEG_AUDIO_MINOR		4

int __init empeg_dsp_init(void);
static int empeg_dsp_major_open(struct inode *inode, struct file *file);

static struct file_operations dsp_fops =
{
	open:		empeg_dsp_major_open
};

int __init empeg_dsp_init(void)
{
	int ret;

	if((ret = empeg_audio_init()) != 0)
		return ret;

	if((ret = empeg_mixer_init()) != 0)
		return ret;

	if((ret = register_chrdev(EMPEG_DSP_MAJOR,
				  EMPEG_DSP_NAME,
				  &dsp_fops)) != 0) {
		printk(EMPEG_DSP_NAME
		       ": unable to register major device %d\n",
		       EMPEG_DSP_MAJOR);
		
		return ret;
	}

	printk(EMPEG_DSP_NAME_VERBOSE " initialised\n");

	return 0;
}

static int empeg_dsp_major_open(struct inode *inode, struct file *file)
{
	
	switch(MINOR(inode->i_rdev)) {
	case EMPEG_AUDIO_MINOR:
	case EMPEG_DSP_MINOR:
		return empeg_audio_open(inode, file);
		
	case EMPEG_MIXER_MINOR:
		return empeg_mixer_open(inode, file);

	default:
		return -ENXIO;
	}
}

