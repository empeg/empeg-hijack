/*
 * empeg-car hardware-specifics
 *
 * (C) 1999/2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Hugo Fiennes, <hugo@empeg.com>
 *   Mike Crowe, <mac@empeg.com>
 *
 * This is the kernel/userspace interaction interface. This header
 * file should not be included from any kernel header files, if it is
 * then there is a great risk that the wrong version of this file
 * could be used sometimes.
 *
 * Only userspace code and kernel driver implementation (i.e. *.c)
 * files should include this file.
 *
 */

#ifndef _INCLUDE_EMPEG_H
#define _INCLUDE_EMPEG_H 1

/* Empeg IR ioctl values */
#define EMPEG_IR_MAGIC			'i'

/* Set/get the remote control type */
#define EMPEG_IR_WRITE_TYPE		_IOW(EMPEG_IR_MAGIC, 1, int)
#define EMPEG_IR_READ_TYPE		_IOR(EMPEG_IR_MAGIC, 2, int)

#define IR_TYPE_COUNT			1

#define IR_TYPE_CAPTURE			0
#define IR_TYPE_KENWOOD			1

/* Empeg Display ioctl values */
#define EMPEG_DISPLAY_MAGIC		'd'

/* Deprecated ioctl codes */
#define DIS_IOCREFRESH			_IO(EMPEG_DISPLAY_MAGIC, 0)
#define DIS_IOCSPOWER			_IOW(EMPEG_DISPLAY_MAGIC, 1, int)
#define DIS_IOCSPALETTE			_IOW(EMPEG_DISPLAY_MAGIC, 4, int)
#define DIS_IOCCLEAR			_IO(EMPEG_DISPLAY_MAGIC, 5)
#define DIS_IOCENQUEUE			_IO(EMPEG_DISPLAY_MAGIC, 6)
#define DIS_IOCPOPQUEUE			_IO(EMPEG_DISPLAY_MAGIC, 7)
#define DIS_IOCFLUSHQUEUE		_IO(EMPEG_DISPLAY_MAGIC, 8)

/* Should use these ioctl codes instead */
#define EMPEG_DISPLAY_REFRESH		_IO(EMPEG_DISPLAY_MAGIC, 0)
#define EMPEG_DISPLAY_POWER		_IOW(EMPEG_DISPLAY_MAGIC, 1, int)
#define EMPEG_DISPLAY_WRITE_PALETTE	_IOW(EMPEG_DISPLAY_MAGIC, 4, int)
#define EMPEG_DISPLAY_CLEAR		_IO(EMPEG_DISPLAY_MAGIC, 5)
#define EMPEG_DISPLAY_ENQUEUE		_IO(EMPEG_DISPLAY_MAGIC, 6)
#define EMPEG_DISPLAY_POPQUEUE		_IO(EMPEG_DISPLAY_MAGIC, 7)
#define EMPEG_DISPLAY_FLUSHQUEUE	_IO(EMPEG_DISPLAY_MAGIC, 8)
#define EMPEG_DISPLAY_QUERYQUEUEFREE	_IOR(EMPEG_DISPLAY_MAGIC, 9, int)
#define EMPEG_DISPLAY_SENDCONTROL	_IOW(EMPEG_DISPLAY_MAGIC, 10, int)
#define EMPEG_DISPLAY_SETBRIGHTNESS	_IOW(EMPEG_DISPLAY_MAGIC, 11, int)

/* Sound IOCTLs */
/* Make use of the bitmasks in soundcard.h, we only support.
 * PCM, RADIO and LINE. */

#define EMPEG_MIXER_MAGIC 'm'
#define EMPEG_DSP_MAGIC 'a'

