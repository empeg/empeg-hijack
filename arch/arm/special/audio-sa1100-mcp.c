/*
 * Itsy (SA1100+UCB1200) Audio Device Driver
 * Copyright (c) 1998,1999 Carl Waldspurger
 *
 * $Log: audio-sa1100-mcp.c,v $
 * Revision 2.2  1999/03/05 21:38:35  caw
 * Added support for output mixing and sampling-rate conversion.
 * Other minor improvements.
 *
 * Revision 2.1  1999/02/19  19:42:27  caw
 * Initial checkin for OSS-compliant driver.
 *
 */

/*
 * Overview
 *
 * This driver supports audio I/O on the Itsy platform, which
 * uses a StrongARM SA1100 coupled with a Philips UCB1200 codec.
 * It started out as a simple hack, but has been extended and
 * substantially rewritten several times to support features
 * requested by Itsy developers and partners.
 *
 * The current version supports ioctls for both an Itsy-specific 
 * native API and the OSS "open sound system" API.  Clients are
 * allowed to issue ioctl requests from either API, or even a 
 * mixture of both.  It is recommended that clients use the OSS
 * interface when possible for portability, and use the native
 * interface only for operations that are not supported by OSS.
 *
 * Unlike the OSS model, which generally allows only one active
 * client, this driver supports any number of simultaneous clients
 * for both microphone input and speaker output.  Microphone input
 * is replicated for all active input clients, and speaker output
 * is mixed for all active output clients.
 *
 * Note that the UCB1200 supports only a single sampling rate for
 * both input and output.  The current driver handles per-client
 * output rates, and transparently performs sampling-rate conversion
 * when necessary.  This sample-rate conversion uses a crude 
 * linear interpolation approach; for better results, perform your
 * own mixing in user-space by running sox or some other tool.  The
 * current implementation is also fairly inefficient, but since audio
 * processing generally consumes less than 1% of the 200MHz Itsy CPU,
 * I'm not terribly motivated to optimize it.  Sample-rate conversion
 * is not yet implemented for microphone input; however, clients can
 * use the native AUDIO_LOCK_RATE ioctl to maintain a desired rate.
 *
 */

/*
 * "to do" list
 *
 *   - sampling-rate conversion for mic input
 *   - add OSS mmap support
 *   - additional OSS-compatibility testing
 *   - expand overview documentation
 *
 *   - reduce overhead for sampling-rate conversion and mixing
 *   - implement DMA support
 *   - remove all button-watching hacks
 *
 */

/*
 * debugging 
 *
 */

#define	AUDIO_DEBUG			(1)
#define	AUDIO_DEBUG_VERBOSE		(0)

#define	AUDIO_DEBUG_MIC			(1)
#define	AUDIO_DEBUG_MIC_VERBOSE		(0)

/*
 * includes
 *
 */

#include <linux/config.h>

#ifdef	MODULE
#include <linux/module.h>
#include <linux/version.h>
#endif

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/sched.h>
#include <linux/major.h>
#include <linux/ctype.h>
#include <linux/wrapper.h>
#include <linux/errno.h>

#ifdef	CONFIG_PROC_FS
#include <linux/stat.h>
#include <linux/proc_fs.h>
#endif

#ifdef	CONFIG_POWERMGR
#include <linux/powermgr.h>
#endif

#include <asm/segment.h>
#include <asm/uaccess.h>
#include <asm/arch/irqs.h>

#include <linux/itsy_audio.h>

#include "mcp-sa1100.h"
#include "mcp_common.h"

//#ifdef CONFIG_SA1100_ITSY
#include "itsy_util.h"
//#endif

/*
 * constants
 *
 */

/* names */
#define	AUDIO_NAME			"audio-mcp"
#define	AUDIO_NAME_VERBOSE		"SA1100 MCP audio driver"
#define	AUDIO_VERSION_STRING		"$Revision: 2.2 $"

/* device numbers */
#define	AUDIO_MAJOR			(42)

/* interrupt numbers */
#define	AUDIO_IRQ			IRQ_Ser4MCP

/* client parameters */
#define	AUDIO_BUFFER_SIZE_DEFAULT	(4 * 1024)
#define	AUDIO_CLIENT_ID_INVALID		(-1)
#define	AUDIO_CLIENT_NBUFS_MIN		(2)
#define	AUDIO_CLIENT_NBUFS_MAX		(16)
#define	AUDIO_CLIENT_NBUFS_DEFAULT	(AUDIO_CLIENT_NBUFS_MIN)

/* mic defaults */
#define	MIC_BUF_META_LG_PAGES		(MIC_MMAP_META_LG_PAGES)
#define	MIC_BUF_META_NBYTES		(MIC_MMAP_META_NBYTES)
#define	MIC_BUF_META_NOFFSET		(MIC_MMAP_META_NOFFSET)
#define	MIC_BUF_LG_PAGES		(MIC_MMAP_DATA_LG_PAGES)
#define	MIC_BUF_NBYTES			(MIC_MMAP_DATA_NBYTES)
#define	MIC_BUF_NSAMPLES		(MIC_MMAP_DATA_NSAMPLES)
#define	AUDIO_MIC_TIMEOUT_JIFFIES	(8 * HZ)
#define	MIC_BUF_MIN_CLIENT_XFER		(64)

/* locks */
#define	AUDIO_RATE_UNLOCKED		(AUDIO_CLIENT_ID_INVALID)

/* rate conversion constants */
#define	AUDIO_RATE_CVT_FRAC_BITS	(16)
#define	AUDIO_RATE_CVT_FRAC_UNITY	(1 << AUDIO_RATE_CVT_FRAC_BITS)
#define	AUDIO_RATE_CVT_FRAC_MASK	(AUDIO_RATE_CVT_FRAC_UNITY - 1)

/* codec registers */
#define	CODEC_REG_IO_DATA		(0)
#define	CODEC_REG_IO_DIRECTION		(1)
#define	CODEC_REG_AUDIO_CTL_A		(7)
#define	CODEC_REG_AUDIO_CTL_B		(8)
#define	CODEC_REG_ADC_CTL		(10)
#define	CODEC_REG_MODE			(13)

/* codec register fields and values */
#define	CODEC_ASD_NUMERATOR		(375000)
#define	CODEC_AUDIO_ATT_MASK		(0x001f)
#define	CODEC_AUDIO_DIV_MASK		(0x007f)
#define	CODEC_AUDIO_GAIN_MASK		(0x0f80)
#define	CODEC_AUDIO_LOOP		(0x0100)
#define CODEC_AUDIO_MUTE		(0x2000)
#define CODEC_AUDIO_IN_ENABLE		(0x4000)
#define CODEC_AUDIO_OUT_ENABLE		(0x8000)
#define	CODEC_AUDIO_OFFSET_CANCEL	(1 << 13)

/* itsy-specific codec register values */
#define	CODEC_ITSY_IO_DIRECTIONS	(0x8200)
#define	CODEC_ITSY_IO_BUTTONS		(0x01ff)

/*
 * types
 *
 */

typedef unsigned char uchar;

typedef int audio_client_id;
typedef enum { AUDIO_INPUT, AUDIO_OUTPUT } audio_io_dir;

typedef struct {
  /* buffer */
  char *data;
  int  size;

  /* valid data */
  int  next;
  int  count;

  /* stream position (in samples) */
  audio_offset offset;
} audio_buf;

typedef struct {
  /* buffer management */
  struct wait_queue *waitq;
  char  *meta_data;
  short *data;
  ulong ndata;
  ulong nbytes;
  uint  lg_pages;
  uint  nmmap;

  /* masks for periodic events */
  uint  lg_wakeup;
  ulong wakeup_mask;
  ulong next_mask;
  ulong inactive_mask;
} mic_buf;

typedef struct {
  /* memory-mapped mcp */
  mcp_reg *mcp;

  /* mcp common id */
  mcp_common_id mcp_id;

  /* codec register values */
  uint audio_ctl_a;
  uint audio_ctl_b;
  uint codec_mode;

  /* common parameters */
  uint attenuation;
  uint gain;
  uint rate;
  uint loop;
  uint offset_cancel;

  /* common mic buffer */
  mic_buf *mic;

  /* unique client id generator */
  audio_client_id client_id_next;

  /* clients */
  int nclients;
  audio_client_id rate_locker;
  
#ifdef	CONFIG_POWERMGR
  /* power management */
  powermgr_id pm_id;
  ulong last_active;
#endif
} audio_shared;

typedef struct audio_client {
  /* linked list */
  struct audio_client *prev, *next;

  /* identifier */
  audio_client_id id;

  /* output buffer management */
  struct wait_queue *waitq;
  audio_buf buf[AUDIO_CLIENT_NBUFS_MAX];
  int buf_size;
  int nbufs;
  int intr_buf;
  int copy_buf;

  /* sample format */
  uint format;
  uint format_lg_bytes;

  /* output parameters */
  uint mix_volume;
  uint rate;

  /* rate-conversion state */
  short rate_cvt_prev;
  short rate_cvt_next;
  uint  rate_cvt;
  ulong rate_cvt_ratio;
  ulong rate_cvt_pos;

  /* stream position (in samples) */
  audio_offset offset;

  /* dropped samples */
  audio_offset dropped;
  int drop_fail;

  /* flags */
  uint reset;
  uint pause;

  /* XXX hack: button state */
  audio_button_match buttons_match;
  int buttons_watch;
} audio_client;

typedef struct {
  audio_client *input;
  audio_client *output;
} audio_io_client;

typedef struct {
  /* device management */
  audio_shared *shared;
  audio_io_dir io_dir;
  int handle_interrupts;
  int fifo_interrupts;

  /* client management */
  audio_client *clients;
  int nclients;

  /* client activity timestamp */
  ulong client_io_timestamp;

  /* stream position (in samples) */
  audio_offset offset;

  /* statistics */
  audio_stats stats;
} audio_dev;

/*
 * globals
 *
 */

static audio_shared shared_global;
static audio_dev audio_global;
static audio_dev mic_global;

#ifdef	CONFIG_PROC_FS
static int audio_get_info(char *, char **, off_t, int, int);
static struct proc_dir_entry audio_proc_entry = {
  0, 5, "audio", S_IFREG | S_IRUGO, 1, 0, 0, 0, NULL, audio_get_info,
};
#endif	/* CONFIG_PROC_FS */

/*
 * instantiate list operations
 *
 */

INSTANTIATE_LIST_INSERT(audio_client)
INSTANTIATE_LIST_REMOVE(audio_client)

/*
 * utility operations
 *
 */

static int lg(uint value)
{
  /*
   * modifies: nothing
   * effects:  Returns the base-two logarithm of value.
   *
   */

  int result = 0;

  while (value > 1)
    {
      result++;
      value >>= 1;
    }

  return(result);
}

/*
 * audio_format operations
 *
 */

static inline uint audio_format_channels(uint format)
{
  int chan = format & AUDIO_FORMAT_CHAN_MASK;
  return((chan == AUDIO_MONO) ? 1 : 2);
}

static inline uint audio_format_bits(uint format)
{
  int size = format & AUDIO_FORMAT_SIZE_MASK;
  return((size == AUDIO_8BIT) ? 8 : 16);
}

static inline uint audio_format_bytes(uint format)
{
  uint bits = audio_format_bits(format) * audio_format_channels(format);
  return(bits / 8);
}

/*
 * audio_offset operations
 *
 */

static inline ulong audio_offset_low32(audio_offset offset)
{
  return((ulong) (offset & 0xffffffff));
}

static inline audio_offset audio_offset_get_user(const audio_offset *ptr)
{
  /* copyin from user space */
  audio_offset value;
  copy_from_user((void *) &value, (const void *) ptr, sizeof(audio_offset));
  return(value);
}

static inline void audio_offset_put_user(audio_offset value, audio_offset *ptr)
{
  /* copyout to user space */
  copy_to_user((void *) ptr, (const void *) &value, sizeof(audio_offset));
}

/*
 * procfs operations
 *
 */

#ifdef	CONFIG_PROC_FS
static int client_get_info(char *buf, const audio_client *c)
{
  int len, i;

  /* client header */
  len = sprintf(buf,
		"client %d: copy=%d intr=%d\n",
		c->id, c->copy_buf, c->intr_buf);

  /* client buffers */
  for (i = 0; i < c->nbufs; i++)
    {
      const audio_buf *b = &c->buf[i];
      len += sprintf(buf + len,
		     "   buf %d: size=%4d next=%4d count=%4d off=%ld\n",
		     i, b->size, b->next, b->count,
		     audio_offset_low32(b->offset));
    }

  return(len);
}

static int audio_get_info(char *buf, 
			  char **start,
			  off_t offset,
			  int length,
			  int unused)
{
  audio_dev *dev_in  = &mic_global;
  audio_dev *dev_out = &audio_global;
  audio_shared *shared = &shared_global;

  audio_stats *stats;
  audio_client *c;
  int len = 0;

  /* shared stats */
  len += sprintf(buf + len,
		 "audio shared:\n"
		 "  device-major: %8d\n"
		 "  sample-rate:  %8u\n"
		 "  rate-locked:  %8s\n"
		 "  attenuation:  %8d\n"
		 "  gain:         %8d\n"
		 "  nclients:     %8d\n",
		 AUDIO_MAJOR,
		 shared->rate,
		 (shared->rate_locker == AUDIO_RATE_UNLOCKED) ? "no" : "yes",
		 shared->attenuation,
		 shared->gain,
		 shared->nclients);

  /* input stats */
  stats = &dev_in->stats;
  len += sprintf(buf + len,
		 "\n"
		 "audio input:\n"
		 "  enabled:      %8s\n"
		 "  interrupts:   %8lu\n"
		 "  wakeups:      %8lu\n"
		 "  samples:      %8lu\n"
		 "  overruns:     %8lu\n",
		 dev_in->handle_interrupts ? "yes" : "no",
		 stats->interrupts,
		 stats->wakeups,
		 stats->samples,
		 stats->fifo_err);
  for (c = dev_in->clients; c != NULL; c = c->next)
    len += client_get_info(buf + len, c);

  /* output stats */
  stats = &dev_out->stats;
  len += sprintf(buf + len,
		 "\n"
		 "audio output:\n"
		 "  enabled:      %8s\n"
		 "  interrupts:   %8lu\n"
		 "  wakeups:      %8lu\n"
		 "  samples:      %8lu\n"
		 "  underruns:    %8lu\n",
		 dev_out->handle_interrupts ? "yes" : "no",
		 stats->interrupts,
		 stats->wakeups,
		 stats->samples,
		 stats->fifo_err);
  for (c = dev_out->clients; c != NULL; c = c->next)
    len += client_get_info(buf + len, c);

  return(len);  
}
#endif	/* CONFIG_PROC_FS */

