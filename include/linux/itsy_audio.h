/*
 * Itsy (SA1100+UCB1200) Audio Device Driver Header File
 * Copyright (c) Carl Waldspurger, 1998
 *
 * $Log: itsy_audio.h,v $
 * Revision 2.2  1999/03/05 18:40:52  caw
 * Added ioctls and constants for audio output mixing.
 *
 * Revision 2.1  1999/02/19  19:36:53  caw
 * Initial checkin for OSS-compliant driver.
 *
 */

#ifndef	ITSY_AUDIO_H
#define	ITSY_AUDIO_H

/*
 * includes 
 *
 */

#include <asm/page.h>

/*
 * types
 *
 */

/* statistics */
typedef struct {
  ulong samples;
  ulong interrupts;
  ulong wakeups;  
  ulong fifo_err;
} audio_stats;

/* button matching */
typedef struct {
  uint up_mask;
  uint down_mask;
} audio_button_match;

/* audio stream position (64 bits) */
typedef long long audio_offset;

/* memory-mapped microphone buffer */
#define	MIC_MMAP_META_LG_PAGES  (0)
#define	MIC_MMAP_META_NBYTES	(PAGE_SIZE << MIC_MMAP_META_LG_PAGES)
#define	MIC_MMAP_META_NOFFSET	(MIC_MMAP_META_NBYTES / sizeof(audio_offset))
#define	MIC_MMAP_DATA_LG_PAGES  (4)
#define	MIC_MMAP_DATA_NBYTES	(PAGE_SIZE << MIC_MMAP_DATA_LG_PAGES)
#define	MIC_MMAP_DATA_NSAMPLES	(MIC_MMAP_DATA_NBYTES / sizeof(short))
#define	MIC_MMAP_DATA_MASK	(MIC_MMAP_DATA_NSAMPLES - 1)

typedef struct {
  audio_offset next;
  audio_offset unused[MIC_MMAP_META_NOFFSET - 1];  
} __attribute__ ((packed)) audio_input_mmap_meta;

typedef struct {
  short data[MIC_MMAP_DATA_NSAMPLES];
  audio_input_mmap_meta meta;
} __attribute__ ((packed)) audio_input_mmap;

/*
 * OSS ioctl interface
 *
 */

#include <linux/soundcard.h>
#define	AUDIO_OSS_IOCTL_TYPE		'P'

/*
 * native ioctl interface
 *
 */

/* driver ioctl type */
#define	AUDIO_IOCTL_TYPE		's'
#define	AUDIO_IOCTL_MAXNR		(41)

/* ioctl commands: bidirectional */
#define	AUDIO_GET_FORMAT		_IOR(AUDIO_IOCTL_TYPE,  0, uint)
#define	AUDIO_SET_FORMAT	 	_IO(AUDIO_IOCTL_TYPE,   1)
#define	AUDIO_GET_RATE			_IOR(AUDIO_IOCTL_TYPE,  2, uint)
#define	AUDIO_SET_RATE			_IO(AUDIO_IOCTL_TYPE,   3)
#define	AUDIO_LOCK_RATE			_IO(AUDIO_IOCTL_TYPE,   4)
#define	AUDIO_GET_NBUFFERED		_IOR(AUDIO_IOCTL_TYPE,  5, uint)
#define	AUDIO_GET_POSITION		_IOR(AUDIO_IOCTL_TYPE,  6, audio_offset)

/* ioctl commands: output */
#define	AUDIO_OUTPUT_GET_ATTENUATION	_IOR(AUDIO_IOCTL_TYPE, 10, uint)
#define	AUDIO_OUTPUT_SET_ATTENUATION	_IO(AUDIO_IOCTL_TYPE,  11)
#define	AUDIO_OUTPUT_STOP		_IO(AUDIO_IOCTL_TYPE,  12)
#define	AUDIO_OUTPUT_PAUSE		_IO(AUDIO_IOCTL_TYPE,  13)
#define	AUDIO_OUTPUT_GET_BUF_SIZE	_IOR(AUDIO_IOCTL_TYPE, 14, uint)
#define	AUDIO_OUTPUT_SET_BUF_SIZE	_IO(AUDIO_IOCTL_TYPE,  15)
#define	AUDIO_OUTPUT_GET_MIX_VOLUME	_IOR(AUDIO_IOCTL_TYPE, 16, uint)
#define	AUDIO_OUTPUT_SET_MIX_VOLUME	_IO(AUDIO_IOCTL_TYPE,  17)