#define EMPEG_MIXER_READ_SOURCE		_IOR(EMPEG_MIXER_MAGIC, 0, int)
#define EMPEG_MIXER_WRITE_SOURCE	_IOW(EMPEG_MIXER_MAGIC, 0, int)
#define EMPEG_MIXER_READ_FLAGS		_IOR(EMPEG_MIXER_MAGIC, 1, int)
#define EMPEG_MIXER_WRITE_FLAGS		_IOW(EMPEG_MIXER_MAGIC, 1, int)
#define EMPEG_MIXER_READ_DB		_IOR(EMPEG_MIXER_MAGIC, 2, int)
#define EMPEG_MIXER_WRITE_LOUDNESS	_IOW(EMPEG_MIXER_MAGIC, 4, int)
#define EMPEG_MIXER_READ_LOUDNESS	_IOR(EMPEG_MIXER_MAGIC, 4, int)
#define EMPEG_MIXER_READ_LOUDNESS_DB	_IOR(EMPEG_MIXER_MAGIC, 5, int)
#define EMPEG_MIXER_WRITE_BALANCE	_IOW(EMPEG_MIXER_MAGIC, 6, int)
#define EMPEG_MIXER_READ_BALANCE	_IOR(EMPEG_MIXER_MAGIC, 6, int)
#define EMPEG_MIXER_READ_BALANCE_DB	_IOR(EMPEG_MIXER_MAGIC, 7, int)
#define EMPEG_MIXER_WRITE_FADE		_IOW(EMPEG_MIXER_MAGIC, 8, int)
#define EMPEG_MIXER_READ_FADE		_IOR(EMPEG_MIXER_MAGIC, 8, int)
#define EMPEG_MIXER_READ_FADE_DB	_IOR(EMPEG_MIXER_MAGIC, 9, int)
#define EMPEG_MIXER_SET_EQ		_IOW(EMPEG_MIXER_MAGIC, 10, int)
#define EMPEG_MIXER_GET_EQ		_IOR(EMPEG_MIXER_MAGIC, 11, int)
#define EMPEG_MIXER_SET_EQ_FOUR_CHANNEL	_IOW(EMPEG_MIXER_MAGIC, 12, int)
#define EMPEG_MIXER_GET_EQ_FOUR_CHANNEL	_IOR(EMPEG_MIXER_MAGIC, 13, int)
#define EMPEG_MIXER_GET_COMPRESSION	_IOR(EMPEG_MIXER_MAGIC, 14, int)
#define EMPEG_MIXER_SET_COMPRESSION	_IOW(EMPEG_MIXER_MAGIC, 14, int)
#define EMPEG_MIXER_SET_SAM		_IOW(EMPEG_MIXER_MAGIC, 15, int)
#define EMPEG_MIXER_RAW_I2C_READ	_IOR(EMPEG_MIXER_MAGIC, 16, int)
#define EMPEG_MIXER_RAW_I2C_WRITE	_IOW(EMPEG_MIXER_MAGIC, 16, int)
#define EMPEG_MIXER_WRITE_SENSITIVITY	_IOW(EMPEG_MIXER_MAGIC, 17, int)
#define EMPEG_MIXER_READ_SIGNAL_STRENGTH _IOR(EMPEG_MIXER_MAGIC, 18, int)
#define EMPEG_MIXER_READ_SIGNAL_STEREO	_IOR(EMPEG_MIXER_MAGIC, 19, int)
#define EMPEG_MIXER_READ_LEVEL_ADJUST	_IOR(EMPEG_MIXER_MAGIC, 20, int)
#define EMPEG_MIXER_WRITE_LEVEL_ADJUST	_IOW(EMPEG_MIXER_MAGIC, 20, int)
#define EMPEG_MIXER_READ_SIGNAL_NOISE	_IOR(EMPEG_MIXER_MAGIC, 21, int)
#define EMPEG_MIXER_READ_SIGNAL_MULTIPATH _IOR(EMPEG_MIXER_MAGIC, 22, int)
#define EMPEG_MIXER_READ_FM_AM_SELECT	_IOR(EMPEG_MIXER_MAGIC, 23, int)
#define EMPEG_MIXER_WRITE_FM_AM_SELECT	_IOW(EMPEG_MIXER_MAGIC, 23, int)
#define EMPEG_MIXER_READ_SIGNAL_STRENGTH_FAST _IOR(EMPEG_MIXER_MAGIC, 24, int)
#define EMPEG_MIXER_READ_FM_DEEMPHASIS	_IOR(EMPEG_MIXER_MAGIC, 25, int)
#define EMPEG_MIXER_WRITE_FM_DEEMPHASIS	_IOW(EMPEG_MIXER_MAGIC, 25, int)

/* Retrieve volume level corresponding to 0dB */
#define EMPEG_MIXER_READ_ZERO_LEVEL _IOR(EMPEG_MIXER_MAGIC, 3, int)
#define EMPEG_MIXER_SELECT_FM		0
#define EMPEG_MIXER_SELECT_AM		1

#define EMPEG_MIXER_FLAG_MUTE		(1<<0)
/*#define EMPEG_MIXER_FLAG_LOUDNESS (1<<1)*/

/* Radio IOCTLs */
/* These are in addition to those provided by the Video4Linux API */
/* Hmm, not sure why we started at 73 but might as well stick to it */
#define EMPEG_RADIO_MAGIC		'r'
#define EMPEG_RADIO_READ_MONO		_IOR(EMPEG_RADIO_MAGIC, 73, int)
#define EMPEG_RADIO_WRITE_MONO		_IOW(EMPEG_RADIO_MAGIC, 73, int)
#define EMPEG_RADIO_READ_DX		_IOR(EMPEG_RADIO_MAGIC, 74, int)
#define EMPEG_RADIO_WRITE_DX		_IOW(EMPEG_RADIO_MAGIC, 74, int)
#define EMPEG_RADIO_READ_SENSITIVITY	_IOR(EMPEG_RADIO_MAGIC, 75, int)
#define EMPEG_RADIO_WRITE_SENSITIVITY	_IOW(EMPEG_RADIO_MAGIC, 75, int)
#define EMPEG_RADIO_SEARCH		_IO(EMPEG_RADIO_MAGIC, 76) /* Pass in direction in *arg */
#define EMPEG_RADIO_GET_MULTIPATH	_IOR(EMPEG_RADIO_MAGIC, 77, int)
#define EMPEG_RADIO_SET_STEREO		_IOW(EMPEG_RADIO_MAGIC, 78, int)
#define EMPEG_RADIO_READ_RAW		_IOR(EMPEG_RADIO_MAGIC, 79, int)