/*
 * mcp/codec operations
 *
 */

static inline void mcp_mcsr_wait_read(mcp_reg *mcp)
{
  /* wait for outstanding read to complete */
  while ((mcp->mcsr & MCSR_M_CRC) == 0)  { /* spin */ }
}

static inline void mcp_mcsr_wait_write(mcp_reg *mcp)
{
  /* wait for outstanding write to complete */
  while ((mcp->mcsr & MCSR_M_CWC) == 0)  { /* spin */ }
}

static int codec_read(mcp_reg *mcp, int addr)
{
  ulong flags;
  int data; 

  /* critical section */
  save_flags_cli(flags);
  {
    mcp_mcsr_wait_write(mcp);
    mcp->mcdr2 = ((addr & 0xf) << MCDR2_V_RN);
    mcp_mcsr_wait_read(mcp);
    data = mcp->mcdr2 & 0xffff;
  }
  restore_flags(flags);

  return(data);
}

static void codec_write(mcp_reg *mcp, int addr, int data)
{
  ulong flags;

  /* critical section */
  save_flags_cli(flags);
  {
    mcp_mcsr_wait_write(mcp);
    mcp->mcdr2 = ((addr & 0xf) << MCDR2_V_RN) | MCDR2_M_nRW | (data & 0xffff);
  }
  restore_flags(flags);
}

static inline void mcp_write_interrupt_enable(mcp_reg *mcp)
{
  /* set audio transmit FIFO interrupt enable */
  mcp->mccr |= MCCR_M_ATE;
}

static inline void mcp_write_interrupt_disable(mcp_reg *mcp)
{
  /* clear audio transmit FIFO interrupt enable */
  mcp->mccr &= ~MCCR_M_ATE;
}

static inline void mcp_read_interrupt_enable(mcp_reg *mcp)
{
  /* set audio receive FIFO interrupt enable */
  mcp->mccr |= MCCR_M_ARE;
}

static inline void mcp_read_interrupt_disable(mcp_reg *mcp)
{
  /* clear audio receive FIFO interrupt enable */
  mcp->mccr &= ~MCCR_M_ARE;
}

static uint mcp_read_buttons(mcp_reg *mcp)
{
  /*
   * modifies: mcp
   * effects:  Returns current button state from GPIO input ports.
   *           Bit set = "button down", bit clear = "button up".
   * caveats:  All button-related code is a legacy hack, and will
   *           eventually be removed from the audio driver.
   *
   */

  /* XXX setup - need to coordinate with Larry to avoid doing repeatedly */
  codec_write(mcp, CODEC_REG_IO_DIRECTION, CODEC_ITSY_IO_DIRECTIONS);
  codec_write(mcp, CODEC_REG_IO_DATA, 0);
  
  /* grab button state */
  return((~codec_read(mcp, CODEC_REG_IO_DATA)) & CODEC_ITSY_IO_BUTTONS);
}



#ifdef CONFIG_SA1100_TIFON

/* 
 * Tifon specific routines to
 * enable/disable hardware
 */

static unsigned int switch_status = SPKR_ON | MIKE1_EN;


static void disable_speakers(audio_dev *dev)
{
	/* disable all the speakers */
	GPSR = (7 << 2);
}

static void enable_speakers(audio_dev *dev)
{
	disable_speakers(dev);
	if( switch_status & EEAR_ON )
		GPCR = (1 << 2);
	if( switch_status & IEAR_ON )
		GPCR = (1 << 3);
	if( switch_status & SPKR_ON)
		GPCR = (1 << 4);
}

static void disable_microphones(audio_dev *dev)
{
	/* disable all microphones */
	unsigned int data;
	data = codec_read(dev->shared->mcp,0);
	codec_write(dev->shared->mcp, 0, data & (3<<8));
}

static void enable_microphones(audio_dev *dev)
{
	unsigned int data;
	data = codec_read(dev->shared->mcp,0);
	data &= ~(3<<8);
	if( switch_status & MIKE1_EN )
		data |= 1<<9;
	if( switch_status & MIKE2_EN )
		data |= 1<<8;
	codec_write(dev->shared->mcp, 0, data);
}

#endif


/*
 * audio_shared operations
 *
 */

static void audio_shared_init(audio_shared *shared)
{
  /* initialize shared state */
  KOBJ_INIT(shared, audio_shared);

  /* set defaults */
  shared->mcp  = MCP_BASE_V;
  shared->mcp_id = MCP_COMMON_ID_INVALID;
  shared->rate = AUDIO_RATE_DEFAULT;
  shared->rate_locker = AUDIO_RATE_UNLOCKED;
  shared->gain = AUDIO_GAIN_DEFAULT;
  shared->attenuation = AUDIO_ATT_DEFAULT;

#ifdef	CONFIG_POWERMGR
  /* initialize powermgr state */
  shared->pm_id = POWERMGR_ID_INVALID;
  shared->last_active = jiffies;
#endif	/* CONFIG_POWERMGR */
}

static int audio_shared_rate_lock(audio_shared *shared, audio_client_id id)
{
  /*
   * modifies: shared
   * effects:  Attempts to acquire sample rate lock for specified client.
   *           Returns TRUE iff lock successfully acquired.
   *           
   */

  if ((shared->rate_locker == AUDIO_RATE_UNLOCKED) ||
      (shared->rate_locker == id))
    {
      shared->rate_locker = id;
      return(1);
    }

  return(0);
}

static int audio_shared_rate_unlock(audio_shared *shared, audio_client_id id)
{
  /*
   * modifies: shared
   * effects:  Attempts to release sample rate lock by specified client.
   *           Returns TRUE iff lock successfully released.
   *           
   */

  if ((shared->rate_locker == AUDIO_RATE_UNLOCKED) ||
      (shared->rate_locker == id))
    {
      shared->rate_locker = AUDIO_RATE_UNLOCKED;
      return(1);
    }

  return(0);
}

static int audio_shared_rate_mutable(const audio_shared *shared, 
				     audio_client_id id)
{
  /*
   * modifies: nothing
   * effects:  Returns TRUE iff the audio sample rate can be changed by
   *           the client specified by id.
   *
   */

  return((shared->rate_locker == AUDIO_RATE_UNLOCKED) ||
	 (shared->rate_locker == id));
}

static void audio_shared_set_attenuation(audio_shared *shared, uint att)
{
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_shared_set_attenuation: att=%u\n", att);

  /* set codec attenuation */
  shared->audio_ctl_b &= ~CODEC_AUDIO_ATT_MASK;
  shared->audio_ctl_b |= att & CODEC_AUDIO_ATT_MASK;
  codec_write(shared->mcp, CODEC_REG_AUDIO_CTL_B, shared->audio_ctl_b);

  /* update shared state */
  shared->attenuation = att;
}

static void audio_shared_set_gain(audio_shared *shared, uint gain)
{
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_shared_set_gain: gain=%u\n", gain);

  /* set codec gain */
  shared->audio_ctl_a &= ~CODEC_AUDIO_GAIN_MASK;
  shared->audio_ctl_a |= (gain << 7) & CODEC_AUDIO_GAIN_MASK;
  codec_write(shared->mcp, CODEC_REG_AUDIO_CTL_A, shared->audio_ctl_a);

  /* update shared state */
  shared->gain = gain;
}

static int audio_shared_set_loop(audio_shared *shared, uint enable)
{
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_shared_set_loop: enable=%u\n", enable);

  /* set codec loopback mode */
  if (enable)
    shared->audio_ctl_b |= CODEC_AUDIO_LOOP;
  else
    shared->audio_ctl_b &= ~CODEC_AUDIO_LOOP;
  codec_write(shared->mcp, CODEC_REG_AUDIO_CTL_B, shared->audio_ctl_b);

  /* update shared state */
  shared->loop = enable;

  /* everything OK */
  return(0);
}

static int audio_shared_set_offset_cancel(audio_shared *shared, uint enable)
{
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_set_offset_cancel: enable=%u\n", enable);  

  /* set codec offset cancel mode */
  if (enable)
    shared->codec_mode |= CODEC_AUDIO_OFFSET_CANCEL;
  else
    shared->codec_mode &= ~CODEC_AUDIO_OFFSET_CANCEL;    
  codec_write(shared->mcp, CODEC_REG_MODE, shared->codec_mode);
  
  /* update shared state */
  shared->offset_cancel = enable;

  /* everything OK */
  return(0);
}

/*
 * audio_buf operations
 *
 */

static inline void audio_buf_init(audio_buf *buf)
{
  /* initialize */
  buf->data   = NULL;
  buf->size   = 0;
  buf->next   = 0;
  buf->count  = 0;
  buf->offset = 0;
}

static inline void audio_buf_set(audio_buf *buf, int next, int count)
{
  /* reset */
  buf->next  = next;
  buf->count = count;
}

static inline int audio_buf_done(const audio_buf *buf)
{
  /* returns TRUE iff audio buf is complete */
  return(buf->next >= buf->count);
}

static inline void audio_buf_dealloc(audio_buf *buf)
{
  /* reclaim buffer storage */
  if (buf->data != NULL)
    {
      kfree_s(buf->data, buf->size);
      buf->data = NULL;
    }
}

/*
 * audio_client operations
 *
 */

static void audio_client_destroy(audio_client *client)
{
  /*
   * modifies: client
   * effects:  Reclaims all storage associated with client.
   *
   */

  int i;

  /* sanity check */
  if (client == NULL)
    return;

  /* deallocate buffers */
  for (i = 0; i < client->nbufs; i++)
    audio_buf_dealloc(&client->buf[i]);

  /* deallocate container */
  KOBJ_DEALLOC(client, audio_client);
}

static audio_client *audio_client_create(int nbufs, int buf_size)
{
  /*
   * modifies: nothing
   * effects:  Creates and returns a new, initialized audio_client object
   *           with nbufs buffers each containing buf_size bytes.
   *           Returns NULL iff unable to allocate storage.
   *
   */

  audio_client *client;
  int i;

  /* allocate client, fail if unable */
  if ((client = KOBJ_ALLOC(audio_client)) == NULL)
    return(NULL);

  /* initialize client */
  KOBJ_INIT(client, audio_client);
  client->id = AUDIO_CLIENT_ID_INVALID;

  /* initialize client format */
  client->format = AUDIO_FORMAT_DEFAULT;
  client->format_lg_bytes = lg(audio_format_bytes(client->format));

  /* initialize client output parameters */
  client->mix_volume = AUDIO_OUTPUT_MIX_VOLUME_DEFAULT;
  client->rate = AUDIO_RATE_DEFAULT;

  /* initialize client rate-conversion state */
  client->rate_cvt = 0;
  client->rate_cvt_ratio = 0;
  client->rate_cvt_pos = AUDIO_RATE_CVT_FRAC_UNITY;  /* XXX */

  /* initialize reset flag */
  client->reset = 1;

  /* allocate, initialize client buffers */
  for (i = 0; i < nbufs; i++)
    audio_buf_init(&client->buf[i]);
  if (buf_size > 0)
    for (i = 0; i < nbufs; i++)
      {
	audio_buf *buf = &client->buf[i];
	buf->size = buf_size;
	if ((buf->data = (char *) kmalloc(buf_size, GFP_KERNEL)) == NULL)
	  {
	    /* cleanup and fail */
	    audio_client_destroy(client);
	    return(NULL);
	  }
      }

  /* intialize buffer info */
  client->buf_size = buf_size;
  client->nbufs = nbufs;

  /* everything OK */
  return(client);
}
  
static int audio_client_get_mix_volume(const audio_client *client, uint *v)
{
  /*
   * requires: v is a user-space pointer
   * modifies: v
   * effects:  Sets v to the mix volume associated with client.
   *
   */

  /* sanity check */
  if (client == NULL)
    return(-EINVAL);

  /* everything OK */
  put_user(client->mix_volume, v);
  return(0);
}
  
static int audio_client_set_mix_volume(audio_client *client, uint v)
{
  /*
   * modifies: client
   * effects:  Sets the mix volume associated with client to v.
   *           Returns 0 iff successful, otherwise error code.
   *
   */

  /* sanity check */
  if (client == NULL)
    return(-EINVAL);

  /* range check */
  if ((v < AUDIO_OUTPUT_MIX_VOLUME_MIN) || (v > AUDIO_OUTPUT_MIX_VOLUME_MAX))
    return(-EINVAL);

  /* set mix volume */
  client->mix_volume = v;

  /* everything OK */
  return(0);
}

static int audio_client_get_buffer_size(const audio_client *client, uint *size)
{
  /*
   * requires: size is a user-space pointer
   * modifies: size
   * effects:  Sets size to the buffer size associated with client, in bytes.
   *
   */

  /* sanity check */
  if (client == NULL)
    return(-EINVAL);

  /* everything OK */
  put_user(client->buf_size, size);
  return(0);
}
  