/* ioctl commands: input */
#define	AUDIO_INPUT_GET_GAIN		_IOR(AUDIO_IOCTL_TYPE, 20, uint)
#define	AUDIO_INPUT_SET_GAIN		_IO(AUDIO_IOCTL_TYPE,  21)
#define	AUDIO_INPUT_SET_LOOPBACK	_IO(AUDIO_IOCTL_TYPE,  22)
#define	AUDIO_INPUT_SET_OFFSET_CANCEL	_IO(AUDIO_IOCTL_TYPE,  23)
#define	AUDIO_INPUT_GET_LG_WAKEUP	_IOR(AUDIO_IOCTL_TYPE, 24, uint)
#define	AUDIO_INPUT_SET_LG_WAKEUP	_IO(AUDIO_IOCTL_TYPE,  25)
#define	AUDIO_INPUT_GET_DROPPED		_IOR(AUDIO_IOCTL_TYPE, 26, audio_offset)
#define	AUDIO_INPUT_SET_DROPPED		_IOW(AUDIO_IOCTL_TYPE, 27, audio_offset)
#define	AUDIO_INPUT_SET_POSITION	_IOWR(AUDIO_IOCTL_TYPE, 28, audio_offset)
#define	AUDIO_INPUT_SET_DROP_FAIL	_IO(AUDIO_IOCTL_TYPE,  29)

/* ioctl commands: hacks */
#define	AUDIO_WATCH_BUTTONS		_IOW(AUDIO_IOCTL_TYPE, 40, audio_button_match)
#define	AUDIO_DEBUG_HOOK		_IO(AUDIO_IOCTL_TYPE,  41)

/* backwards compatibility */
#define	AUDIO_SET_ATT			AUDIO_OUTPUT_SET_ATTENUATION
#define	AUDIO_SET_GAIN			AUDIO_INPUT_SET_GAIN
#define	AUDIO_SET_OFFSET_CANCEL		AUDIO_INPUT_SET_OFFSET_CANCEL
#define	AUDIO_SET_LOOP			AUDIO_INPUT_SET_LOOPBACK

/* AUDIO_SET_FORMAT args */
#define	AUDIO_FORMAT_CHAN_MASK		(0x1)
#define	AUDIO_MONO			(0x0)
#define	AUDIO_STEREO			(0x1)
#define	AUDIO_FORMAT_SIZE_MASK		(0x2)
#define	AUDIO_8BIT			(0x0)
#define	AUDIO_16BIT			(0x2)
#define	AUDIO_FORMAT_MASK		(AUDIO_FORMAT_CHAN_MASK | AUDIO_FORMAT_SIZE_MASK)

#define	AUDIO_8BIT_MONO			(AUDIO_MONO   | AUDIO_8BIT)
#define	AUDIO_8BIT_STEREO		(AUDIO_STEREO | AUDIO_8BIT)
#define	AUDIO_16BIT_MONO		(AUDIO_MONO   | AUDIO_16BIT)
#define	AUDIO_16BIT_STEREO		(AUDIO_STEREO | AUDIO_16BIT)
#define	AUDIO_FORMAT_DEFAULT		AUDIO_16BIT_MONO

/* AUDIO_SET_RATE args [samples/sec] */
#define	AUDIO_RATE_MIN			(3000)
#define	AUDIO_RATE_MAX			(41500)
#define	AUDIO_RATE_DEFAULT		(11025)

/* AUDIO_INPUT_SET_GAIN args [0=0dB, 15=22.5dB] */
#define	AUDIO_GAIN_MIN			(0)
#define	AUDIO_GAIN_MAX			(15)
#define	AUDIO_GAIN_DEFAULT		(10)

/* AUDIO_OUTPUT_SET_ATTENUATION args [0=0dB, 31=69dB] */
#define	AUDIO_ATT_MIN			(0)
#define	AUDIO_ATT_MAX			(31)
#define	AUDIO_ATT_DEFAULT		(0)

/* AUDIO_OUTPUT_SET_BUF_SIZE args [bytes] */
#define	AUDIO_OUTPUT_BUF_SIZE_MIN	(   1 * 1024)
#define AUDIO_OUTPUT_BUF_SIZE_MAX	(1024 * 1024)

/* AUDIO_OUTPUT_SET_MIX_VOLUME args [units] */
#define	AUDIO_OUTPUT_MIX_VOLUME_MIN	(0)
#define	AUDIO_OUTPUT_MIX_VOLUME_MAX	(256)
#define	AUDIO_OUTPUT_MIX_VOLUME_DEFAULT	(AUDIO_OUTPUT_MIX_VOLUME_MAX)

/* AUDIO_INPUT_SET_LG_WAKEUP args [lg(samples)] */
#define	AUDIO_INPUT_LG_WAKEUP_MIN	(5)
#define	AUDIO_INPUT_LG_WAKEUP_MAX	(14)
#define	AUDIO_INPUT_LG_WAKEUP_DEFAULT	(11)


#ifdef CONFIG_SA1100_TIFON

#define	AUDIO_SWITCH_IOCTL_TYPE		'x'

#define EEAR_ON     1
#define IEAR_ON     2
#define SPKR_ON     4
#define MIKE1_EN    8
#define MIKE2_EN   16 

#define	AUDIO_SWITCH_GET	_IOR(AUDIO_SWITCH_IOCTL_TYPE,  0, uint)
#define	AUDIO_SWITCH_SET	_IO(AUDIO_SWITCH_IOCTL_TYPE,   1)

#endif


#endif	/* ITSY_AUDIO_H */