#define EMPEG_DSP_BEEP			_IOW(EMPEG_DSP_MAGIC, 0, int)
#define EMPEG_DSP_PURGE			_IOR(EMPEG_DSP_MAGIC, 1, int)
#define EMPEG_DSP_VOLADJ		_IOW(EMPEG_DSP_MAGIC, 2, int)
#define EMPEG_DSP_GRAB_OUTPUT		_IOR(EMPEG_DSP_MAGIC, 3, int) /* must be the same in 2.4 */

/* Audio input IOCTLs */
#define EMPEG_AUDIOIN_MAGIC		'c'
#define EMPEG_AUDIOIN_READ_SAMPLERATE	_IOR(EMPEG_AUDIOIN_MAGIC, 0, int)
#define EMPEG_AUDIOIN_WRITE_SAMPLERATE	_IOW(EMPEG_AUDIOIN_MAGIC, 1, int)
#define EMPEG_AUDIOIN_READ_CHANNEL	_IOR(EMPEG_AUDIOIN_MAGIC, 2, int)
#define EMPEG_AUDIOIN_WRITE_CHANNEL	_IOW(EMPEG_AUDIOIN_MAGIC, 3, int)
#define EMPEG_AUDIOIN_READ_STEREO	_IOR(EMPEG_AUDIOIN_MAGIC, 4, int)
#define EMPEG_AUDIOIN_WRITE_STEREO	_IOW(EMPEG_AUDIOIN_MAGIC, 5, int)
#define EMPEG_AUDIOIN_READ_GAIN		_IOR(EMPEG_AUDIOIN_MAGIC, 6, int)
#define EMPEG_AUDIOIN_WRITE_GAIN	_IOW(EMPEG_AUDIOIN_MAGIC, 7, int)
#define EMPEG_AUDIOIN_CHANNEL_DSPOUT	0
#define EMPEG_AUDIOIN_CHANNEL_AUXIN	1
#define EMPEG_AUDIOIN_CHANNEL_MIC	2

/* Power control IOCTLs */
#define EMPEG_POWER_MAGIC		'p'
#define EMPEG_POWER_TURNOFF		_IO(EMPEG_POWER_MAGIC, 0)
#define EMPEG_POWER_WAKETIME		_IOW(EMPEG_POWER_MAGIC, 1, int)
#define EMPEG_POWER_READSTATE		_IOR(EMPEG_POWER_MAGIC, 2, int)

#define EMPEG_POWER_FLAG_DC 		0x01
#define EMPEG_POWER_FLAG_FAILENABLED	0x02
#define EMPEG_POWER_FLAG_ACCESSORY	0x04
#define EMPEG_POWER_FLAG_FIRSTBOOT	0x08
#define EMPEG_POWER_FLAG_EXTMUTE	0x10
#define EMPEG_POWER_FLAG_LIGHTS		0x20
#define EMPEG_POWER_FLAG_DISPLAY       	0x40

/* State storage ioctls */
/* Shouldn't need either of these in normal use. */
#define EMPEG_STATE_MAGIC		's'
#define EMPEG_STATE_FORCESTORE		_IO(EMPEG_STATE_MAGIC, 74)
#define EMPEG_STATE_FAKEPOWERFAIL	_IO(EMPEG_STATE_MAGIC, 75)

/* RDS ioctls */
#define EMPEG_RDS_MAGIC			'R'
#define EMPEG_RDS_GET_INTERFACE		_IOR(EMPEG_RDS_MAGIC, 0, int)
#define EMPEG_RDS_SET_INTERFACE		_IOW(EMPEG_RDS_MAGIC, 0, int)

#define EMPEG_RDS_INTERFACE_OFF		0
#define EMPEG_RDS_INTERFACE_COOKED	1
#define EMPEG_RDS_INTERFACE_RAW		2

#define EMPEG_RAMTEST_MAGIC		'T'
#define EMPEG_RAMTEST_TEST_PAGE		_IOW(EMPEG_RAMTEST_MAGIC, 0, unsigned long)

#ifndef __ASSEMBLY__
struct empeg_eq_section_t
{
	unsigned int word1;
	unsigned int word2;
};

struct empeg_ramtest_args_t
{
	unsigned long addr;
	unsigned long ret;
};
#endif /* !defined(__ASSEMBLY__) */

#endif /* _INCLUDE_EMPEG_H */