static int audio_client_set_buffer_size(audio_client *client, 
					int nbufs,
					int nbytes)
{
  /*
   * modifies: client
   * effects:  Attempts to set the number of buffers (aka fragments)
   *           to nbufs and the size of each buffer to nbytes for client.
   *           Returns 0 iff successful, otherwise error code.
   *
   */

  /* forward declaration */
  static int audio_client_write_flush(audio_client *);

  char *buf[AUDIO_CLIENT_NBUFS_MAX];
  int i, j;

  /* sanity checks */
  if ((client == NULL) ||
      (nbufs  < AUDIO_CLIENT_NBUFS_MIN)    ||
      (nbufs  > AUDIO_CLIENT_NBUFS_MAX)    ||
      (nbytes < AUDIO_OUTPUT_BUF_SIZE_MIN) || 
      (nbytes > AUDIO_OUTPUT_BUF_SIZE_MAX))
    return(-EINVAL);
  
  /* flush pending client data, abort if interrupted */
  if (audio_client_write_flush(client) == -EINTR)
    return(-EINTR);

  /* attempt to allocate new buffers */
  for (i = 0; i < nbufs; i++)
    if ((buf[i] = (char *) kmalloc(nbytes, GFP_KERNEL)) == NULL)
      {
	/* cleanup and fail */
	for (j = 0; j < i; j++)
	  kfree_s(buf[j], nbytes);
	return(-ENOMEM);
      }

  /* dealloc old buffers, install new buffers */
  for (i = 0; i < client->nbufs; i++)
    if (client->buf[i].data != NULL)
      kfree_s(client->buf[i].data, client->buf[i].size);

  /* install new buffers */
  for (i = 0; i < nbufs; i++)
    {
      audio_buf_init(&client->buf[i]);
      client->buf[i].data = buf[i];
      client->buf[i].size = nbytes;
    }

  /* set buffer config */
  client->nbufs    = nbufs;
  client->buf_size = nbytes;

  /* everything OK */
  return(0);
}

static uint audio_client_output_nbuffered_prim(const audio_client *client)
{
  /*
   * modifies: nothing
   * effects:  Returns the number of output-buffered samples
   *           currently pending for client.
   *
   */

  ulong flags;
  uint nbytes;

  /* initialize */
  nbytes = 0;

  /* critical section */
  save_flags_cli(flags);
  {
    int i;
    for (i = 0; i < client->nbufs; i++)
      if (!audio_buf_done(&client->buf[i]))
	nbytes += client->buf[i].count - client->buf[i].next;
  }
  restore_flags(flags);

  /* everything OK */
  return(nbytes >> client->format_lg_bytes);
}


static int audio_client_output_nbuffered(const audio_client *client,
					 uint *count)
{
  /*
   * requires: count is a user-space pointer
   * modifies: count
   * effects:  Sets count to the number of output-buffered samples
   *           currently pending for client.
   *
   */

  uint nsamples;

  /* sanity check */
  if (client == NULL)
    return(-EINVAL);

  /* obtain information */
  nsamples = audio_client_output_nbuffered_prim(client);

  /* everything OK */
  put_user(nsamples, count);
  return(0);
}

static audio_client *audio_dev_client_create(audio_dev *dev)
{
  /*
   * modifies: dev
   * effects:  Creates a new, initialized client of dev.
   *           Returns the new client iff successful, otherwise NULL.
   *
   */

  const int nbufs = AUDIO_CLIENT_NBUFS_DEFAULT;
  audio_client *client;
  audio_client_id id;
  int buf_size;
  ulong flags;

  /* choose buffer size, mic clients do not use buffers */
  buf_size = AUDIO_BUFFER_SIZE_DEFAULT;
  if (dev->io_dir == AUDIO_INPUT)
    buf_size = 0;

  /* allocate new client, fail if unable */
  if ((client = audio_client_create(nbufs, buf_size)) == NULL)
    return(NULL);
  
  /* generate new client id */
  client->id = dev->shared->client_id_next++;

  /* critical section */
  save_flags_cli(flags);
  {
    /* add to client list */
    audio_client_insert(&dev->clients, client);
    dev->nclients++;
    dev->shared->nclients++;
  }
  restore_flags(flags);

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_dev_client_create: id=%u\n", id);

  /* everything OK */
  return(client);
}

static int audio_dev_client_destroy(audio_dev *dev, audio_client *client)
{
  /*
   * modifies: dev
   * effects:  Detaches client from dev, and reclaims all associated storage.
   *           Returns 0 iff successful, otherwise error code.
   *
   */

  ulong flags;

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_dev_client_destroy: id=%d\n", client->id);

  /* critical section */
  save_flags_cli(flags);
  {
    int i;

    /* wait until client no longer active */
    for (i = 0; i < client->nbufs; i++)
      while (!audio_buf_done(&client->buf[i]))
	interruptible_sleep_on(&client->waitq);
    
    /* remove from client list */
    audio_client_remove(&dev->clients, client);
    dev->nclients--;
    dev->shared->nclients--;
  }
  restore_flags(flags);

  /* reclaim storage */
  audio_client_destroy(client);

  /* everything OK */
  return(0);
}

/*
 * audio_io_client operations
 *
 */

static audio_io_client *audio_io_client_create(audio_client *input,
					       audio_client *output)
{
  /*
   * modifies: nothing
   * effects:  Creates and returns a new, initialized audio_io_client
   *           object containing specified input and output clients.
   *           Returns NULL iff unable to allocate storage.
   *
   */

  audio_io_client *client;

  /* allocate and initialize client, fail if unable */
  if ((client = KOBJ_ALLOC(audio_io_client)) == NULL)
    return(NULL);
  KOBJ_INIT(client, audio_io_client);

  /* attach input and output clients */
  client->input  = input;
  client->output = output;

  return(client);
}

static void audio_io_client_destroy(audio_io_client *client)
{
  /*
   * modifies: clients
   * effects:  Reclaims all storage associated with client.
   *
   */

  /* reclaim container */
  KOBJ_DEALLOC(client, audio_io_client);
}

/*
 * audio_dev operations
 *
 */

static void audio_dev_init(audio_dev *dev,
			   audio_shared *shared,
			   audio_io_dir io_dir)
{
  /* initialize */
  KOBJ_INIT(dev, audio_dev);

  /* device management */
  dev->shared   = shared;
  dev->io_dir   = io_dir;

  /* client management */
  dev->clients  = NULL;
  dev->nclients = 0;
  
  /* stream position */
  dev->offset   = 0;
}

static inline audio_offset audio_dev_get_offset(const audio_dev *dev)
{
  /*
   * modifies: nothing
   * effects:  Returns a consistent snapshot of the current stream
   *           position associated with dev.
   *
   */

  audio_offset offset;
  ulong flags;

  /* critical section */
  save_flags_cli(flags);
  {
    offset = dev->offset;
  }
  restore_flags(flags);

  return(offset);
}

static void audio_output_enable(audio_dev *dev)
{
  audio_shared *shared = dev->shared;

  /* enable interrupt processing */
  dev->handle_interrupts = 1;

  /* enable audio output */
  shared->audio_ctl_b |= CODEC_AUDIO_OUT_ENABLE;
  codec_write(shared->mcp, CODEC_REG_AUDIO_CTL_B, shared->audio_ctl_b);  

  /* XXX other params need reset? */
  /* reset attenuation after every enable */
  audio_shared_set_attenuation(shared, shared->attenuation);

  /* enable fifo ready interrupts */
  mcp_write_interrupt_enable(shared->mcp);
  dev->fifo_interrupts = 1;

#ifdef CONFIG_SA1100_TIFON
  /* enable speakers */
  enable_speakers(dev);
#endif

}

static void audio_output_disable(audio_dev *dev)
{
  audio_shared *shared = dev->shared;

  /* disable fifo ready interrupts */
  mcp_write_interrupt_disable(shared->mcp);
  dev->fifo_interrupts = 0;

  /* disable audio output */
  shared->audio_ctl_b &= ~CODEC_AUDIO_OUT_ENABLE;
  codec_write(shared->mcp, CODEC_REG_AUDIO_CTL_B, shared->audio_ctl_b);  

  /* disable interrupt processing */
  dev->handle_interrupts = 0;

#ifdef CONFIG_SA1100_TIFON
  /* disable speakers */
  disable_speakers(dev);
#endif

}

static void audio_input_enable(audio_dev *dev)
{
  audio_shared *shared = dev->shared;

  /* enable interrupt processing */
  dev->handle_interrupts = 1;

  /* enable audio input */
  shared->audio_ctl_b |= CODEC_AUDIO_IN_ENABLE;
  codec_write(shared->mcp, CODEC_REG_AUDIO_CTL_B, shared->audio_ctl_b);  

  /* XXX other params need reset?  */
  /* reset gain after every enable */
  audio_shared_set_gain(shared, shared->gain);

  /* enable fifo ready interrupts */
  mcp_read_interrupt_enable(shared->mcp);
  dev->fifo_interrupts = 1;

#ifdef CONFIG_SA1100_TIFON
  /* enable microphones */
  enable_microphones(dev);
#endif
}

static void audio_input_disable(audio_dev *dev)
{
  audio_shared *shared = dev->shared;

  /* disable fifo ready interrupts */
  mcp_read_interrupt_disable(shared->mcp);
  dev->fifo_interrupts = 0;

  /* disble audio input */
  shared->audio_ctl_b &= ~CODEC_AUDIO_IN_ENABLE;
  codec_write(shared->mcp, CODEC_REG_AUDIO_CTL_B, shared->audio_ctl_b);  

  /* disable interrupt processing */
  dev->handle_interrupts = 0;

#ifdef CONFIG_SA1100_TIFON
  /* disable microphones */
  disable_microphones(dev);
#endif
}

static inline int audio_client_buttons_match(const audio_client *client)
{
  /*
   * modifies: nothing
   * effects:  Returns TRUE iff client watching buttons, and the
   *           button state matches the specified up/down masks.
   *
   */

  audio_shared *shared = &shared_global;

  if (client->buttons_watch)
    {  
      uint buttons = mcp_read_buttons(shared->mcp);
      int match = ((buttons    & client->buttons_match.down_mask) ||
		   ((~buttons) & client->buttons_match.up_mask));

      if (AUDIO_DEBUG_VERBOSE)
	printk(AUDIO_NAME ": match: b=%x, dm=%x, um=%x, m=%d\n",
	       buttons, 
	       client->buttons_match.down_mask,
	       client->buttons_match.up_mask,
	       match);

      return(match);
    }

  return(0);
}

static int audio_client_watch_buttons(audio_client *client, void *arg)
{
  /*
   * requires: arg is a user-space pointer
   * modifies: client
   * effects:  Sets client button state masks for future read/write ops.
   *           If both masks are zero, stops all button matching.
   *           Returns 0 if successful, otherwise error code.
   *
   */

  /* copy argument from user space */
  copy_from_user((void *) &client->buttons_match,
		(const void *) arg,
		sizeof(audio_button_match));
  
  /* watch buttons if either mask set */
  client->buttons_watch =
    (client->buttons_match.up_mask   != 0) ||
    (client->buttons_match.down_mask != 0);

  if (AUDIO_DEBUG_VERBOSE)
    printk("audio_client_watch_buttons: arg=%lx, um=%x, dm=%x, w=%d\n",
	   (ulong) arg,
	   client->buttons_match.up_mask,
	   client->buttons_match.down_mask,
	   client->buttons_watch);

  /* everything OK */
  return(0);
}

static int audio_client_lock_rate(audio_client *client, int lock)
{
  /*
   * modifies: dev
   * effects:  If lock is set, attempts to acquire audio rate lock for client.
   *           If lock unset,  attempts to unlock  audio rate lock for client.
   *           Returns 0 iff successful, else error code.
   *
   */

  audio_shared *shared = &shared_global;

  if (lock)
    {
      /* attempt to acquire lock */
      if (audio_shared_rate_lock(shared, client->id))
	return(0);
    }
  else
    {
      /* attempt to release lock */
      if (audio_shared_rate_unlock(shared, client->id))
	return(0);
    }

  /* unable to update lock */
  return(-EACCES);
}

static void audio_set_rate_prim(audio_shared *shared, uint rate)
{
  /*
   * modifies: shared
   * effects:  Sets shared audio sampling rate to rate.
   *
   */

  audio_dev *audio = &audio_global;
  audio_dev *mic   = &mic_global;
  uint asd, old_enables;
  ulong flags;

  /* critical section */
  save_flags_cli(flags);
  {
    /* disable I/O while changing rate */
    old_enables = shared->audio_ctl_b;
    audio_output_disable(audio);
    audio_input_disable(mic);

    asd = (CODEC_ASD_NUMERATOR / rate) & CODEC_AUDIO_DIV_MASK;
    shared->mcp->mccr   = MCCR_M_ADM | MCCR_M_MCE | (40 << MCCR_V_TSD)  | asd;
    shared->audio_ctl_a = (shared->audio_ctl_a & ~CODEC_AUDIO_DIV_MASK) | asd;
    codec_write(shared->mcp, CODEC_REG_AUDIO_CTL_A, shared->audio_ctl_a);

    /* restore after changing rate */
    if (old_enables & CODEC_AUDIO_OUT_ENABLE)
      audio_output_enable(audio);
    if (old_enables & CODEC_AUDIO_IN_ENABLE)
      audio_input_enable(mic);

    /* update shared rate state */
    shared->rate = rate;
  }
  restore_flags(flags);

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_set_rate_prim: reenabled I/O (I=%d,O=%d)\n",
	   old_enables & CODEC_AUDIO_IN_ENABLE,
	   old_enables & CODEC_AUDIO_OUT_ENABLE);
}

static int audio_client_set_rate(audio_client *client, uint rate)
{
  /*
   * modifies: audio
   * effects:  Attempts to set audio sampling rate to rate for client.
   *           Returns 0 iff successful, else error code.
   *
   */

  audio_shared *shared = &shared_global;

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_set_rate: rate=%u\n", rate);

  /* set local client rate */
  client->rate = rate;

  /* if possible, set global shared rate */
  if (audio_shared_rate_mutable(shared, client->id))
    audio_set_rate_prim(shared, rate);

  /* everything OK */
  return(0);
}

static int audio_client_get_rate(audio_client *client, uint *rate)
{
  /*
   * requires: rate is a user-space pointer
   * modifies: rate
   * effects:  Sets rate to current audio sampling rate.
   *
   */

  audio_shared *shared = &shared_global;

  /* copyout reply value */
  put_user(shared->rate, rate);
  return(0);
}

static int audio_client_get_position(audio_client *client,
				     audio_offset *offset)
{
  /*
   * requires: offset is a user-space pointer
   * modifies: offset
   * effects:  Sets offset to the current stream position (in samples)
   *           associated with client.
   *
   */

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_client_get_position\n");

  /* copyout reply value */
  audio_offset_put_user(client->offset, offset);
  return(0);
}

static void audio_hardware_init(audio_shared *shared)
{
  mcp_reg *mcp = shared->mcp;
  int asd;

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_hardware_init\n");

  /* turn on the MCP */
  (void) mcp_common_enable(shared->mcp_id);

  /* disable audio I/O, clear options */
  shared->audio_ctl_b = 0;
  codec_write(mcp, CODEC_REG_AUDIO_CTL_B, shared->audio_ctl_b);

  /* set default gain, audio sample rate divisor */
  asd = CODEC_ASD_NUMERATOR / AUDIO_RATE_DEFAULT;
  shared->audio_ctl_a = (AUDIO_GAIN_DEFAULT << 7) | asd;
  codec_write(mcp, CODEC_REG_AUDIO_CTL_A, shared->audio_ctl_a);

#ifdef CONFIG_SA1100_TIFON
  /* direction of audio switch control pins */
  GPDR |= 0x1c;
  codec_write( mcp, 1, codec_read( mcp, 1) | (3<<8));
#endif


}

static void audio_hardware_shutdown(audio_shared *shared)
{
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_hardware_shutdown\n");

  /* turn off the MCP */
  (void) mcp_common_disable(shared->mcp_id);
}

/*
 * sample conversion
 *
 */

static inline uchar codec_sample_to_char(short sample)
{
  /* 
   * modifies: nothing
   * effects:  Converts a 16-bit CODEC audio sample to 8-bit audio sample.
   *
   */

  /* codec sample is 12-bits, left-aligned in 16-bit field */
  return(((sample >> 8) + 0x80) & 0xff);
}

static inline short codec_sample_from_char(uchar sample)
{
  /* 
   * modifies: nothing
   * effects:  Converts an 8-bit audio sample to 16-bit CODEC sample.
   *
   */

  /* codec sample is 12-bits, left-aligned in 16-bit field */
  short cvt = sample;
  return(((cvt - 0x80) << 8) & ~0xf);
}

/*
 * mic_buf operations
 *
 */

static void mic_buf_destroy(mic_buf *mic)
{
  /*
   * modifies: mic
   * effects:  Reclaims all storage associated with mic.
   *
   */

  ulong reserve_start, reserve_end;

  /* sanity check */
  if (mic == NULL)
    return;

  /* deallocate metadata buffer */
  if (mic->meta_data != NULL)
    {
      /* mark page unreserved */
      reserve_start = (ulong) mic->meta_data;
      mem_map_unreserve(MAP_NR(reserve_start));
      
      /* deallocate buffer */
      free_pages((ulong) mic->meta_data, MIC_BUF_META_LG_PAGES);
    }

  /* deallocate circular buffer */
  if (mic->data != NULL)
    {
      int i;
      
      /* mark pages unreserved */
      reserve_start = (ulong) mic->data;
      reserve_end   = reserve_start + (mic->nbytes - 1);
      for (i = MAP_NR(reserve_start); i <= MAP_NR(reserve_end); i++)
	mem_map_unreserve(i);

      /* deallocate buffer */
      free_pages((ulong) mic->data, mic->lg_pages);
      mic->data = NULL;
    }

  /* deallocate container */
  KOBJ_DEALLOC(mic, mic_buf);
}

static mic_buf *mic_buf_create(void)
{
  /*
   * modifies: nothing
   * effects:  Creates and returns a new, initialized mic_buf object.
   *           Returns NULL iff unable to allocate storage.
   *
   */

  ulong reserve_start, reserve_end;
  mic_buf *mic;
  int i;

  /* allocate and initialize container */
  if ((mic = KOBJ_ALLOC(mic_buf)) == NULL)
    return(NULL);
  KOBJ_INIT(mic, mic_buf);

  /* allocate and initialize metadata buffer */
  mic->meta_data = (char *) __get_dma_pages(GFP_KERNEL, MIC_BUF_META_LG_PAGES);
  if (mic->meta_data == NULL)
    {
      /* cleanup and fail */
      mic_buf_destroy(mic);
      return(NULL);
    }
  memset((void *) mic->meta_data, 0, PAGE_SIZE);

  /* mark metadata page reserved to support mmap */
  reserve_start = (ulong) mic->meta_data;
  mem_map_reserve(MAP_NR(reserve_start));

  /* allocate and initialize circular buffer */
  mic->lg_pages = MIC_BUF_LG_PAGES;
  mic->ndata  = MIC_BUF_NSAMPLES;
  mic->nbytes = mic->ndata * sizeof(short);
  mic->data = (short *) __get_dma_pages(GFP_KERNEL, mic->lg_pages);
  if (mic->data == NULL)
    {
      /* cleanup and fail */
      mic_buf_destroy(mic);
      return(NULL);
    }
  memset((void *) mic->data, 0, mic->nbytes);

  /* mark buffer pages reserved to support mmap */
  reserve_start = (ulong) mic->data;
  reserve_end   = reserve_start + (mic->nbytes - 1);
  for (i = MAP_NR(reserve_start); i <= MAP_NR(reserve_end); i++)
    mem_map_reserve(i);

  /* initialize masks */
  mic->lg_wakeup     = AUDIO_INPUT_LG_WAKEUP_DEFAULT;
  mic->wakeup_mask   = (1 << mic->lg_wakeup) - 1;
  mic->next_mask     = MIC_BUF_NSAMPLES - 1;
  mic->inactive_mask = mic->next_mask;

  /* everything OK */
  return(mic);
}

static int mic_buf_set_wakeup_mask(mic_buf *mic, int lg_wakeup)
{
  /* sanity check */
  if (mic == NULL)
    return(-EINVAL);

  /* range check */
  if ((lg_wakeup < AUDIO_INPUT_LG_WAKEUP_MIN) ||
      (lg_wakeup > AUDIO_INPUT_LG_WAKEUP_MAX))
    return(-EINVAL);

  /* everything OK */
  mic->lg_wakeup   = lg_wakeup;
  mic->wakeup_mask = (1 << mic->lg_wakeup) - 1;
  return(0);
}

static int mic_buf_get_wakeup_mask(const mic_buf *mic, uint *lg_wakeup)
{
  /*
   * requires: lg_wakeup is a user-space pointer
   * modifies: lg_wakeup
   * effects:  XXX
   *
   */

  /* sanity check */
  if (mic == NULL)
    return(-EINVAL);

  /* copyout reply value */
  put_user(mic->lg_wakeup, lg_wakeup);
  return(0);
}

static int audio_client_input_nbuffered(const audio_client *client,
					uint *count)
{
  /*
   * requires: count is a user-space pointer
   * modifies: count
   * effects:  Sets count to the number of input-buffered samples
   *           currently pending in internal buffers for client.
   *
   */

  audio_dev *dev = &mic_global;
  mic_buf *mic = dev->shared->mic;
  audio_offset behind;
  uint nsamples;

  /* sanity check */
  if (client == NULL)
    return(-EINVAL);

  /* compute client lag */
  behind = audio_dev_get_offset(dev) - client->offset;

  /* compute number of buffered samples */
  nsamples = MIN(behind, mic->ndata);

  /* everything OK */
  put_user(nsamples, count);
  return(0);
}

static int audio_client_get_dropped(const audio_client *client,
				    audio_offset *count)
{
  /*
   * requires: count is a user-space pointer
   * modifies: count
   * effects:  Sets count to the number of input samples dropped by client.
   *
   */

  /* sanity check */
  if (client == NULL)
    return(-EINVAL);

  /* copyout reply value */
  audio_offset_put_user(client->dropped, count);
  return(0);
}

static int audio_client_set_dropped(audio_client *client,
				    const audio_offset *count)
{
  /*
   * requires: count is a user-space pointer
   * modifies: client
   * effects:  Sets the number of input samples dropped by client to count.
   *
   */

  /* sanity check */
  if (client == NULL)
    return(-EINVAL);

  /* copy argument from user space */
  client->dropped = audio_offset_get_user(count);
  return(0);
}

static int audio_client_input_set_position(audio_dev *dev,
					   audio_client *client,
					   audio_offset *offset)
{
  /*
   * requires: offset is a user-space pointer
   * modifies: client, offset
   * effects:  Sets stream offset associated with client to the
   *           current offset associated with dev plus offset.
   *           Updates offset to contain the modified client value.
   *           Returns 0 iff successful, otherwise error code.
   *
   */

  audio_offset delta;

  /* sanity check */
  if (client == NULL)
    return(-EINVAL);
  
  /* copy argument from user space */
  delta = audio_offset_get_user(offset);
  
  /* update client stream position */
  client->offset = audio_dev_get_offset(dev) + delta;

  /* update argument value */
  audio_offset_put_user(client->offset, offset);

  /* everything OK */
  return(0);
}

static int audio_client_set_drop_fail(audio_client *client, int fail)
{
  /*
   * modifies: client
   * effects:  Specifies client behavior when samples are dropped.
   *           If fail is set, dropped samples will result in EIO
   *           errors for relevant system calls.
   *
   */

  /* update client state */
  client->drop_fail = fail;

  /* everything OK */
  return(0);
}

static void mic_buf_copy_to_user(const mic_buf *mic,
				 char *to, 
				 audio_offset start,
				 int nsamples,
				 uint format)
{
  /*
   * requires: to is a user-space pointer
   * modifies: to
   * effects:  Transfers nsamples of audio data to user-space buffer in
   *           the specified format, performing conversions if necessary.
   *
   */

  const short *from;
  short *to_16;
  ulong next, mask;
  int i, j;

  if (AUDIO_DEBUG_VERBOSE || AUDIO_DEBUG_MIC_VERBOSE)
    printk(AUDIO_NAME ": mic_buf_copy_to_user: "
	   "start=%lu, nsamples=%d, format=%x\n",
	   audio_offset_low32(start), nsamples, format);

  /* avoid unnecessary work */
  if (nsamples <= 0)
    return;

  /* compute next buffer slot */
  from = mic->data;
  mask = mic->next_mask;
  next = start & mask;

#if	0
  /* debugging */
  if (AUDIO_DEBUG_MIC_VERBOSE)
    {
      static short mic_buf_debug[MIC_BUF_NSAMPLES];
      static int   mic_buf_debug_init = 0;

      if (!mic_buf_debug_init)
	{
	  for (i = 0; i < MIC_BUF_NSAMPLES; i++)
	    mic_buf_debug[i] = i;
	  mic_buf_debug_init = 1;
	}

      from = mic_buf_debug;
    }
#endif

  /* copy samples to user space, converting if necessary */
  switch (format) {
  case (AUDIO_8BIT_MONO):
    /* convert each 12-bit CODEC sample to 8-bit sample */
    for (i = 0; i < nsamples; i++, next++)
      put_user(codec_sample_to_char(from[next & mask]), &to[i]);
    break;

  case (AUDIO_8BIT_STEREO):
    /* convert each 12-bit CODEC sample to two 8-bit samples */
    for (i = 0, j = 0; i < nsamples; i++, j += 2, next++)
      {
	uchar sample = codec_sample_to_char(from[next & mask]);
	put_user(sample, &to[j]);
	put_user(sample, &to[j+1]);
      }
    break;

  case (AUDIO_16BIT_MONO):
    /* no conversion necessary */
    to_16 = (short *) to;
    for (i = 0; i < nsamples; i++, next++)
      put_user(from[next & mask], &to_16[i]);
    break;

  case (AUDIO_16BIT_STEREO):
    /* ouput identical sample for both channels */
    to_16 = (short *) to;
    for (i = 0, j = 0; i < nsamples; i++, j += 2, next++)
      {
	put_user(from[next & mask], &to_16[j]);
	put_user(from[next & mask], &to_16[j + 1]);
      }
    break;

  default:
    /* invalid format */
    if (AUDIO_DEBUG)
      printk(AUDIO_NAME ": mic_buf_copy_to_user: bad format=%x\n", format);
    break;
  }
}


/*
 * interrupt processing
 *
 */

static int audio_client_get_sample(audio_client *c, short *sample)
{
  /*
   * modifies: c, sample
   * effects:  XXX
   *           Returns TRUE iff sample obtained.
   *
   */

  audio_buf *buf;
  ulong flags;
  short s;

  /* no samples if client paused */
  if (c->pause)
    return(0);

  save_flags_cli(flags);
  {
    buf = &c->buf[c->intr_buf];
    if (audio_buf_done(buf) && (buf->count > 0))
      {
	/* advance to next buffer */
	c->intr_buf++;
	if (c->intr_buf >= c->nbufs)
	  c->intr_buf = 0;
	buf = &c->buf[c->intr_buf];
	
	/* wake up waiter */
	wake_up_interruptible(&c->waitq);
      }

    /* client too far behind */
    if (audio_buf_done(buf))
      {
	restore_flags(flags);
	return(0);
      }
  }
  restore_flags(flags);

  switch (c->format) {
  case (AUDIO_8BIT_MONO):
    s = codec_sample_from_char(buf->data[buf->next++]);
    break;

  case (AUDIO_8BIT_STEREO):
    s  = codec_sample_from_char(buf->data[buf->next++]) / 2;
    s += codec_sample_from_char(buf->data[buf->next++]) / 2;
    s &= ~0xf;
    break;

  case (AUDIO_16BIT_MONO):
    s = *((short *) &buf->data[buf->next]);
    buf->next += 2;
    break;

  case (AUDIO_16BIT_STEREO):
    s  = (*((short *) &buf->data[buf->next]))     / 2;
    s += (*((short *) &buf->data[buf->next + 2])) / 2;
    s &= ~0xf;
    buf->next += 4;
    break;

  default:
    /* not reached */
    return(0);
  }

  /* everything OK */
  *sample = s;
  return(1);
}

static int audio_client_interpolate_sample(audio_client *c,
					   short *sample,
					   uint rate)
{
  long pos, frac_prev, frac_next, mix;

  /* special case: avoid unnecessary work */
  if (c->rate == rate)
    return(audio_client_get_sample(c, sample));

  /* compute input:output sample rate ratio */
  if (c->rate_cvt != rate)
    {
      c->rate_cvt_ratio  = (c->rate * AUDIO_RATE_CVT_FRAC_UNITY) / rate;  
      c->rate_cvt = rate;
    }
  
  /* update [prev, next] to contain interpolatation point */
  for (pos = c->rate_cvt_pos + c->rate_cvt_ratio;
       pos > AUDIO_RATE_CVT_FRAC_UNITY;
       pos -= AUDIO_RATE_CVT_FRAC_UNITY)
    {
      short s;

      /* fail if unable to get raw sample */
      if (!audio_client_get_sample(c, &s))
	return(0);

      c->rate_cvt_prev = c->rate_cvt_next;
      c->rate_cvt_next = s;
    }
  c->rate_cvt_pos = pos;

  /* linear interpolatation between prev and next */
  frac_next = pos & AUDIO_RATE_CVT_FRAC_MASK;
  frac_prev = AUDIO_RATE_CVT_FRAC_UNITY - frac_next;
  mix = (c->rate_cvt_prev * frac_prev) + (c->rate_cvt_next * frac_next);
  *sample = (short) (mix / AUDIO_RATE_CVT_FRAC_UNITY);

  /* everything OK */
  return(1);
}

static void mic_handle_interrupt(audio_dev *dev)
{
  /*
   * modifies: dev
   * effects:  Handle codec receive interrupt.  Records available samples
   *           in a special circular buffer associated with dev.  Wakes
   *           up clients waiting for more data, and updates statistics.
   *
   */

  mcp_reg *mcp = dev->shared->mcp;
  mic_buf *mic = dev->shared->mic;
  int count;

#ifdef	CONFIG_POWERMGR
  /* update last active time */
  dev->shared->last_active = jiffies;
#endif

  /* update statistics */
  dev->stats.interrupts++;

  /* sanity check */
  if (mic == NULL)
    {
      if (AUDIO_DEBUG)
	printk(AUDIO_NAME ": mic_handle_interrupt: mic=NULL\n");	
      return;
    }

  /* transfer data from codec */
  for (count = 0; mcp->mcsr & MCSR_M_ANE; count++)
    {
      /* compute next buffer slot */
      ulong next = dev->offset & mic->next_mask;

      /* capture sample into buffer */
      mic->data[next] = mcp->mcdr0;

      /* advance stream position */
      dev->offset++;

      /* check for inactivity every so often */
      if ((next & mic->inactive_mask) == 0)
	{
	  ulong flags;

	  if (AUDIO_DEBUG_MIC_VERBOSE)
	    printk(AUDIO_NAME ": mic_handle_interrupt: off=%lu, next=%lu\n",
		   audio_offset_low32(dev->offset), next);

	  /* critical section */
	  save_flags_cli(flags);
	  {
	    /* disable mic input after period of inactivity */
	    if (mic->nmmap == 0)
	      if ((jiffies - dev->client_io_timestamp) >
		  AUDIO_MIC_TIMEOUT_JIFFIES)
		{
		  if (AUDIO_DEBUG_VERBOSE)
		    printk("mic_handle_interrupt: mic timeout\n");
		  audio_input_disable(dev);
		}
	  }
	  restore_flags(flags);
	}

      /* wakeup waiters every so often */
      if ((next & mic->wakeup_mask) == 0)
	{
	  /* update statistics */
	  dev->stats.wakeups++;
	  
	  /* wake up waiters */
	  wake_up_interruptible(&mic->waitq);
	}
    }
  
  /* expose stream position to mmap clients */
  ((audio_input_mmap_meta *) mic->meta_data)->next = dev->offset;

  /* update statistics */
  dev->stats.samples += count;
}

static void audio_handle_interrupt(audio_dev *dev)
{
  /*
   * modifies: dev
   * effects:  XXX
   *           Handle transmit interrupts.
   *
   */

  mcp_reg *mcp = dev->shared->mcp;
  audio_client *c;
  ulong flags;

#ifdef	CONFIG_POWERMGR
  /* update last active time */
  dev->shared->last_active = jiffies;
#endif

  /* update statistics */
  dev->stats.interrupts++;

  /* write samples while FIFO not full */
  while (mcp->mcsr & MCSR_M_ANF)
    {
      long sum_sample = 0, sum_volume = 0, nmix = 0;
      short sample;

      /* accumulate volume-weighted samples for all clients */
      for (c = dev->clients; c != NULL; c = c->next)
	if (audio_client_interpolate_sample(c, &sample, dev->shared->rate))
	  {
	    sum_sample += sample * c->mix_volume;
	    sum_volume += c->mix_volume;
	    nmix++;
	  }

      if (nmix == 0)
	{
	  /* no clients: disable output */
	  save_flags_cli(flags);
	  {
	    audio_output_disable(dev);
	  }
	  restore_flags(flags);
	  return;
	}

      /* play adjusted sample */
      sample = (short) (sum_sample / sum_volume);
      mcp->mcdr0 = sample;

      /* update stream position */
      dev->offset++;

      /* update statistics */
      dev->stats.samples++;
    }
}

static void generic_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
  audio_shared *shared = &shared_global;
  audio_dev *dev_in    = &mic_global;
  audio_dev *dev_out   = &audio_global;

  /* capture mcsr status on entry */
  uint mcsr = shared->mcp->mcsr;

  /* clear interrupts (FIFO underrun/overrun) */
  shared->mcp->mcsr = MCSR_M_ATU | MCSR_M_ARO;

  if (AUDIO_DEBUG_VERBOSE)
  {
    static int count = 0;
    if ((count++ & 0xff) == 0)
      printk(AUDIO_NAME ": generic_interrupt: count=%d\n", count);      
  }

  /* handle receive interrupts */
  if (dev_in->handle_interrupts)
    {
      if (mcsr & MCSR_M_ARO)
	dev_in->stats.fifo_err++;
      mic_handle_interrupt(dev_in);
    }
  
  /* handle transmit interrupts */
  if (dev_out->handle_interrupts)
    {
      if (mcsr & MCSR_M_ATU)
	dev_out->stats.fifo_err++;
      audio_handle_interrupt(dev_out);
    }
}

/*
 * file operations
 *
 */

static int audio_client_write_flush(audio_client *client)
{
  ulong flags;
  int i;

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_write_flush: client %d: wait\n", client->id);

  /* critical section */
  save_flags_cli(flags);
  {
    /* wait for all buffers to completely drain */
    for (i = 0; i < client->nbufs; i++)
      while (!audio_buf_done(&client->buf[i]))
	{
	  interruptible_sleep_on(&client->waitq);
	  if (signal_pending(current))
	    {
	      /* abort if interrupted */
	      restore_flags(flags);
	      return(-EINTR);
	    }
	}
  }
  restore_flags(flags);

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_write_flush: client %d: done\n", client->id);
  
  /* everything OK */
  return(0);
}

static int audio_fsync(struct file *file, struct dentry *dentry)
{
  audio_io_client *io_client = (audio_io_client *) file->private_data;
  audio_client *client = io_client->output;

  /* fail if not output client */
  if (client == NULL)
    return(-EINVAL);

  /* flush all pending samples for output client */
  return(audio_client_write_flush(client));
}

static int audio_client_write_stop(audio_client *client)
{
  /*
   * modifies: client
   * effects:  Clears buffers associated with client.
   *           Returns number of cleared bytes, if any.
   *
   */

  int unwritten, i;
  ulong flags;

  /* initialize */
  unwritten = 0;

  /* critical section */
  save_flags_cli(flags);
  {
    /* mark all buffers empty */
    for (i = 0; i < client->nbufs; i++)
      {
	unwritten += (client->buf[i].count - client->buf[i].next);
	audio_buf_set(&client->buf[i], 0, 0);
      }
 
    /* reset buffer pointers */
    client->intr_buf = 0;
    client->copy_buf = 0;
  }
  restore_flags(flags);

  return(unwritten);
}

static size_t audio_write(struct file *file,
			  const char *buffer,
			  size_t count,
			  loff_t *posp)
{
  audio_io_client *io_client = (audio_io_client *) file->private_data;
  audio_client *client = io_client->output;
  audio_dev *dev = &audio_global;
  int total, err;
  ulong flags;

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_write: count=%d\n", count);

  /* verify user-space buffer validity */
  if ((err = verify_area(VERIFY_READ, (void *) buffer, count)) != 0)
    return(err);

  /* initialize total */
  total = 0;  
  
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_write: "
	   "client=%lx, copy_buf=%d, intr_buf=%d\n",
	   (ulong) client, client->copy_buf, client->intr_buf);

  while (count > 0)
    {
      audio_buf *buf = &client->buf[client->copy_buf];
      int chunk;

      /* update activity timestamp */
      dev->client_io_timestamp = jiffies;

      if (AUDIO_DEBUG_VERBOSE)
	printk(AUDIO_NAME ": audio_write: copy_buf=%d, intr_buf=%d\n",
	       client->copy_buf, client->intr_buf);

      /* wait for next buffer to drain */
      save_flags_cli(flags);
      while (!audio_buf_done(buf))
	{
	  /* don't wait if non-blocking */
	  if (file->f_flags & O_NONBLOCK)
	    {
	      restore_flags(flags);
	      return((total > 0) ? total : -EAGAIN);
	    }

	  if (AUDIO_DEBUG_VERBOSE)
	    printk(AUDIO_NAME ": audio_write: sleep_on\n");

	  /* next buffer full */
	  interruptible_sleep_on(&client->waitq);

	  if (AUDIO_DEBUG_VERBOSE)
	    printk(AUDIO_NAME ": audio_write: wake up\n");

	  /* handle interrupted system call (XXX or buttons) */
	  if (signal_pending(current) ||
	      audio_client_buttons_match(client))
	    {
	      /* incomplete write */
	      total -= audio_client_write_stop(client);
	      restore_flags(flags);
	      return((total > 0) ? total : -EINTR);
	    }
	}
      restore_flags(flags);

      /* transfer data from user-space in buffer-size chunks */
      chunk = MIN(count, buf->size);

      /* copy chunk of data from user-space */
      copy_from_user((void *) buf->data, (const void *) buffer, chunk);
      if (AUDIO_DEBUG_VERBOSE)
	printk(AUDIO_NAME ": audio_write: chunk=%d\n", chunk);      

      /* critical section */
      save_flags_cli(flags);
      {
	/* mark buffer full */
	audio_buf_set(buf, 0, chunk);

	/* enable interrupt-driven output */
	if (!dev->handle_interrupts)
	  audio_output_enable(dev);
      }
      restore_flags(flags);

      /* advance to next buffer */
      client->copy_buf++;
      if (client->copy_buf >= client->nbufs)
	client->copy_buf = 0;

      /* update counts */
      total  += chunk;
      buffer += chunk;
      count  -= chunk;

      /* update stream position */
      file->f_pos    += chunk;
      client->offset += (chunk >> client->format_lg_bytes);
    }

  /* write complete */
  return(total);
}

/*
 * mic mmap support
 *
 */

static void mic_vma_open(struct vm_area_struct *area)
{
  audio_dev *dev = &mic_global;  
  mic_buf *mic = dev->shared->mic;

  /* track references */
  dev->nclients++;
  mic->nmmap++;

#ifdef	MODULE
  MOD_INC_USE_COUNT;
#endif
}

static void mic_vma_close(struct vm_area_struct *area)
{
  /* forward declaration */
  static void audio_dev_shutdown(audio_dev *dev);
  
  audio_dev *dev = &mic_global;  
  mic_buf *mic = dev->shared->mic;

  /* track references */
  dev->nclients--;  
  mic->nmmap--;

  /* shutdown if no remaining clients */
  audio_dev_shutdown(dev);

#ifdef	MODULE
  MOD_DEC_USE_COUNT;
#endif
}

static struct vm_operations_struct mic_vm_ops = {
  mic_vma_open,
  mic_vma_close,
  /* rest default to NULL */
};

#if 0 	/* to be updated */
static int audio_mmap(struct file *file,
		      struct vm_area_struct *vma)
{
  audio_dev *dev = &mic_global;
  mic_buf *mic = dev->shared->mic;
  ulong size, flags;

  if (AUDIO_DEBUG_MIC_VERBOSE)
    printk(AUDIO_NAME ": audio_mmap: start=%lx, end=%lx\n", 
	   (ulong) vma->vm_start, 
	   (ulong) vma->vm_end);

  /* sanity checks */
  if (mic == NULL)
    return(-EIO);

  /* support read-only mappings */
  if (vma->vm_flags & VM_WRITE)
    {
      if (AUDIO_DEBUG_MIC)
	printk(AUDIO_NAME ": audio_mmap: invalid flag VM_WRITE (%x)\n",
	       vma->vm_flags);
      return(-EINVAL);
    }

  /* enforce proper size */
  size = vma->vm_end - vma->vm_start;
  if (size != sizeof(audio_input_mmap))
    {
      if (AUDIO_DEBUG_MIC)
	printk(AUDIO_NAME ": audio_mmap: invalid size %lu\n", size);
      return(-EINVAL);
    }

  /* remap data region */
  if (remap_page_range(vma->vm_start,
		       (ulong) mic->data,
		       MIC_BUF_NBYTES,
		       vma->vm_page_prot))
    return(-EAGAIN);

  /* remap meta region */
  if (remap_page_range(vma->vm_start + MIC_BUF_NBYTES,
		       (ulong) mic->meta_data,
		       MIC_BUF_META_NBYTES,
		       vma->vm_page_prot))
    return(-EAGAIN);

  /* sanity check */
  if (vma->vm_ops != NULL)
    return(-EINVAL);

  /* adjust references */
  vma->vm_ops = &mic_vm_ops;
  vma->vm_inode = inode;
  inode->i_count++;

  /* open(vma) wasn't called this time */
  mic_vma_open(vma);

  /* critical section */
  save_flags_cli(flags);
  {
    /* enable interrupt-driven input */
    if (!dev->handle_interrupts)
      audio_input_enable(dev);
  }
  restore_flags(flags);

  /* everything OK */
  return(0);
}
#endif

static int audio_read_dropped(const audio_dev *dev, audio_client *client)
{
  /*
   * modifies: client
   * effects:  If client is too far behind current device stream position,
   *           drops entire buffer; client skips over dropped samples.
   *           Returns TRUE iff samples were dropped.
   *
   */

  mic_buf *mic = dev->shared->mic;
  audio_offset offset, behind, dropped;

  /* snapshot stream position */
  offset = audio_dev_get_offset(dev);
  
  /* done if not too far behind */
  behind = offset - client->offset;
  if (behind <= mic->ndata)
    return(0);

  /* drop entire buffer, update stats */
  dropped = behind - mic->ndata;
  client->offset = offset;
  client->dropped += behind;
  
  /* debugging */
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_read_dropped: dropped %lu, total %lu\n",
	   audio_offset_low32(behind),
	   audio_offset_low32(client->dropped));
	  
  /* dropped samples */
  return(1);
}

static size_t audio_read(struct file *file,
			 char *buffer,
			 size_t count,
			 loff_t *posp)
{
  audio_io_client *io_client = (audio_io_client *) file->private_data;
  audio_client *client = io_client->input;
  audio_dev *dev = &mic_global;
  mic_buf *mic = dev->shared->mic;
  int total, err, interrupted;
  ulong flags;

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_read: count=%d\n", count);

  /* verify user-space buffer validity */
  if ((err = verify_area(VERIFY_WRITE, (void *) buffer, count)) != 0)
    return(err);

  /* enable interrupt-driven input */
  save_flags_cli(flags);
  {
    if (!dev->handle_interrupts)
      audio_input_enable(dev);
  }
  restore_flags(flags);

  /* initialize */
  interrupted = 0;
  total = 0;

  while ((count > 0) && (!interrupted))
    {
      int count_samples, chunk_bytes, chunk_samples, min_xfer;
      audio_offset offset, behind;

      /* update activity timestamp */
      dev->client_io_timestamp = jiffies;

      /* snapshot current stream position */
      offset = audio_dev_get_offset(dev);

      /* handle client resets */
      if (client->reset)
	{
	  /* warp current stream position */
	  client->offset = offset;
	  client->reset  = 0;
	}
      behind = offset - client->offset;

      /* debugging */
      if (AUDIO_DEBUG_MIC_VERBOSE)
	printk(AUDIO_NAME ": audio_read: doff=%lu, coff=%lu, behind=%lu\n",
	       audio_offset_low32(offset),
	       audio_offset_low32(client->offset),
	       audio_offset_low32(behind));

      /* wait until sufficient data ready for client transfer */
      min_xfer = MIN(count, MIC_BUF_MIN_CLIENT_XFER);
      if (behind < min_xfer)
	{
	  /* don't wait if non-blocking */
	  if (file->f_flags & O_NONBLOCK)
	    return((total > 0) ? total : -EAGAIN);

	  if (AUDIO_DEBUG_MIC_VERBOSE)
	    printk(AUDIO_NAME ": audio_read: wait\n");

	  /* wait for more data to become available */
	  interruptible_sleep_on(&mic->waitq);

	  /* handle interrupted system call (XXX or buttons) */
	  if (signal_pending(current) ||
	      audio_client_buttons_match(client))
	    interrupted = 1;
	}

      /* skip over dropped samples */
      if (audio_read_dropped(dev, client))
	{
	  /* fail with I/O error, if specified by client */
	  if (client->drop_fail)
	    return(-EIO);
	  continue;
	}

      /* snapshot current stream position */
      offset = audio_dev_get_offset(dev);
      behind = offset - client->offset;

      /* copy chunk of data to user-space */
      count_samples = count >> client->format_lg_bytes;
      chunk_samples = MIN(behind, count_samples);
      chunk_bytes = chunk_samples << client->format_lg_bytes;
      mic_buf_copy_to_user(mic,
			   buffer,
			   client->offset,
			   chunk_samples, 
			   client->format);

      /* ensure no samples dropped during copy */
      if (audio_read_dropped(dev, client))
	{
	  /* fail with I/O error, if specified by client */
	  if (client->drop_fail)
	    return(-EIO);
	  continue;
	}

      /* update counts */
      total  += chunk_bytes;
      buffer += chunk_bytes;
      count  -= chunk_bytes;
      
      /* update stream position */
      file->f_pos    += chunk_bytes;
      client->offset += chunk_samples;
      
      /* debugging */
      if (AUDIO_DEBUG_VERBOSE)
	printk(AUDIO_NAME ": audio_read: chunk_bytes=%d\n", chunk_bytes);
    }

  /* handle interrupted read */
  if (interrupted && (total == 0))
    return(-EINTR);

  /* read complete */
  return(total);
}

#if 0 	/* must be updated */
static int audio_poll ( struct struct file *file,
			poll_table_struct *table)
{
  audio_io_client *io_client = (audio_io_client *) file->private_data;

  /* handle select on output */
  if (mode == SEL_OUT)
    {
      audio_client *client = io_client->output;
      ulong flags;

      /* critical section */
      save_flags_cli(flags);
      {
	audio_buf *buf = &client->buf[client->copy_buf];
	if (audio_buf_done(buf))
	  {
	    /* ready for output */
	    restore_flags(flags);
	    return(1);
	  }

	/* debugging */
	if (AUDIO_DEBUG)
	  printk(AUDIO_NAME ": audio_select: waiting...\n");

	/* wait for free output buffer */
	select_wait(&client->waitq, table);
      }
      restore_flags(flags);

      /* not ready for output */
      return(0);
    }

  /* SEL_IN, SEL_EX not supported */
  return(0);
}
#endif

static int audio_client_get_format(audio_client *client, uint *format)
{
  /* copyout reply value */
  put_user(client->format, format);
  return(0);
}

static int audio_client_set_format(audio_client *client, uint format)
{
  /* fail if argument invalid */
  if ((format & AUDIO_FORMAT_MASK) != format)
    return(-EINVAL);

  /* set format */
  client->format = format;
  client->format_lg_bytes = lg(audio_format_bytes(format));

  /* everything OK */
  return(0);
}

static int audio_get_attenuation(audio_dev *dev, uint *attenuation)
{
  /* copyout reply value */
  put_user(dev->shared->attenuation, attenuation);
  return(0);
}

static int audio_set_attenuation_safe(audio_dev *dev, uint attenuation)
{
  /* ensure attenuation in valid range */
  if ((attenuation < AUDIO_ATT_MIN) || (attenuation > AUDIO_ATT_MAX))
    return(-EINVAL);

  /* set attenuation */
  audio_shared_set_attenuation(dev->shared, attenuation);
  return(0);
}

static int audio_get_gain(audio_dev *dev, uint *gain)
{
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_get_gain\n");

  /* copyout reply value */
  put_user(dev->shared->gain, gain);
  return(0);
}

static int audio_set_gain_safe(audio_dev *dev, uint gain)
{
  /* ensure gain in valid range */
  if ((gain < AUDIO_GAIN_MIN) || (gain > AUDIO_GAIN_MAX))
    return(-EINVAL);

  /* set gain */
  audio_shared_set_gain(dev->shared, gain);
  return(0);
}

static int audio_debug_hook(uint arg)
{
  /* 
   * modifies: nothing
   * effects:  General debugging hook.  Currently unused.
   *
   */

  /* everything OK */
  return(0);
}

static int audio_client_output_stop(audio_client *client)
{
  /*
   * modifies: client
   * effects:  Clears all pending output for client.
   *
   */

  /* clear client write buffers */
  (void) audio_client_write_stop(client);

  /* everything OK */
  return(0);
}

static int audio_output_pause(audio_dev *dev, audio_client *client, uint pause)
{
  /*
   * modifies: dev, client
   * effects:  If pause is set, pauses output associated with client.
   *           Otherwise resumes output associated with client.
   *
   */

  ulong flags;

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_output_pause: client %d, pause=%d\n",
	   client->id, pause);  

  /* critical section */
  save_flags_cli(flags);
  {
    /* set client flag */
    client->pause = pause;

    /* resume output, if appropriate */
    if (!pause)
      {
	/* enable interrupt-driven output */
	if (!dev->handle_interrupts)
	  audio_output_enable(dev);
      }
  }
  restore_flags(flags);

  /* everything OK */
  return(0);
}

/*
 * OSS compatibility routines 
 *
 */


static int audio_oss_reset(audio_io_client *io_client)
{
  int err;

  /* reset input */
  if (io_client->input != NULL)
    {
      /* clear dropped count, reset stream position */
      io_client->input->dropped = 0;
      io_client->input->reset = 1;
    }

  /* reset output */
  if (io_client->output != NULL)
    if ((err = audio_client_output_stop(io_client->output)) != 0)
      return(err);

  /* everything OK */
  return(0);
}

static int audio_oss_sync(audio_io_client *io_client)
{
  int err;

  /* sync input */
  if (io_client->input != NULL)
    {
      /* XXX */
    }

  /* sync output */
  if (io_client->output != NULL)
    if ((err = audio_client_write_flush(io_client->output)) != 0)
      return(err);

  /* everything OK */
  return(0);
}

static int audio_oss_set_speed(audio_io_client *io_client, int *arg)
{
  audio_shared *shared = &shared_global;
  int rate;

  /* copyin rate value */
  get_user(rate, arg);

  /* returning old setting and fail if invalid rate */
  if ((rate < AUDIO_RATE_MIN) || (rate > AUDIO_RATE_MAX))
    {
      put_user(shared->rate, (uint *) arg);
      return(-EINVAL);
    }

  /* flush pending output, if any */
  if (io_client->output != NULL)
    (void) audio_client_write_flush(io_client->output);

  /* set client rates */
  if (io_client->input  != NULL)
    (void) audio_client_set_rate(io_client->input,  rate);
  if (io_client->output != NULL)
    (void) audio_client_set_rate(io_client->output, rate);

  /* everything OK */
  return(0);
}

static int audio_oss_set_stereo_prim(audio_client *client, int *arg)
{
  int stereo, err;
  uint format;

  /* copyin stereo flag */
  get_user(stereo, arg);

  /* attempt to set format */
  format  = (client->format & AUDIO_FORMAT_SIZE_MASK);
  format |= (stereo ? AUDIO_STEREO : AUDIO_MONO);
  if ((err = audio_client_set_format(client, format)) != 0)
    {
      /* fail, returning old setting */
      put_user(audio_format_channels(client->format) - 1, arg);
      return(err);
    }

  /* everything OK */
  return(0);
}

static int audio_oss_set_stereo(audio_io_client *io_client, int *arg)
{
  int err;

  /* flush pending output, if any */
  if (io_client->output != NULL)
    (void) audio_client_write_flush(io_client->output);
      
  /* set input client format */
  if (io_client->input != NULL)
    if ((err = audio_oss_set_stereo_prim(io_client->input, arg)) != 0)
      return(err);

  /* set output client format */
  if (io_client->output != NULL)
    if ((err = audio_oss_set_stereo_prim(io_client->output, arg)) != 0)
      return(err);

  /* everything OK */
  return(0);
}

static int audio_oss_set_channels_prim(audio_client *client, int *arg)
{
  int channels, err;

  /* copyin channels value */
  get_user(channels, arg);

  err = -EINVAL;
  if ((channels == 1) || (channels == 2))
    {
      uint format;

      /* set desired format */
      format  = (client->format & AUDIO_FORMAT_SIZE_MASK);
      format |= ((channels == 2) ? AUDIO_STEREO : AUDIO_MONO);

      /* attempt to set stereo/mono */
      if ((err = audio_client_set_format(client, format)) == 0)
	return(0);
    }

  /* fail, returning old setting */
  put_user(audio_format_channels(client->format), arg);
  return(err);
}

static int audio_oss_set_channels(audio_io_client *io_client, int *arg)
{
  int err;

  /* flush pending output, if any */
  if (io_client->output != NULL)
    (void) audio_client_write_flush(io_client->output);

  /* set input client channels */
  if (io_client->input != NULL)
    if ((err = audio_oss_set_channels_prim(io_client->input, arg)) != 0)
      return(err);

  /* set input client channels */
  if (io_client->output != NULL)
    if ((err = audio_oss_set_channels_prim(io_client->output, arg)) != 0)
      return(err);

  /* everything OK */
  return(0);
}

static int audio_oss_set_sample_size_prim(audio_client *client, int *arg)
{
  int size, old, err;

  /* get desired size */
  err = -EINVAL;
  get_user(size, arg);
  if ((size == AFMT_S8) || (size == AFMT_S16_LE))
    {
      uint format;

      /* set desired format */
      format  = (client->format & AUDIO_FORMAT_CHAN_MASK);
      format |= ((size == AFMT_S8) ? AUDIO_8BIT : AUDIO_16BIT);

      /* flush output, attempt to set sample size */
      if ((err = audio_client_write_flush(client)) == 0)    
	if ((err = audio_client_set_format(client, format)) == 0)
	  return(0);
    }

  /* fail, returning old setting */
  old = (audio_format_bits(client->format) == 8) ? AFMT_S8 : AFMT_S16_LE;
  put_user(old, arg);
  return(err);
}

static int audio_oss_set_sample_size(audio_io_client *io_client, int *arg)
{
  int err;

  /* flush pending output, if any */
  if (io_client->output != NULL)
    (void) audio_client_write_flush(io_client->output);

  /* set input client sample size */
  if (io_client->input != NULL)
    if ((err = audio_oss_set_sample_size_prim(io_client->input, arg)) != 0)
      return(err);

  /* set input client sample size */
  if (io_client->output != NULL)
    if ((err = audio_oss_set_sample_size_prim(io_client->output, arg)) != 0)
      return(err);

  /* everything OK */
  return(0);
}

static int audio_oss_get_frag_size(audio_io_client *io_client, int *arg)
{
  audio_shared *shared = &shared_global;
  int frag_size_in, frag_size_out;

  /* initialize */
  frag_size_in = frag_size_out = 0;

  /* for input client, approx frag size with wakeup granularity */
  if (io_client->input != NULL)
    frag_size_in = (1 << shared->mic->lg_wakeup) * sizeof(short);

  /* for output client, obtain exact frag size */
  if (io_client->output != NULL)
    frag_size_out = io_client->output->buf_size;

  /* XXX OSS semantics if both input and output clients active? */
  if ((io_client->input != NULL) && (io_client->output != NULL))
    {
      /* reply with minimum fragment size */
      put_user(MIN(frag_size_in, frag_size_out), arg);
      return(0);
    }

  /* no ambiguity if single client */
  if (io_client->input  != NULL)
    put_user(frag_size_in,  arg);
  if (io_client->output != NULL)
    put_user(frag_size_out, arg);

  /* everything OK */
  return(0);
}

static int audio_oss_set_frag_size(audio_io_client *io_client, int *arg)
{
  int value, frag_lg_size, nfrags_max, err;

  /* copyin value 0xMMMMSSSS */
  get_user(value, arg);

  /* decode fragment information */
  frag_lg_size = (value & 0xffff);
  nfrags_max   = (value >> 16);

  /* XXX OSS semantics if both input and output clients active? */

  /* configure output buffers */
  if (io_client->output != NULL)
    {
      int nbufs  = MIN(AUDIO_CLIENT_NBUFS_MAX, nfrags_max);
      int nbytes = (1 << frag_lg_size);
      err = audio_client_set_buffer_size(io_client->output, nbufs, nbytes);
      if (err != 0)
	return(err);
    }

  /* configure input granularity */
  if (io_client->input != NULL)
    {
      audio_shared *shared = &shared_global;
      int lg_samples = frag_lg_size - 1;

      if ((err = mic_buf_set_wakeup_mask(shared->mic, lg_samples)) != 0)
	return(err);
    }

  /* everything OK */
  return(0);
}

static int audio_oss_get_ispace(audio_io_client *io_client, audio_buf_info *arg)
{
  audio_dev *dev = &mic_global;
  audio_shared *shared = &shared_global;
  mic_buf *mic = shared->mic;

  audio_client *client;
  audio_buf_info info;
  audio_offset behind;

  /* sanity check */
  if ((client = io_client->input) == NULL)
    return(-EINVAL);

  /* info: totals */
  info.fragsize   = (1 << mic->lg_wakeup) * sizeof(short);
  info.fragstotal = mic->nbytes / info.fragsize;
    
  /* info: current availability */
  behind = audio_dev_get_offset(dev) - client->offset;
  info.bytes = sizeof(short) * MIN(behind, mic->ndata);
  info.fragments = info.bytes / info.fragsize;

  /* copyout info */
  copy_to_user((void *) arg, (const void *) &info, sizeof(audio_buf_info));

  /* everything OK */
  return(0);
}

static int audio_oss_get_ospace(audio_io_client *io_client, audio_buf_info *arg)
{
  audio_client *client;
  audio_buf_info info;
  ulong flags;

  /* sanity check */
  if ((client = io_client->output) == NULL)
    return(-EINVAL);

  /* info: totals */
  info.fragsize   = client->buf_size;
  info.fragstotal = client->nbufs;

  /* info: current availability */
  info.fragments = 0;
  info.bytes = 0;
  save_flags_cli(flags);
  {
    int i;
    for (i = 0; i < client->nbufs; i++)
      if (audio_buf_done(&client->buf[i]))
	{
	  info.fragments++;
	  info.bytes += client->buf[i].size;
	}
  }
  restore_flags(flags);

  /* copyout info */
  copy_to_user((void *) arg, (const void *) &info, sizeof(audio_buf_info));

  /* everything OK */
  return(0);
}

static int audio_oss_get_iptr(audio_io_client *io_client,
			      struct file *file,
			      count_info *arg)

{
  audio_dev *dev = &mic_global;
  audio_client *client;
  audio_offset behind;  
  count_info info;
  int npending;

  /* sanity check */
  if ((client = io_client->input) == NULL)
    return(-EINVAL);

  /* compute positions */
  behind   = audio_dev_get_offset(dev) - client->offset;
  npending = MIN(behind, dev->shared->mic->ndata);

  /* set info; n.b. no direct access to input buffer */
  info.bytes  = file->f_pos + (npending * sizeof(short));
  info.blocks = 0;
  info.ptr    = 0;

  /* copyout info */
  copy_to_user((void *) arg, (const void *) &info, sizeof(count_info));

  /* everything OK */
  return(0);
}

static int audio_oss_get_optr(audio_io_client *io_client,
			      struct file *file,
			      count_info *arg)
{
  audio_client *client;
  count_info info;
  int npending;

  /* sanity check */
  if ((client = io_client->output) == NULL)
    return(-EINVAL);

  /* compute positions */
  npending = audio_client_output_nbuffered_prim(client);

  /* set info; n.b. no direct access to output buffer */
  info.bytes  = file->f_pos - (npending << client->format_lg_bytes);
  info.blocks = 0;
  info.ptr    = 0;

  /* copyout info */
  copy_to_user((void *) arg, (const void *) &info, sizeof(count_info));

  /* everything OK */
  return(0);  
}

static int audio_oss_set_duplex(audio_io_client *io_client)
{
  /* full duplex always enabled */
  return(0);
}

static int audio_oss_supported_formats(int *arg)
{
  int supported = AFMT_S8 | AFMT_S16_LE;
  put_user(supported, arg);
  return(0);
}

static int audio_oss_supported_capabilities(int *arg)
{
  /* XXX DSP_CAP_MMAP not set because mmap not yet OSS-compatible */
  int supported = DSP_CAP_DUPLEX | DSP_CAP_REALTIME;
  put_user(supported, arg);
  return(0);
}

static int audio_oss_ioctl(struct inode *inode,
			   struct file *file,
			   uint command,
			   ulong arg)
{
  audio_io_client *io_client = (audio_io_client *) file->private_data;
  int size, err;

  /* sanity check */
  if (_IOC_TYPE(command) != AUDIO_OSS_IOCTL_TYPE)
    return(-EINVAL);

  /* verify transfer memory */
  size = _IOC_SIZE(command);
  if (_IOC_DIR(command) & _IOC_READ)
    if ((err = verify_area(VERIFY_WRITE, (void *) arg, size)) != 0)
      return(err);
  if (_IOC_DIR(command) & _IOC_WRITE)
    if ((err = verify_area(VERIFY_READ,  (void *) arg, size)) != 0)
      return(err);

  /* dispatch based on command */
  switch (command) {
  case SNDCTL_DSP_RESET:
    return(audio_oss_reset(io_client));
  case SNDCTL_DSP_POST:
  case SNDCTL_DSP_SYNC:
    return(audio_oss_sync(io_client));
  case SNDCTL_DSP_SPEED:
    return(audio_oss_set_speed(io_client, (int *) arg));
  case SNDCTL_DSP_STEREO:
    return(audio_oss_set_stereo(io_client, (int *) arg));
  case SNDCTL_DSP_GETBLKSIZE:
    return(audio_oss_get_frag_size(io_client, (int *) arg));
  case SNDCTL_DSP_SAMPLESIZE:
    return(audio_oss_set_sample_size(io_client, (int *) arg));
  case SNDCTL_DSP_CHANNELS:
    return(audio_oss_set_channels(io_client, (int *) arg));
  case SNDCTL_DSP_GETFMTS:
    return(audio_oss_supported_formats((int *) arg));
  case SNDCTL_DSP_GETCAPS:
    return(audio_oss_supported_capabilities((int *) arg));
  case SNDCTL_DSP_SETDUPLEX:
    return(audio_oss_set_duplex(io_client));
  case SNDCTL_DSP_SETFRAGMENT:
    return(audio_oss_set_frag_size(io_client, (int *) arg));
  case SNDCTL_DSP_GETISPACE:
    return(audio_oss_get_ispace(io_client, (audio_buf_info *) arg));
  case SNDCTL_DSP_GETOSPACE:
    return(audio_oss_get_ospace(io_client, (audio_buf_info *) arg));
  case SNDCTL_DSP_GETIPTR:
    return(audio_oss_get_iptr(io_client, file, (count_info *) arg));
  case SNDCTL_DSP_GETOPTR:
    return(audio_oss_get_optr(io_client, file, (count_info *) arg));
  }

  /* invalid command */
  return(-EINVAL);
}

static int audio_itsy_ioctl_input(audio_dev *dev,
				  audio_client *client,
				  uint command,
				  ulong arg)
{
  /* audio input commands */
  switch (command) {
  case AUDIO_GET_NBUFFERED:
    return(audio_client_input_nbuffered(client, (uint *) arg));
  case AUDIO_INPUT_GET_GAIN:
    return(audio_get_gain(dev, (uint *) arg));
  case AUDIO_INPUT_SET_GAIN:
    return(audio_set_gain_safe(dev, arg));
  case AUDIO_INPUT_SET_LOOPBACK:
    return(audio_shared_set_loop(dev->shared, arg));
  case AUDIO_INPUT_SET_OFFSET_CANCEL:
    return(audio_shared_set_offset_cancel(dev->shared, arg));
  case AUDIO_INPUT_GET_LG_WAKEUP:
    return(mic_buf_get_wakeup_mask(dev->shared->mic, (uint *) arg));
  case AUDIO_INPUT_SET_LG_WAKEUP:
    return(mic_buf_set_wakeup_mask(dev->shared->mic, arg));
  case AUDIO_INPUT_GET_DROPPED:
    return(audio_client_get_dropped(client, (audio_offset *) arg));
  case AUDIO_INPUT_SET_DROPPED:
    return(audio_client_set_dropped(client, (audio_offset *) arg));
  case AUDIO_INPUT_SET_POSITION:
    return(audio_client_input_set_position(dev, client, (audio_offset *) arg));
  case AUDIO_INPUT_SET_DROP_FAIL:
    return(audio_client_set_drop_fail(client, arg));
  }

  /* invalid command */
  return(-EINVAL);
}

static int audio_itsy_ioctl_output(audio_dev *dev,
				   audio_client *client,
				   uint command,
				   ulong arg)
{
  /* audio output commands */
  switch (command) {
  case AUDIO_GET_NBUFFERED:
    return(audio_client_output_nbuffered(client, (uint *) arg));
  case AUDIO_OUTPUT_GET_ATTENUATION:
    return(audio_get_attenuation(dev, (uint *) arg));
  case AUDIO_OUTPUT_SET_ATTENUATION:
    return(audio_set_attenuation_safe(dev, arg));
  case AUDIO_OUTPUT_STOP:
    return(audio_client_output_stop(client));
  case AUDIO_OUTPUT_PAUSE:
    return(audio_output_pause(dev, client, arg));
  case AUDIO_OUTPUT_GET_BUF_SIZE:
    return(audio_client_get_buffer_size(client, (uint *) arg));
  case AUDIO_OUTPUT_SET_BUF_SIZE:
    return(audio_client_set_buffer_size(client, client->nbufs, arg));
  case AUDIO_OUTPUT_GET_MIX_VOLUME:
    return(audio_client_get_mix_volume(client, (uint *) arg));
  case AUDIO_OUTPUT_SET_MIX_VOLUME:
    return(audio_client_set_mix_volume(client, arg));
  }

  /* invalid command */
  return(-EINVAL);
}

static int audio_itsy_ioctl(struct inode *inode,
			    struct file *file,
			    uint command,
			    ulong arg)
{
  audio_io_client *io_client = (audio_io_client *) file->private_data;
  audio_client *client;
  int size, err;

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_itsy_ioctl: cmd=%d, arg=%lu\n", command, arg);

  /* sanity checks */
  if ((_IOC_TYPE(command) != AUDIO_IOCTL_TYPE) ||
      (_IOC_NR(command) > AUDIO_IOCTL_MAXNR))
    return(-EINVAL);

  /* verify transfer memory */
  size = _IOC_SIZE(command);
  if (_IOC_DIR(command) & _IOC_READ)
    if ((err = verify_area(VERIFY_WRITE, (void *) arg, size)) != 0)
      return(err);
  if (_IOC_DIR(command) & _IOC_WRITE)
    if ((err = verify_area(VERIFY_READ,  (void *) arg, size)) != 0)
      return(err);

  /* itsy interface requires separate files for input and output */
  if (((io_client->input != NULL) && (io_client->output != NULL)) ||
      ((io_client->input == NULL) && (io_client->output == NULL)))
    return(-EINVAL);
  client = (io_client->input != NULL) ? io_client->input : io_client->output;

  /* bidirectional commands */
  switch (command) {
  case AUDIO_GET_FORMAT:
    return(audio_client_get_format(client, (uint *) arg));
  case AUDIO_SET_FORMAT:
    return(audio_client_set_format(client, arg));
  case AUDIO_GET_RATE:
    return(audio_client_get_rate(client, (uint *) arg));
  case AUDIO_SET_RATE:
    return(audio_client_set_rate(client, arg));
  case AUDIO_LOCK_RATE:
    return(audio_client_lock_rate(client, arg));
  case AUDIO_GET_POSITION:
    return(audio_client_get_position(client, (audio_offset *) arg));
  case AUDIO_WATCH_BUTTONS:
    return(audio_client_watch_buttons(client, (void *) arg));
  case AUDIO_DEBUG_HOOK:
    return(audio_debug_hook(arg));
  }

  /* input commands */
  if (io_client->input != NULL)
    return(audio_itsy_ioctl_input(&mic_global, client, command, arg));

  /* output commands */
  if (io_client->output != NULL)
    return(audio_itsy_ioctl_output(&audio_global, client, command, arg));

  /* invalid command */
  return(-EINVAL);
}


#ifdef CONFIG_SA1100_TIFON

static int audio_switch_ioctl(struct inode *inode,
			      struct file *file,
			      uint command,
			      ulong arg)
{
	switch( command ){
	case AUDIO_SWITCH_GET:
		return switch_status;
	case AUDIO_SWITCH_SET:
		switch_status = arg;
		break;
	default:
		return (-EINVAL);
	}
	return 0;
}

#endif


static int audio_ioctl(struct inode *inode,
		       struct file *file,
		       uint command,
		       ulong arg)
{
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_ioctl: cmd=%d, arg=%lu\n", command, arg);

  /* OSS compatibility */
  if (_IOC_TYPE(command) == AUDIO_OSS_IOCTL_TYPE)
    return(audio_oss_ioctl(inode, file, command, arg));

  /* native itsy compatibility */
  if (_IOC_TYPE(command) == AUDIO_IOCTL_TYPE)
    return(audio_itsy_ioctl(inode, file, command, arg));

#ifdef CONFIG_SA1100_TIFON
  /* support for audioswitch on Tifon */
  if (_IOC_TYPE(command) == AUDIO_SWITCH_IOCTL_TYPE)
    return(audio_switch_ioctl(inode, file, command, arg));

#endif

  /* invalid command type */
  return(-EINVAL);
}

static int audio_open(struct inode *inode, struct file *file)
{
  audio_shared *shared = &shared_global;
  audio_dev *dev_in    = &mic_global;
  audio_dev *dev_out   = &audio_global;

  audio_client *client_in, *client_out;
  audio_io_client *client_io;
  int status, nclients;

  /* debugging */
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_open\n");

  /* initialize */
  client_in = client_out = NULL;
  client_io = NULL;
  status = -ENOMEM;

  /* create new clients */
  nclients = shared->nclients;
  if (file->f_mode & FMODE_READ)
    if ((client_in = audio_dev_client_create(dev_in)) == NULL)
      goto audio_open_fail;
  if (file->f_mode & FMODE_WRITE)
    if ((client_out = audio_dev_client_create(dev_out)) == NULL)
      goto audio_open_fail;

  /* create container, attach to file */
  if ((client_io = audio_io_client_create(client_in, client_out)) == NULL)
    goto audio_open_fail;

  /* create special mic_buf, if necessary */
  if ((client_in != NULL) && (shared->mic == NULL))
    if ((shared->mic = mic_buf_create()) == NULL)
      goto audio_open_fail;
  
  /* initialize IRQ, hardware if necessary */
  if (nclients == 0)
    {
      /* request appropriate interrupt line */
      if ((status = request_irq(AUDIO_IRQ,
				generic_interrupt,
				SA_INTERRUPT, 
				AUDIO_NAME,
				NULL)) != 0)
	{
	  /* fail: unable to acquire interrupt */
	  if (AUDIO_DEBUG)
	    printk(AUDIO_NAME ": request_irq failed: %d\n", status);
	  goto audio_open_fail;
	}
      
      /* initialize hardware */
      audio_hardware_init(shared);
    }
  
#ifdef	MODULE
  MOD_INC_USE_COUNT;
#endif

  /* attach client state to file */
  file->private_data = client_io;

  /* everything OK */
  return(0);

audio_open_fail:

  /* reclaim storage and fail */
  if (client_in  != NULL) 
    audio_dev_client_destroy(dev_in,  client_in);
  if (client_out != NULL)
    audio_dev_client_destroy(dev_out, client_out);
  if (client_io  != NULL)
    audio_io_client_destroy(client_io);
  return(status);
}

static void audio_dev_shutdown(audio_dev *dev)
{
  audio_shared *shared = dev->shared;
  ulong flags;

  /* cleanup if no clients */
  if (dev->nclients == 0)
    {
      /* critical section */
      save_flags_cli(flags);
      {
	/* shutdown I/O appropriately */
	if (dev->io_dir == AUDIO_INPUT)
	  audio_input_disable(dev);
	if (dev->io_dir == AUDIO_OUTPUT)
	  audio_output_disable(dev);
      }
      restore_flags(flags);

      /* destroy special mic_buf, if necessary */
      if (dev->io_dir == AUDIO_INPUT)
	if (shared->mic != NULL)
	  {
	    mic_buf_destroy(shared->mic);
	    shared->mic = NULL;
	  }
    }

  /* reclaim IRQ, shutdown hardware if necessary */
  if (shared->nclients == 0)
    {
      /* shutdown hardware */
      audio_hardware_shutdown(shared);
      
      /* free appropriate interrupt line */
      free_irq(AUDIO_IRQ, NULL);
    }
}

static int audio_release(struct inode *inode, struct file *file)
{
  audio_io_client *io_client = (audio_io_client *) file->private_data;
  audio_shared *shared = &shared_global;
  audio_dev *dev_out   = &audio_global;
  audio_dev *dev_in    = &mic_global;

  /* detach client state from file */
  file->private_data = NULL;

  /* detach input client */
  if (io_client->input != NULL)
    {
      (void) audio_shared_rate_unlock(shared, io_client->input->id);
      audio_dev_client_destroy(dev_in, io_client->input);
      audio_dev_shutdown(dev_in);
    }

  /* detach output client */
  if (io_client->output != NULL)
    {
      (void) audio_shared_rate_unlock(shared, io_client->output->id);
      audio_dev_client_destroy(dev_out, io_client->output);
      audio_dev_shutdown(dev_out);
    }  

#ifdef	MODULE
  MOD_DEC_USE_COUNT;
#endif

  return 0;
}

/*
 * power management operations
 *
 */

#ifdef	CONFIG_POWERMGR
static int audio_suspend_check(void *ignore, int idle_jiffies)
{
  /*
   * modifies: nothing
   * effects:  Always returns zero; i.e. willing to sleep immediately.
   *
   */

  return(0);
}

static int audio_suspend(void *ignore)
{
  /*
   * modifies: shared_global (global)
   * effects:  Saves audio state in preparation for sleep mode.
   *           Always returns zero (ignored).
   *
   */

  audio_shared *shared = &shared_global;

  /* disable audio interrupts, if necessary */
  if (shared->nclients > 0)
    mask_irq(AUDIO_IRQ);

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_suspend\n");

  /* return value ignored */
  return(0);
}

static int audio_resume(void *ignore, int resume_flags)
{
  /*
   * modifies: shared_global (global)
   * effects:  Resumes audio state after wakeup from sleep mode.
   *           Always returns zero (ignored).
   *
   */

  audio_shared *shared = &shared_global;

  /* enable audio interrupts, if necessary */
  if (shared->nclients > 0)
    {
      /* set rate, re-enabling audio I/O */
      audio_set_rate_prim(shared, shared->rate);

      /* enable audio interrupts */
      unmask_irq(AUDIO_IRQ);
    }

  /* debugging */
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_resume\n");

  /* return value ignored */
  return(0);
}

static const powermgr_client audio_powermgr = {
  /* callback functions */
  audio_suspend_check,
  audio_suspend,
  audio_resume,
  
  /* uninterpreted client data */
  NULL,

  /* identity */
  POWERMGR_ID_INVALID,
  AUDIO_NAME_VERBOSE,

  /* power-consumption info */
  0,
  0
};
#endif	/* CONFIG_POWERMGR */

/*
 * initialization
 *
 */

static struct file_operations audio_fops =
{
  NULL,			/* no special lseek */
  audio_read,		
  audio_write,
  NULL,			/* no special readdir */
#if 0	/* need to be updated */
  audio_poll,
#else
  NULL,
#endif
  audio_ioctl,
#if 0 	/* need to be updated */
  audio_mmap,		
#else
  NULL,
#endif
  audio_open,
  NULL,			/* no special flush */
  audio_release,
  audio_fsync,
  NULL,			/* no special fasync */
  NULL,			/* no special check_media_change */
  NULL,			/* no special revalidate */
  NULL			/* no special lock */
};

static char *audio_version(void)
{
  /*
   * modifies: nothing
   * effects:  Returns the current cvs revision number.
   * storage:  Return value is statically-allocated.
   *
   */

  static char version[8];
  char *revision;
  int i = 0;

  /* extract version number from cvs string */
  for (revision = AUDIO_VERSION_STRING; *revision != '\0'; revision++)
    if (isdigit((int) *revision) || (*revision == '.'))
      version[i++] = *revision;
  version[i] = '\0';

  return(version);
}

int audio_sa1100_mcp_init(void)
{
  audio_shared *shared = &shared_global;
  int err;

  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": audio_sa1100_init\n");

  /* sanity check */
  if (sizeof(audio_input_mmap) != (MIC_BUF_NBYTES + MIC_BUF_META_NBYTES))
    {
      /* log error and fail */
      printk(AUDIO_NAME ": invalid audio_input_mmap size (%d, expected %ld)\n",
	     sizeof(audio_input_mmap),
	     MIC_BUF_NBYTES + MIC_BUF_META_NBYTES);
      return(-EINVAL);
    }

  /* initialize global state */
  audio_shared_init(shared);
  audio_dev_init(&audio_global, shared, AUDIO_OUTPUT);
  audio_dev_init(&mic_global,   shared, AUDIO_INPUT);

  /* register device */
  if ((err = register_chrdev(AUDIO_MAJOR, AUDIO_NAME, &audio_fops)) != 0)
    {
      /* log error and fail */
      printk(AUDIO_NAME ": unable to register major device %d\n", AUDIO_MAJOR);
      return(err);
    }

  /* register with mcp common module */
  if ((shared->mcp_id = mcp_common_register()) == MCP_COMMON_ID_INVALID)
    {
      /* log error and fail */
      printk(AUDIO_NAME ": unable to register with mcp module\n");
      return(-EBUSY);
    }

#ifdef	CONFIG_POWERMGR
  /* register with power manager */
  shared->pm_id = powermgr_register(&audio_powermgr);
  if (shared->pm_id == POWERMGR_ID_INVALID)
    if (AUDIO_DEBUG)
      printk(AUDIO_NAME ": unable to register with power manager\n");
#endif	/* CONFIG_POWERMGR */

#ifdef	CONFIG_PROC_FS
  /* register procfs device */
  proc_register(&proc_root, &audio_proc_entry);
#endif	/* CONFIG_PROC_FS */

  /* log device registration */
  printk(AUDIO_NAME_VERBOSE " version %s initialized\n", audio_version());

  /* everything OK */
  return(0);
}

#ifdef	MODULE
int init_module(void)
{
  if (AUDIO_DEBUG_VERBOSE)
    printk(AUDIO_NAME ": init_module\n");

  return(audio_sa1100_mcp_init());
}

void cleanup_module(void)
{
  audio_shared *shared = &shared_global;
  int err;

  /* unregister with mcp common module */
  if (mcp_common_unregister(shared->mcp_id) != 0)
    printk(AUDIO_NAME ": unable to unregister with mcp module\n");

#ifdef	CONFIG_POWERMGR
  /* unregister with power manager */
  if (powermgr_unregister(shared->pm_id) != POWERMGR_SUCCESS)
    printk(AUDIO_NAME ": unable to unregister with power manager\n");
#endif	/* CONFIG_POWERMGR */

#ifdef	CONFIG_PROC_FS
  /* unregister procfs entry */
  if ((err = proc_unregister(&proc_root, audio_proc_entry.low_ino)) != 0)
    if (AUDIO_DEBUG)
      printk(AUDIO_NAME ": cleanup_module: proc_unregister(audio) %d\n", err);
#endif	/* CONFIG_PROC_FS */

  /* unregister driver */
  unregister_chrdev(AUDIO_MAJOR, AUDIO_NAME);

  /* log device unload */
  printk(AUDIO_NAME_VERBOSE " unloaded\n");
}
#endif	/* MODULE */
