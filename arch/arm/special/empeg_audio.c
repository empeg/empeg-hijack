/*
 * SA1100/empeg Audio Device Driver
 *
 * (C) 1999/2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Hugo Fiennes, <hugo@empeg.com>
 *   Mike Crowe, <mac@empeg.com>
 *   John Ripley, <john@empeg.com>
 *
 * Heavily based on audio-sa1100.c, copyright (c) Carl Waldspurger, 1998
 * but with DMA support and more limited audio set due to the empeg hardware.
 *
 * The empeg audio output device has several 'limitations' due to the hardware
 * implementation:
 *
 * - Always in stereo
 * - Always at 44.1kHz
 * - Always 16-bit
 *
 * Due to the high data rate these parameters dictate, this driver uses the
 * onboard SA1100 DMA engine to fill the FIFOs. As this is the first DMA
 * device on the empeg, we use DMA channel 0 for it - only a single channel
 * is needed as, although the SSP input is connected, this doesn't synchronise
 * with what we need and so we ignore the input (currently).
 *
 * This current version is total **** as it's just the "one afternoon" 2.0
 * version munged to do *anything* under 2.2. This will change a lot when more
 * pressing things are out of the way :-)
 *
 */

/*
 * "to do" list
 *
 *   - add "get rate" ioctl
 *   - implement select
 *   - update file->f_pos (output done, update input)
 *
 *   - reduce overly-conservative critical sections
 *
 *   - audio mixing, or perhaps better in user-level server
 *
 */

#ifdef CONFIG_EMPEG_DAC
#error empeg DSP driver cannot coexist with DAC driver.
#endif

/*           
 * debugging 
 *           
 */

#define AUDIO_DEBUG			0
#define AUDIO_DEBUG_VERBOSE		0
#define AUDIO_DEBUG_STATS		1 //AUDIO_DEBUG | AUDIO_DEBUG_VERBOSE


/*          
 * includes 
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

#ifdef	CONFIG_PROC_FS
#include <linux/stat.h>
#include <linux/proc_fs.h>
#endif

#include <asm/segment.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/arch/hardware.h>
#include <asm/arch/SA-1100.h>

#include <linux/soundcard.h>
#include <asm/uaccess.h>
#include <linux/delay.h>

// #define I2C_LOCK
#ifdef I2C_LOCK
static volatile int i2c_lock = 0;
#endif

static struct tq_struct i2c_queue = {
	NULL, 0, NULL, NULL
};
#include "empeg_audio.h"

/*           
 * constants 
 *           
 */

/* options */
#define OPTION_DEBUGHOOK		0

/* defaults */
#define AUDIO_FORMAT_MASK		(AFMT_U8 | AFMT_S16_LE | AFMT_MU_LAW | AFMT_A_LAW \
					 | AFMT_U16_LE | AFMT_IMA_ADPCM)
#define AUDIO_FORMAT_DEFAULT		(AFMT_S16_LE)
#define AUDIO_CHANNELS_DEFAULT		2		/* default: mono */
#define AUDIO_RATE_DEFAULT		44100

#define MIXER_DEVMASK			(SOUND_MASK_VOLUME)
#define MIXER_STEREODEVS		MIXER_DEVMASK

/* names */
#define AUDIO_NAME			"audio-empeg"
#define AUDIO_NAME_VERBOSE		"empeg audio driver"

/* device numbers */
#define AUDIO_MAJOR			(EMPEG_AUDIO_MAJOR)
#define AUDIO_NMINOR			(5)
#define DSP_MINOR			(3)
#define AUDIO_MINOR			(4)
#define MIXER_MINOR			(0)
#define MIC_MINOR			(1)

/* interrupt numbers */
#define AUDIO_IRQ			IRQ_DMA0 /* DMA channel 0 IRQ */

/* client parameters */
#define AUDIO_NBR_BUFFER_DEFAULT	(8)   /* Number of audio buffers */
#define AUDIO_BUFFER_SIZE_DEFAULT	(4608) /* Size of user buffer chunks */
#define AUDIO_USER_BUFFER_SIZE		(4608) /* size returned by getblocksize() */
#define AUDIO_MAX_CLIENTS		(16)
#define AUDIO_CLIENT_ID_INVALID	(-1)

/* Input channels */

#define INPUT_PCM (1)
#define INPUT_RADIO (0)
#define INPUT_AUX (2)

/*       
 * types 
 *       
 */

typedef unsigned char uchar;

typedef int audio_client_id;
typedef long long audio_offset;
typedef enum { AUDIO_INPUT, AUDIO_OUTPUT } audio_io_dir;

/* statistics */
typedef struct {
	ulong samples;
	ulong interrupts;
	ulong wakeups;  
	ulong fifo_err;
} audio_stats;

typedef struct {
	/* Device info */
	int rate;

	/* locks */
	audio_client_id rate_locker;
	
	/* open minor devices */
	int  open[AUDIO_NMINOR];
	uint nbr_buffer;		       /* Number of buffers */
	uint buffer_size;		       /* Size of buffer */

} audio_shared;

typedef struct
{
	/* buffer */
	char *data;
	//int  size;			       /* pas besoin */
	
	/* valid data */
	int  next;
	int  count;
	
	/* stream position (in samples) */
	audio_offset offset;
} audio_buf;

typedef struct
{
	/* identifier */
	audio_client_id id;
	
	/* scheduling notifications */
	struct wait_queue *schedq;
	
	/* buffer management */
	struct wait_queue *waitq;
	int interrupt_buf;
	int copy_buf;
	audio_buf *buf;		       /* buffers allocated at run-time */
	int copy_idx;		       /* index de copie des données dans le buffer courant */
	int sync;		       /* flag: 1 -> traitement du sync en cours,  0 -> pas de sync en cours */
	int initialized;	       /* flag: 1 -> les buffers sont initialisés, 0 -> pas encore */
	
	/* sample format */
	uint format;
	uint format_bytes;
	int  channels;		       /* nombre de channels */
	
	/* stream position (in samples) */
	audio_offset offset;
	
	/* pause flag */
	uint pause;
	
} audio_client;

typedef struct
{
	/* device management */
	audio_shared *shared;
	audio_io_dir io_dir;
	int handle_interrupts;
	int fifo_interrupts;
	int minor;
	
	/* client management */
	audio_client *client[AUDIO_MAX_CLIENTS];
	audio_client *active;
	audio_client *special;
	int nclients;
	
	/* client activity timestamp */
	ulong client_io_timestamp;
	
	/* stream position (in samples) */
	audio_offset offset;

	/* beep timeout */
	struct timer_list beep_timer;

	/* currently buffering user data flag (used to note underruns) */
	int gooddata;

	/* info gathering */
        audio_stats stats;
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

/*         
 * globals 
 *         
 */

static void emit_action(void *);
static void mixer_select_input(int input);
static int mixer_setvolume(mixer_dev *dev, int vol);
static void audio_beep_end(unsigned long);
static void mixer_eq_reset(void);
static void mixer_eq_set(struct empeg_eq_section_t *sections);
static void mixer_eq_apply(void);
static unsigned int eq_last[40];
static unsigned int eq_reg_last = 0;

static audio_shared	shared_global;
static audio_dev	audio_global;
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
#endif	/* CONFIG_PROC_FS */

/* DSP operations */

static int dsp_write(unsigned short address, unsigned int data)
{
	//printk("dsp_write %x=%x\n",address,data);
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

/*                    
 * utility operations 
 *                    
 */

//                                                    
static inline void del_buffers( audio_client *client )
{
	if( client->buf !=NULL ) {
		audio_shared *shared = &shared_global;
		int i;
		for( i = 0; i < shared->nbr_buffer; i++ ) if( client->buf[i].data != NULL ) {
			kfree_s( client->buf[i].data, shared->buffer_size );
			client->buf[i].data = NULL;
		}
		kfree_s( client->buf, shared->nbr_buffer * sizeof( audio_buf ));
		client->buf = NULL;
	}
}

//                                                   
static inline int new_buffers( audio_client *client )
/*
 * allocate memory and initialize buffers if not already done.
 */
{
	if( ! client->initialized ) {
		audio_shared *shared = &shared_global;
		int i;
		/* allocate and clear buffers */
		if(( client->buf = (audio_buf*) kmalloc( shared->nbr_buffer * sizeof(audio_buf), GFP_KERNEL )) == NULL ) {
			del_buffers( client );
			return 0;
		}
		for( i = 0; i < shared->nbr_buffer; i++ ) {
			if(( client->buf[i].data = (char*) kmalloc( shared->buffer_size, GFP_KERNEL )) == NULL ) {
				del_buffers( client );
				return 0;
			}
			memset( (void*) client->buf[i].data, 0, shared->buffer_size );
			client->buf[i].next = client->buf[i].count = client->buf[i].offset = 0;
			//client->buf[i].size = shared->buffer_size;
		}
		client->initialized = 1;
	}
	return 1;
}


//                                                 
static inline uint audio_format_size( uint format )
/*
 * return number of bits par sample for a given format.
 */
{
	switch( format ) {
	case AFMT_S16_LE:
	case AFMT_U16_LE:
		return 16;
	default: return 0;
	}
}

//                                                                
static inline uint audio_format_bytes( uint format, int channels )
/*
 * returns the amount of bits according to format and nbf of channels.
 */
{
	uint bits = channels * audio_format_size( format );
	//uint bits = audio_format_size(format) * audio_format_channels(format);
	return(bits / 8);
}


/*                   
 * procfs operations 
 *                   
 */

#ifdef	CONFIG_PROC_FS

#if AUDIO_DEBUG
//                                                            
static int client_get_info( char *buf, const audio_client *c )
{
	audio_shared *shared = &shared_global;
	int i, len;
	
	len = sprintf( buf, "client %d: copy=%d intr=%d\n", c->id, c->copy_buf, c->interrupt_buf );
	for( i = 0; i < shared->nbr_buffer; i++ ) {
		const audio_buf *b = &c->buf[i];
		len += sprintf( buf+len, "   buf %d: data=%lx size=%d next=%d count=%d off=%ld\n", i,
				(ulong) b->data, shared->buffer_size, b->next, b->count, (ulong) (b->offset & 0xffffffff));
	}
	return len;
#if 0
	const audio_buf *b0 = &c->buf[0];
	const audio_buf *b1 = &c->buf[1];
	int len;
	
	len = sprintf(buf,
		      "client %d: copy=%d intr=%d\n"
		      "   buf 0: data=%lx size=%d next=%d count=%d off=%ld\n"
		      "   buf 1: data=%lx size=%d next=%d count=%d off=%ld\n",
		      c->id, c->copy_buf, c->interrupt_buf,
		      (ulong) b0->data, b0->size, b0->next, b0->count,
		      (ulong) (b0->offset & 0xffffffff),
		      (ulong) b1->data, b1->size, b1->next, b1->count,
		      (ulong) (b1->offset & 0xffffffff));
	return(len);
#endif
}
#endif

/* XXX expand reported info */
//                                                                                       
static int generic_get_info( char *buf, const audio_dev *dev, const char *fifo_err_name )
{
	int len=0;
#if AUDIO_DEBUG_STATS
	audio_shared *shared = dev->shared;
	
	/* format device stats */
	len = sprintf(buf, 
		      "device-major: %8d\n"
		      "device-minor: %8d\n"
		      "device-open:  %8s\n"
		      "sample-rate:  %8u\n"
		      "nclients:     %8d\n"
		      "intr enabled: %8s\n"
		      "interrupts:   %8lu\n"
		      "wakeups:      %8lu\n"
		      "samples:      %8lu\n"
		      "%-13s %8lu\n",
		      AUDIO_MAJOR,
		      dev->minor,
		      shared->open[dev->minor] ? "yes" : "no",
		      shared->rate,
		      dev->nclients,
		      dev->handle_interrupts ? "yes" : "no",
		      dev->stats.interrupts,
		      dev->stats.wakeups,
		      dev->stats.samples,
		      fifo_err_name,
		      dev->stats.fifo_err);
#endif
	
	/* debugging */
#if AUDIO_DEBUG
	{
		int i;
		
		/* format client info */
		for( i = 0; i < AUDIO_MAX_CLIENTS; i++ )
			if( dev->client[i] != NULL )
				len += client_get_info( buf + len, dev->client[i] );
		if( dev->special != NULL )
			len += client_get_info( buf + len, dev->special );
	}
#endif
	
	return(len);  
}

//                                                                                        
static int audio_read_proc( char *buf, char **start, off_t offset, int length,
			    int *eof, void *private )
{
	return(generic_get_info(buf, &audio_global, "underruns:"));
}


#endif	/* CONFIG_PROC_FS */


/*                         
 * audio_shared operations 
 *                         
 */

//                                                   
static void __init audio_shared_init( audio_shared *shared )
{
	/* initialize shared state */
	memset((void *) shared, 0, sizeof(audio_shared));
	shared->rate = AUDIO_RATE_DEFAULT;
	shared->nbr_buffer = shared->buffer_size = 0;
}

//                                                              
static int audio_shared_open_count( const audio_shared *shared )
/*
 * modifies: nothing
 * effects:  Returns the number of open audio devices sharing the codec.
 */
{
	int i, count;
	count = 0;
	for( i = 0; i < AUDIO_NMINOR; i++ )
		if( shared->open[i] ) count++;
	return count;
}

static void audio_shared_set_attenuation( audio_shared *shared, uint att )
{
   #if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_shared_set_attenuation: att=%u\n", att);
#endif
	
#if 0   
	/* set codec attenuation */
	shared->audio_ctl_b &= ~CODEC_AUDIO_ATT_MASK;
	shared->audio_ctl_b |= att & CODEC_AUDIO_ATT_MASK;
	codec_write(CODEC_REG_AUDIO_CTL_B, shared->audio_ctl_b);
	
	/* update shared state */
	shared->attenuation = att;
#endif
}

/*                      
 * audio_buf operations 
 *                      
 */

#if 0
//                                                 
static inline void audio_buf_init( audio_buf *buf )
{
	/* initialize */
	buf->size   = 0;
	buf->next   = 0;
	buf->count  = 0;
	buf->offset = 0;
}
#endif

//                                                                     
static inline void audio_buf_set( audio_buf *buf, int next, int count )
{
	/* reset */
	buf->next  = next;
	buf->count = count;
}

//                                                      
static inline int audio_buf_done( const audio_buf *buf )
{
	/* returns TRUE iff audio buf is complete */
	return buf->next >= buf->count;
}


/*                         
 * audio_client operations 
 *                         
 */

//                                                      
static void audio_client_destroy( audio_client *client )
/*
 * modifies: client
 * effects:  Reclaims all storage associated with client.
 */
{
	if( client != NULL ) {
		/* deallocate buffers */
		del_buffers( client );
		
		/* deallocate container */
		kfree_s(client, sizeof(audio_client));
	}
}

//                                                                          
static audio_client *audio_client_create( audio_client_id id, int buf_size )
/*
 * modifies: nothing
 * effects:  Creates and returns a new, initialized audio_client object
 *           with identifier id and buffers each containing buf_size bytes.
 *           Returns NULL iff unable to allocate storage.
 */
{
	audio_client *client;
	
	/* allocate client object */
	client = (audio_client *) kmalloc(sizeof(audio_client), GFP_KERNEL);
	if( client == NULL ) return NULL;
	
	/* initialize client */
	memset( (void *) client, 0, sizeof( audio_client ));
	client->id = id;
	
	/* initialize client format */
	client->format	= AUDIO_FORMAT_DEFAULT;
	client->channels	= AUDIO_CHANNELS_DEFAULT;
	client->format_bytes	= audio_format_bytes( client->format, client->channels );
	client->copy_idx	= client->sync = 0;
	
	/* allocate, initialize client buffers */
	if( buf_size > 0 ) {
		audio_shared *shared = &shared_global;
		/* audio fragments initialisation */
		shared->nbr_buffer   = AUDIO_NBR_BUFFER_DEFAULT;
		shared->buffer_size  = buf_size;
		/* buffer initialisation is now done in audio_write()
		 * in order to be able to use set_fragment() in the mean time.
		 */
		/* if( ! new_buffers( client )) { audio_client_destroy( client ); return NULL; } */
	}
	
	/* everything OK */
	return client;
}

//                                                              
static audio_client_id audio_dev_client_create( audio_dev *dev )
/*
 * modifies: dev
 * effects:  Creates a new, initialized client of dev.
 *           Returns the client id, otherwise error code.
 */
{
	audio_client *client;
	audio_client_id id;
	int buf_size;
	
	/* choose buffer size, mic clients do not use buffers */
	buf_size = AUDIO_BUFFER_SIZE_DEFAULT;
	if( dev->io_dir == AUDIO_INPUT ) buf_size = 0;
	
	/* look for free client slot */
	for( id = 0; id < AUDIO_MAX_CLIENTS; id++ )
		if( dev->client[id] == NULL ) {
			/* allocate new client, fail if unable */
			if(( client = audio_client_create( id, buf_size )) == NULL )
				return(-ENOMEM);
			
			/* attach client to device */
			dev->client[id] = client;
			dev->nclients++;
			
#if AUDIO_DEBUG_VERBOSE
			printk(AUDIO_NAME ": audio_dev_client_create: id=%u\n", id);
#endif
			
			/* return client id */
			return(id);
		}
	
	/* no free client slots */
	return(-EBUSY);
}

//                                                                       
static int audio_dev_client_destroy( audio_dev *dev, audio_client_id id )
/*
 * modifies: dev
 * effects:  Detaches client associated with id from dev,
 *           if any, and reclaims all associated storage.
 *           Returns 0 iff successful, otherwise error code.
 */
{
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_dev_client_destroy: id=%d\n", id);
#endif
	
	if( dev->client[id] != NULL ) {
		audio_client *client = dev->client[id];
		ulong flags;
		
		/* critical section */
		save_flags_cli(flags);

		/* wait until client inactive */
		while( dev->active == client )
			interruptible_sleep_on(&client->schedq);
		
		/* XXX should do something if client signalled */
		
		/* detach client from device */
		dev->client[id] = NULL;
		dev->nclients--;

		restore_flags(flags);
		
		/* reclaim storage */
		audio_client_destroy(client);
	}
	
	/* everything OK */
	return(0);
}

//                                                                                                
static void __init audio_dev_init( audio_dev *dev, audio_shared *shared, int minor, audio_io_dir io_dir )
{
	/* initialize */
	memset((void *) dev, 0, sizeof(audio_dev));
	
	/* device management */
	dev->shared   = shared;
	dev->io_dir   = io_dir;
	dev->minor    = minor;
	
	/* client management */
	dev->active   = NULL;
	dev->nclients = 0;
	
	/* stream position */
	dev->offset   = 0;

	/* beep timeout */
	init_timer(&dev->beep_timer);
	dev->beep_timer.data = 0;
	dev->beep_timer.function = audio_beep_end;
}

//                                               
static void audio_output_enable( audio_dev *dev , audio_client *client )
{
	unsigned int data;
	unsigned int length;
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_output_enable\n");
#endif
	/* enable interrupt processing */
	dev->handle_interrupts = 1;
	
        /* Note we've done a wakeup */
	dev->stats.wakeups++;

	/* Clear bits in DCSR0 */
	ClrDCSR0=0x28;
	
	/* Initialise DDAR0 for SSP */
	DDAR0=0x81c01be8;
	
	/* Are we starting with client data or not? */
	if (client) {
		/* Actual data in buffers */
		data=virt_to_phys(client->buf[client->interrupt_buf].data);
		length=client->buf[client->interrupt_buf].count;

		/* Buffering good data */
		dev->gooddata=1;
	} else {
		/* Zero memory */
		data=_ZeroMem;
		length=4608;

		/* Were we buffering good data? If so, reset flag and signal
		   an underrun */
		if (dev->gooddata) {
			dev->stats.fifo_err++;
			dev->gooddata=0;
		}
	}

	/* Which buffer is to be used next? Initialise the appropriate one */
	if (RdDCSR0&0x80) {
		/* Buffer B is next buffer - start it */
		DBSB0=(unsigned char*)data;
		DBTB0=length;
		SetDCSR0=0x43;
	} else {
		/* Buffer A is next buffer - start it */
		DBSA0=(unsigned char*)data;
		DBTA0=length;
		SetDCSR0=0x13;
	}
}

//                                                
static void audio_output_disable( audio_dev *dev )
{
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_output_disable\n");
#endif
	
	/* disable DMA */
	ClrDCSR0=0x2b;
	
	/* disable interrupt processing */
	dev->handle_interrupts = 0;
}

//                                                                          
static int audio_set_rate( audio_dev *dev, audio_client *client, uint rate )
/*
 * modifies: audio
 * effects:  Attempts to set audio sampling rate to rate for client.
 *           Returns 0 iff successful, else error code.
 */
{
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_set_rate: rate=%u\n", rate);
#endif
	
	/* everything OK */
	return(0);
}

//                                                     
static void audio_hardware_init( audio_shared *shared )
{
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_hardware_init\n");
#endif
	
	/* This should be set up already, but it won't hurt: we run the SSP
	   from an external clock on GPIO19 - ensure the GAFR is setup correctly */
	GAFR|=(1<<19);
	
	/* Turn on the SSP */
	Ser4SSCR0=0x8f;
	Ser4SSCR1=0x30;
	Ser4SSSR=SSSR_ROR; /* Clear ROR bit as noted in the datasheets */
}

//                                                         
static void audio_hardware_shutdown( audio_shared *shared )
{
   #if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_hardware_shutdown\n");
   #endif
}

/*                      
 * interrupt processing 
 *                      
 */

//                                                                 
static audio_client *audio_get_client( audio_dev *dev, int wakeup )
/*
 * requires: called with interrupts disabled
 * modifies: dev
 * effects:  Returns a client requesting service from dev,
 *           or NULL if no such client exists.  If wakeup
 *           is set, wakes up "descheduled" client.
 */
{
	int next, i;
	
	/* start search with active client, if any */
	next = 0;
	if( dev->active != NULL )
		next = dev->active->id;
	
	/* search all client slots */
	for( i = 0; i < AUDIO_MAX_CLIENTS; i++ ) {
		audio_client *client = dev->client[next];
		
		/* check client for pending data */
		if(( client != NULL ) && ( !client->pause )) {
			audio_buf *buf = &client->buf[client->interrupt_buf];
			
			if( !audio_buf_done( buf )) {
				/* wakeup descheduled client, if any */
				if( wakeup )
					if(( dev->active != NULL ) && ( dev->active != client ))
						wake_up_interruptible( &dev->active->schedq );
				
#if AUDIO_DEBUG_VERBOSE
				printk(AUDIO_NAME ": audio_get_client: id=%d\n", client->id);
#endif
				/* make client active */
				dev->active = client;
				
				return(client);
			}
		}
		
		/* advance client, handle wraparound */
		next++;
		if (next >= AUDIO_MAX_CLIENTS)
			next = 0;
	}
	
	/* no clients with pending data */
	
	/* wakeup descheduled client, if any */
	if( wakeup )
		if( dev->active != NULL )
			wake_up_interruptible( &dev->active->schedq );
	
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_get_client: NULL\n");
#endif
	
	/* no active client */
	dev->active = NULL;
	
	return(NULL);
}

//                                                  
static void audio_handle_interrupt( audio_dev *dev )
{
	/* handle transmit interrupts */
	audio_client *client;
	ulong flags;
	audio_shared *shared = &shared_global;
	
	/* update statistics */
#if AUDIO_DEBUG_STATS
	dev->stats.interrupts++;
#endif
	
	/* find client to service */
	save_flags_cli(flags);
	if(( client = audio_get_client( dev, 1 )) == NULL ) {
#if 0
		/* no clients: disable output */
		audio_output_disable( dev );
#else
		/* No clients: clock out zeros to keep DSP running. We take these from the
		   zero-memory page of the SA1100 (0xe0000000 physical) as there is no read
                   penalty for this */
		/* Clear bits in DCSR0 */
		ClrDCSR0=0x28;
	
		/* Note: this is not ideal - we don't use the hardware queueing of the
		   DMA A/B selects at the moment. We will later, but this'll do for now */
		if (RdDCSR0&0x80) {
			/* Buffer B is next buffer */
			DBSB0=(unsigned char*)_ZeroMem;
			DBTB0=4608;
		
			/* Set it going */
			SetDCSR0=0x43;
		} else {
			/* Buffer A is next buffer*/
			DBSA0=(unsigned char*)_ZeroMem;
			DBTA0=4608;
			
			/* Set it going */
			SetDCSR0=0x13;
		}
#endif
		restore_flags( flags );

		/* Were we buffering good data? If so, reset flag and signal
		   an underrun */
		if (dev->gooddata) {
			dev->stats.fifo_err++;
			dev->gooddata=0;
		}

		/* Run the audio buffer emmitted action */
		queue_task(&emit_task, &tq_immediate);
		mark_bh(IMMEDIATE_BH);
	
		return;		
	}
	restore_flags(flags);
	
	/* Done this buffer */
	client->buf[client->interrupt_buf].next=client->buf[client->interrupt_buf].count;
	
	/* Move to next buffer */
#if AUDIO_DEBUG
	printk(".%x",client->interrupt_buf);
#endif
	
	/* DMA IRQ - buffer completed. Set up next one */
	client->interrupt_buf = (client->interrupt_buf+1) % shared->nbr_buffer;
	
	/* Clear bits in DCSR0 */
	ClrDCSR0=0x28;
	
	/* Note: this is not ideal - we don't use the hardware queueing of the
	   DMA A/B selects at the moment. We will later, but this'll do for now */
	if (RdDCSR0&0x80) {
		/* Buffer B is next buffer */
		DBSB0=(unsigned char*)virt_to_phys(client->buf[client->interrupt_buf].data);
		DBTB0=client->buf[client->interrupt_buf].count;
		
		/* Set it going */
		SetDCSR0=0x43;
	} else {
		/* Buffer A is next buffer*/
		DBSA0=(unsigned char*)virt_to_phys(client->buf[client->interrupt_buf].data);
		DBTA0=client->buf[client->interrupt_buf].count;
		
		/* Set it going */
		SetDCSR0=0x13;
	}

	/* We're buffering good data */
	dev->gooddata=1;

	/* Run the audio buffer emmitted action */
	queue_task(&emit_task, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
	
	/* wake up waiter */
	wake_up_interruptible( &client->waitq );
}

static void emit_action(void *p)
{
	audio_emitted_action();
}

//                                                                          
static void generic_interrupt( int irq, void *dev_id, struct pt_regs *regs )
{
	audio_dev *audio     = &audio_global;
	
	/* handle transmit interrupts */
	if( audio->handle_interrupts ) {
		audio_handle_interrupt( audio );
	}
}


/*                 
 * file operations 
 *                 
 */

//                                                          
static inline int audio_buffers_done( audio_client *client )
{
	audio_shared *shared = &shared_global;
	int i;
	for( i = 0; i < shared->nbr_buffer; i++ )
		if( ! audio_buf_done( &client->buf[i] )) return 0;
	return 1;
}

//                                                         
static int audio_client_write_flush( audio_client *client )
{
	ulong flags;
	int result;
	
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_write_flush: client %d: wait\n", client->id);
#endif
	
	/* initialize */
	result = 0;
	
	/* critical section */
	save_flags_cli(flags);

	client->sync = 1;
	/* wait for both buffers to completely drain */
	while( ! audio_buffers_done( client )) {
		interruptible_sleep_on( &client->waitq );
#if 0
		if( current->signal & ~current->blocked ) {
			/* abort if interrupted */
			result = -EINTR;
			break;
		}
#endif
	}
	client->sync = 0;
	restore_flags(flags);
	
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_write_flush: client %d: done\n", client->id);
#endif
	
	return(result);
}

//                                                              
static int audio_fsync(struct file *file, struct dentry *dentry)
{
	/* flush all pending samples for client associated with file */
	audio_client *client = (audio_client *) file->private_data;
	return audio_client_write_flush( client );
}

//                                                        
static int audio_client_write_stop( audio_client *client )
/*
 * modifies: client
 * effects:  Clears buffers associated with  client.
 *           Returns number of cleared bytes, if any.
 */
{
	audio_shared *shared = &shared_global;
	int unwritten = 0;
	ulong flags;
	
	/* critical section */
	save_flags_cli(flags);
	{
		int i;
		/* mark both buffers empty */
		for( i = 0; i < shared->nbr_buffer; i++ ) {
			unwritten += client->buf[i].count - client->buf[i].next;
			audio_buf_set( &client->buf[i], 0, 0 );
		}
		
		/* reset buffer pointers */
		client->interrupt_buf = 0;
		client->copy_buf = 0;
	}
	restore_flags(flags);
	
	return unwritten;
}

//                                                                                             
static int audio_write( struct file *file, const char *buffer, size_t count, loff_t *ppos )
{
	audio_client *client = (audio_client *) file->private_data;
	audio_dev *dev = &audio_global;
	audio_shared *shared = &shared_global;
	int total;
	/*int err;*/
	ulong flags;
	
	if( ! new_buffers( client )) return -ENOMEM;
	
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_write: count=%d\n", count);
#endif
	
#if 0   
	/* verify user-space buffer validity */
	if(( err = verify_area( VERIFY_READ, (void *) buffer, count )) != 0 ) {
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": audio_write: verify_area() failed\n");
#endif
		return(err);
	}
#endif
	
	/* initialize total */
	total = 0;  
	
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_write: "
	       "client=%lx, copy_buf=%d, intr_buf=%d\n",
	       (ulong) client, client->copy_buf, client->interrupt_buf);
#endif
	
	while( count > 0 ) {
		audio_buf *buf = &client->buf[client->copy_buf];
		int chunk;
		
		/* update activity timestamp */
		dev->client_io_timestamp = jiffies;
		
#if AUDIO_DEBUG_VERBOSE
		printk(AUDIO_NAME ": audio_write: copy_buf: "
		       "next=%d, count=%d, size=%d\n",
		       buf->next, buf->count, shared->buffer_size );
#endif
		
		/* transfer data from user-space in buffer-size chunks */
		chunk = ( count+client->copy_idx <= shared->buffer_size ? count : shared->buffer_size-client->copy_idx );
		//chunk = ( count <= shared->buffer_size ? count : shared->buffer_size );
		
#if AUDIO_DEBUG_VERBOSE
		printk(AUDIO_NAME ": audio_write: copy_buf=%d, intr_buf=%d\n",
		       client->copy_buf, client->interrupt_buf);
#endif
		
		/* wait for next buffer to drain */
		save_flags_cli(flags);
		while( ! audio_buf_done( buf ) && ! ( client->copy_idx || client-> sync )) {
			/* don't wait if non-blocking */
			if( file->f_flags & O_NONBLOCK ) {
				restore_flags( flags );
				return (total > 0) ? total : -EAGAIN;
			}
			
#if AUDIO_DEBUG_VERBOSE
			printk(AUDIO_NAME ": audio_write: sleep_on\n");
#endif
			
			if (file->f_flags&O_NONBLOCK) return -EBUSY;

			/* next buffer full */
			interruptible_sleep_on( &client->waitq );
			
#if AUDIO_DEBUG_VERBOSE
			printk(AUDIO_NAME ": audio_write: wake up\n");
#endif
			
			/* handle interrupted system call (XXX or buttons) */
			if (signal_pending(current)) {
				/* incomplete write */
				total -= audio_client_write_stop( client );
				restore_flags( flags );
				
				return(total>0)?total:-ERESTARTSYS; /* EINTR? */
			}
		}
		restore_flags( flags );
		
		/* copy chunk of data from user-space */
		copy_from_user((void*)buf->data,(const void*)buffer,chunk);
				
#if AUDIO_DEBUG_VERBOSE
		printk(AUDIO_NAME ": audio_write: chunk=%d\n", chunk);      
#endif
		
		/* critical section */
		save_flags_cli( flags );
		{
			/* mark buffer full */
			audio_buf_set( buf, 0, client->copy_idx + chunk );
			if( client->sync || ( client->copy_idx + chunk >= shared->buffer_size )) {
				client->copy_idx = 0;
				
				/* enable interrupt-driven output */
				if( ! dev->handle_interrupts ) 
					audio_output_enable( dev , client );

				/* flip buffers */
#if AUDIO_DEBUG
				printk("+%x",client->copy_buf);
#endif
				client->copy_buf = (client->copy_buf+1) % shared->nbr_buffer;
			}
			else client->copy_idx += chunk;
		}
		restore_flags( flags );
		
		/* update counts */
		total  += chunk;
		buffer += chunk;
		count  -= chunk;
	}
	
	/* write complete */
	return(total);
}

//                                                                     
static int audio_client_set_format( audio_client *client, uint format )
{
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_client_set_format: format=%x\n", format);
#endif
	
	/* fail if argument invalid */
	if( !( format & AUDIO_FORMAT_MASK )) return -EINVAL;
	
	/* everything OK */
	client->format = format;
	client->format_bytes = audio_format_bytes( format, client->channels );
	
	return 0;
}

//                                                                        
static int audio_client_set_channels( audio_client *client, int channels )
{
   #if AUDIO_DEBUG_VERBOSE
	printk( AUDIO_NAME ": audio_client_set_channels: channels=%d\n", channels );
#endif
	/* fail if argument invalid */
	if( channels < 1 || channels > 2 ) return -EINVAL;   /* Not supported */
	/* everything OK */
	client->channels = channels;
	client->format_bytes = audio_format_bytes( client->format, channels );
	return 0;
}

static int audio_set_fragment( audio_shared *shared, audio_client *client, uint val )
{
	if( ! client->initialized ) {
		shared->buffer_size = 1 << (val & 0x0000FFFF );
		shared->buffer_size = ( shared->buffer_size < 16 ) ? 16 : shared->buffer_size;   /* minimum 16 bytes */
		shared->nbr_buffer  = (val >> 16) & 0x00007FFF;
		shared->nbr_buffer  = ( shared->nbr_buffer  < 2  ) ?  2 : shared->nbr_buffer;    /* minimum 2 buffers */
	}
	return 0;
}

//                                      
static int audio_get_format( ulong arg )
{
	put_user( AUDIO_FORMAT_MASK | AFMT_MU_LAW, (long*) arg );
	return 0;
}

//                                       
static int audio_get_blksize( ulong arg )
{
	put_user( AUDIO_USER_BUFFER_SIZE, (long*) arg );
	return 0;
}

void audio_beep(audio_dev *dev, int pitch, int length, int volume)
{
	/* Section 9.8 */
	unsigned long coeff;
	int low, high, vat, i;
	unsigned int beep_start_coeffs[4];
	
#if AUDIO_DEBUG
	/* Anyone really need this debug output? */
	printk("BEEP %d, %d\n", length, pitch);
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
		beep_start_coeffs[0] = vat;	/* Y_VLsin */
		beep_start_coeffs[1] = vat;	/* Y_VRsin */
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
		if(i2c_write(IICD_DSP, Y_IcoefBL, beep_start_coeffs, 2)) {
		    printk("i2c_write for beep failed\n");
		}

		/* Coefficients for channel beep volume */
		for(i=0; i<4; i++) beep_start_coeffs[i] = volume;
		if(i2c_write(IICD_DSP, Y_tfnFL, beep_start_coeffs, 4)) {
		    printk("i2c_write for beep failed\n");
		}

		dsp_write(X_plusmax, length * 44);
		dsp_write(X_minmax, 0x3fbfe);
		dsp_write(X_stepSize, 1);
		dsp_write(X_counterX, 0);
		
		/* latch new values in synchronously */
		dsp_write(Y_iSinusWant, 0x82a);
		/* turn on the oscillator, superposition mode */
		dsp_write(Y_sinusMode, 0x88d);
		if (length > 0) {
			/* schedule a beep off */
			if (timer_pending(&dev->beep_timer))
				del_timer(&dev->beep_timer);
			dev->beep_timer.expires = jiffies + (length * 2 * HZ)/1000;
			add_timer(&dev->beep_timer);
		}
	}
}

static void audio_beep_end_sched(void *unused)
{
	/* This doesn't handle all cases */
	/* if another thing timed, we should keep the beep on, really */
	if(timer_pending(&audio_global.beep_timer)) return;

	/* Turn beep off */
#if AUDIO_DEBUG
	/* This all works now, I'm pretty sure */
	printk("BEEP off.\n");
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

static int audio_ioctl( struct inode *inode, struct file *file, uint command, ulong arg )
{
	/*audio_client *client = (audio_client *) file->private_data;*/
	audio_dev *dev = &audio_global;
	/*audio_shared *shared = &shared_global;*/
	
#if 0
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_ioctl: command=%d, arg=%lu\n", command, get_user( (long*) arg ));
#endif
#endif
	/* dispatch based on command */
	switch (command)
	{
#if 0   
	case SNDCTL_DSP_SETFMT:	 return audio_client_set_format( client, get_user( (long*) arg ));
	case SNDCTL_DSP_CHANNELS:	 return audio_client_set_channels( client, get_user( (long*) arg ));
	case SNDCTL_DSP_STEREO:      return audio_client_set_channels( client, get_user( (long*) arg )+1 );
	case SNDCTL_DSP_SPEED:	 return audio_set_rate( dev, client, get_user( (long*) arg ));
	case SNDCTL_DSP_GETFMTS:	 return audio_get_format( arg );
	case SNDCTL_DSP_GETBLKSIZE:	 return audio_get_blksize( arg );
	case SNDCTL_DSP_SETFRAGMENT: return audio_set_fragment( shared, client, get_user( (long*) arg ));
	case SNDCTL_DSP_SYNC:	 return audio_fsync( inode, file );
	case SNDCTL_DSP_RESET:
	case SNDCTL_DSP_POST:
	case SNDCTL_DSP_SUBDIVIDE:
	case SNDCTL_DSP_GETOSPACE:
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
#if 0
	case AUDIO_SET_FORMAT:	return( audio_client_set_format( client, arg ));
	case AUDIO_SET_RATE:	return( audio_set_rate( dev, client, arg ));
	case AUDIO_SET_ATT:	return( audio_set_attenuation_safe( dev, arg ));
	case AUDIO_LOCK_RATE:	return( audio_lock_rate( dev, client, arg ));
	case AUDIO_WATCH_BUTTONS:	return( audio_client_watch_buttons( client, arg ));
	case AUDIO_DEBUG_HOOK:	return( audio_debug_hook( dev, arg ));
	case AUDIO_OUTPUT_STOP:	return( audio_output_stop( client));
	case AUDIO_OUTPUT_PAUSE:	return( audio_output_pause( dev, client, arg ));
#endif
#endif
	case EMPEG_DSP_BEEP: {
		int pitch, length, volume;
		int *ptr = (int *)arg;
		get_user_ret(pitch, ptr, -EFAULT);
		get_user_ret(length, ptr + 1, -EFAULT);
		get_user_ret(volume, ptr + 2, -EFAULT);
		audio_beep(dev, pitch, length, volume);
		return 0;
	} 
	default:
		return 0;
	}
	
	/* invalid command */
	return(-EINVAL);
}

//                                                                     
static int generic_open( audio_dev *dev, int minor, struct file *file )
{
	audio_shared *shared = dev->shared;
	audio_client_id id;
	int err;
	
	/* add new client, fail if unable */
	if(( id = audio_dev_client_create( dev )) < 0 )
		return(id);
	
	/* attach client state to file */
	file->private_data = dev->client[id];
	
	/* initialize IRQ, hardware if necessary */
	if( audio_shared_open_count( shared ) == 0 ) {
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": requesting irq %d\n",AUDIO_IRQ);
#endif
		/* request appropriate interrupt line */
		if(( err = request_irq( AUDIO_IRQ, generic_interrupt, SA_INTERRUPT, AUDIO_NAME, NULL )) != 0 ) {
			/* fail: unable to acquire interrupt */
#if AUDIO_DEBUG
			printk(AUDIO_NAME ": request_irq failed: %d\n", err);
#endif
			return(err);
		}
		
		/* initialize hardware */
		audio_hardware_init(shared);
#if AUDIO_DEBUG
		printk(AUDIO_NAME ": hw init done\n");
#endif
	}
	
	/* mark device in-use */
	shared->open[minor] = 1;

	/* start sending data (will be zeros until data turns up in buffer) */
	audio_output_enable(dev,NULL);

	mixer_eq_apply();

#if AUDIO_DEBUG
	printk(AUDIO_NAME ": open done\n");
#endif
	/* everything OK */
	return(0);
}

//                                                             
static int audio_open( struct inode *inode, struct file *file )
{
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": audio_open\n");
#endif

	return(generic_open(&audio_global, AUDIO_MINOR, file));
}

//                                                              
static void generic_release( audio_dev *dev, struct file *file )
{
	audio_client *client = (audio_client *) file->private_data;
	audio_shared *shared = dev->shared;
	ulong flags;
	
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": generic_release\n");
#endif
	/* reclaim storage */
	audio_dev_client_destroy( dev, client->id );
	file->private_data = NULL;
	
	/* cleanup if no clients */
	if( dev->nclients == 0 ) {
		/* mark device free */
		shared->open[dev->minor] = 0;  
		/* critical section */
		save_flags_cli( flags );

		/* shutdown I/O appropriately */
		if( dev->io_dir == AUDIO_OUTPUT )
			audio_output_disable( dev );
		restore_flags( flags );
	}
	
	
	/* reclaim IRQ, shutdown hardware if necessary */
	if( audio_shared_open_count( shared ) == 0 ) {
		/* shutdown hardware */
		audio_hardware_shutdown( shared );
		
		/* free appropriate interrupt line */
		free_irq( AUDIO_IRQ, NULL );
	}
	
	client->initialized = 0;
}

//                                                                 
static int audio_release( struct inode *inode, struct file *file )
{
	audio_dev *audio = &audio_global;
	
#if AUDIO_DEBUG
	printk(AUDIO_NAME ": audio_release\n");
#endif
	
	/* generic cleanup */
	generic_release( audio, file );
	
	return 0;
}

/*                
 * mixer
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
   audio_release,
   audio_fsync,
   NULL,		/* no special fasync */
   NULL,		/* no special check_media_change */
   NULL,       		/* no special revalidate */
   NULL                 /* no special lock */
};

/*
 * MIXER
 */

static inline void mixer_setloudness(mixer_dev *dev, int level)
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

static inline void mixer_setbalance(mixer_dev *dev, int balance)
{
	dsp_write(Y_BALL0, balance_table[balance].ball/8);
	dsp_write(Y_BALL1, balance_table[balance].ball/8);
	dsp_write(Y_BALR0, balance_table[balance].balr/8);
	dsp_write(Y_BALR1, balance_table[balance].balr/8);
	dev->balance = balance;
}

static inline void mixer_setfade(mixer_dev *dev, int fade)
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
}

static void mixer_select_input(int input)
{
	static dsp_setup fm_setup[]=
	{ { 0xfff, 0x5323 },
	  { 0xffe, 0x28ed },
	  // flat eric
	  //	  { 0xffd, 0x102a }, /* 2-ch eq, 65mV tuner */
	  { 0xffd, 0x100a }, /* 2-ch eq, 65mV tuner */	// also in case statement
	  { 0xffc, 0xe086 }, /* level IAC off=e080 */
	  { 0xffb, 0x0aed },
	  { 0xffa, 0x1048 },
	  { 0xff9, 0x0020 }, /* no I2S out */
	  { 0xff3, 0x0000 },
	  { 0,0 } };
	
	static dsp_setup mpeg_setup[]=
	{ { 0xfff, 0xd223 },
	  { 0xffe, 0x28ed },
	  //	  { 0xffd, 0x006c },
	  { 0xffd, 0x106c },	/* 2 ch too */		// also in case statement
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
	  { 0xffd, 0x106c }, /*AD - 2 ch */		// also in case statement
	  { 0xffc, 0x6080 }, /*LEVEL_IAC*/
	  { 0xffb, 0x0aed }, /*IAC*/
	  { 0xffa, 0x904a }, /*SEL*/
	  { 0xff9, 0x0000 }, /*HOST*/ /* no I2S out */
	  { 0xff3, 0x0000 }, /*RDS_CONTROL*/
	  { 0,0 } };

	/* Ensure any beeps playing are off, because this may block for some time */
	dsp_write(Y_sinusMode, 0x89a);
	
	/* POM low */ 
	GPCR=EMPEG_DSPPOM;
	
	switch(input) {
	case INPUT_RADIO: /* FM */
		/* FM mode: see p64 */
		dsp_writemulti(fm_setup);
		eq_reg_last = 0x100a;
		
		/* Easy programming set 2, FM */
		dsp_write(X_modpntr,0x600);
		
		/* Release POM if it wasn't already released*/
		if (!(mixer_global.flags & EMPEG_MIXER_FLAG_MUTE))
		    GPSR = EMPEG_DSPPOM;
		
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
		dsp_write(Y_p1, 2047);	// coefficients
		//		dsp_write(Y_q1, 3987);
		dsp_write(Y_q1, 4012);
		
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
		eq_reg_last = 0x106c;

		/* Easy programming set 4, CD */
		dsp_write(X_modpntr,0x5c0);
		
		/* Release POM */
		/* Why do we do it now? Why not wait until after we've set the mode? */
		if (!(mixer_global.flags & EMPEG_MIXER_FLAG_MUTE))
		    GPSR = EMPEG_DSPPOM;

#if 1
		/* Send it some junk I2S to ensure the DSP can run the initialisation */
		{
			int a,timeout=(jiffies+HZ);
			for(a=0;a<20000 && jiffies<timeout;a++) {
				while((Ser4SSSR&2)==0 && jiffies<timeout);
				Ser4SSDR=0x0000;
			}

			if (jiffies>=timeout) {
				printk("empeg_audio: there appears to be no serial clock!\n");
				while(1);
			}
		}
#endif		
		/* Select mode */
		dsp_write(X_modpntr,0x0200);

		// we want the 44.1kHz tone table
		csin_table = csin_table_44100;

		break;
	case INPUT_AUX:
		/* AUX mode: see p64 */
		dsp_writemulti(aux_setup);
		eq_reg_last = 0x106c;
		
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
	
	/* Turn off soft audio mute (SAM) */
	/* Actually, let's not! */
	/*
	dsp_write(Y_samCl,0x189);
	dsp_write(Y_samCh,0x7a5);
	dsp_write(Y_delta,0x07d);
	dsp_write(Y_switch,0x5d4);
	*/

	/* Setup beep coefficients for this sampling frequency */
	if(csin_table == csin_table_38000) {
		dsp_patchmulti(beep_setup, Y_samAttl, 0x471);
		dsp_patchmulti(beep_setup, Y_samAtth, 0x7ea);
		dsp_patchmulti(beep_setup, Y_deltaA, 0x059);
		dsp_patchmulti(beep_setup, Y_switchA, 0x1f3);
		dsp_patchmulti(beep_setup, Y_samDecl, 0x471);
		dsp_patchmulti(beep_setup, Y_samDech, 0x7ea);
		dsp_patchmulti(beep_setup, Y_deltaD, 0x059);
		dsp_patchmulti(beep_setup, Y_switchD, 0);
	}
	else {
	    /*
		dsp_patchmulti(beep_setup, Y_samAttl, 1040);
		dsp_patchmulti(beep_setup, Y_samAtth, 2029);
		dsp_patchmulti(beep_setup, Y_deltaA, 195);
		dsp_patchmulti(beep_setup, Y_switchA, 195);
		dsp_patchmulti(beep_setup, Y_samDecl, 2029);
		dsp_patchmulti(beep_setup, Y_samDech, 0x7f9);
		dsp_patchmulti(beep_setup, Y_deltaD, 195);
		dsp_patchmulti(beep_setup, Y_switchD, 0x0);
	    */
		dsp_patchmulti(beep_setup, Y_samAttl, 0x471);
		dsp_patchmulti(beep_setup, Y_samAtth, 0x7ea);
		dsp_patchmulti(beep_setup, Y_deltaA, 0x059);
		dsp_patchmulti(beep_setup, Y_switchA, 0x1f3);
		dsp_patchmulti(beep_setup, Y_samDecl, 0x471);
		dsp_patchmulti(beep_setup, Y_samDech, 0x7ea);
		dsp_patchmulti(beep_setup, Y_deltaD, 0x059);
		dsp_patchmulti(beep_setup, Y_switchD, 0);
		/* testing 1 2 3 */
		/*
		dsp_patchmulti(beep_setup, Y_samDecl, 1287);
		dsp_patchmulti(beep_setup, Y_samDech, 2047);
		dsp_patchmulti(beep_setup, Y_deltaD, 28);
		dsp_patchmulti(beep_setup, Y_switchD, 0x0);
		*/
	}

	dsp_writemulti(beep_setup);
}

static int mixer_setvolume(mixer_dev *dev, int vol)
{
	dsp_write(Y_VAT, volume_table[vol].vat);
	dsp_write(Y_VGA, volume_table[vol].vga);
	dev->volume = vol;
	return vol;
}

static int mixer_getdb(mixer_dev *dev)
{
	return volume_table[dev->volume].db;
}

static void mixer_inflict_flags(mixer_dev *dev)
{
	unsigned int flags = dev->flags;
	mixer_mute(flags & EMPEG_MIXER_FLAG_MUTE);
}

static void __init mixer_dev_init(mixer_dev *dev, int minor)
{
	mixer_eq_reset();	// reset coefficients

    	/* Ensure the POM is on while booting. */
    	GPCR = EMPEG_DSPPOM;

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
	printk(AUDIO_NAME ": mixer_ioctl\n");
#endif
	switch(command)
	{
	case SOUND_MIXER_READ_DEVMASK:
		put_user_ret(MIXER_DEVMASK, (int *)arg, -EFAULT);
		return 0;
		
	case SOUND_MIXER_READ_STEREODEVS:
		put_user_ret(MIXER_STEREODEVS, (int *)arg, -EFAULT);
		return 0;

	case EMPEG_MIXER_READ_LOUDNESS:
		put_user_ret(dev->loudness * 10, (int *)arg, -EFAULT);
		return 0;
	case EMPEG_MIXER_WRITE_LOUDNESS:
	{
		int loudness;
		get_user_ret(loudness, (int *)arg, -EFAULT);
		if (loudness/10 >= LOUDNESS_TABLE_SIZE)
			return -EINVAL;
		if (loudness < 0)
			return -EINVAL;
		mixer_setloudness(dev, loudness / 10);
		loudness = dev->loudness * 10;
		put_user_ret(loudness, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_LOUDNESS_DB:
	{
		int db;
		db = mixer_getloudnessdb(dev);
		put_user_ret(db, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_WRITE_BALANCE:
	{
		int balance;
		get_user_ret(balance, (int *)arg, -EFAULT);
		balance += BALANCE_ZERO;
		if (balance < 0)
			return -EINVAL;
		if (balance >= BALANCE_TABLE_SIZE)
			return -EINVAL;
		mixer_setbalance(dev, balance);
		balance = dev->balance - BALANCE_ZERO;
		put_user_ret(balance, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_BALANCE:
	{
		put_user_ret(dev->balance - BALANCE_ZERO, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_BALANCE_DB:
	{
		int db;
		db = balance_table[dev->balance].db;
		put_user_ret(db, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_WRITE_FADE:
	{
		int fade;
		get_user_ret(fade, (int *)arg, -EFAULT);
		fade += FADE_ZERO;
		if (fade < 0)
			return -EINVAL;
		if (fade >= FADE_TABLE_SIZE)
			return -EINVAL;
		mixer_setfade(dev, fade);
		fade = dev->fade - FADE_ZERO;
		put_user_ret(fade, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_FADE:
		put_user_ret(dev->fade - FADE_ZERO, (int *)arg, -EFAULT);
		return 0;
	case EMPEG_MIXER_READ_FADE_DB:
	{
		int db;
		db = fade_table[dev->fade].db;
		put_user_ret(db, (int *)arg, -EFAULT);
		return 0;
	}
		
	case MIXER_WRITE(SOUND_MIXER_VOLUME):
	{
		int vol;
		get_user_ret(vol, (int *)arg, -EFAULT);
		/* Equalise left and right */
		vol = ((vol & 0xFF) + ((vol >> 8) & 0xFF))>>1;
		if (vol < 0 || vol > 100)
			return -EINVAL;
		mixer_setvolume(dev, vol);
		vol = dev->volume;
		vol = (vol & 0xFF) + ((vol << 8));
		put_user_ret(vol, (int *)arg, -EFAULT);
		return 0;
	}
	case MIXER_READ(SOUND_MIXER_VOLUME):
	{
		int vol = dev->volume;
		vol = (vol & 0xFF) + ((vol << 8));
		put_user_ret(vol, (int *)arg, -EFAULT);
		return 0;
	}		
	case EMPEG_MIXER_READ_DB:
	{
		int db;
		db = mixer_getdb(dev);
		put_user_ret(db, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_ZERO_LEVEL:
	{
		int level = VOLUME_ZERO_DB;
		put_user_ret(level, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_WRITE_SOURCE:
	{
		int source;
		get_user_ret(source, (int *)arg, -EFAULT);
		if (source & SOUND_MASK_PCM)
		{
			/*printk(KERN_DEBUG "Setting mixer to DSP source.\n");*/
			mixer_select_input(INPUT_PCM);
			dev->input = SOUND_MASK_PCM;
		}
		else if (source & SOUND_MASK_RADIO)
		{
			/*printk(KERN_DEBUG "Setting mixer to Radio source.\n");*/
			mixer_select_input(INPUT_RADIO);
			dev->input = SOUND_MASK_RADIO;
		}
		else if (source & SOUND_MASK_LINE)
		{
			/*printk(KERN_DEBUG "Setting mixer to line input source.\n");*/
			mixer_select_input(INPUT_AUX);
			dev->input = SOUND_MASK_LINE;
		}
		put_user_ret(dev->input, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_READ_SOURCE:
	{
		put_user_ret(dev->input, (int *)arg, -EFAULT);
		return 0;
	}		
	case EMPEG_MIXER_WRITE_FLAGS:
	{
		int flags;
		get_user_ret(flags, (int *)arg, -EFAULT);

		dev->flags = flags;
		mixer_inflict_flags(dev);
		
		return 0;
	}
	case EMPEG_MIXER_READ_FLAGS:
	{
		put_user_ret(dev->flags, (int *)arg, -EFAULT);
		return 0;
	}
	case EMPEG_MIXER_SET_EQ:
	{
		struct empeg_eq_section_t sections[20];
		int err;

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
		if((err = verify_area(VERIFY_WRITE, (void *) arg, sizeof(int))) != 0)
			return(err);

		four_chan = ((eq_reg_last >> 12) & 1) ^ 1;

		copy_to_user((void *) arg, (const void *) &four_chan, sizeof(int));
		return 0;
	}
	case EMPEG_MIXER_GET_COMPRESSION:
	{
	        int err;
		if((err = verify_area(VERIFY_WRITE, (void *) arg, sizeof(int))) != 0)
			return err;
		copy_to_user((void *) arg, (const void *) &mixer_compression, sizeof(int));
		return 0;
	}
	case EMPEG_MIXER_SET_COMPRESSION:
	{
		int err, onoff;
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

int audio_get_fm_level(void)
{
	unsigned level;

	if (dsp_read_xram(X_leva, &level) < 0) {
		return -1;
	}
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

static void __init audio_hardware_initial_setup(void)
{
	/* Easy programming mode 1 (needs to be done after reset) */
	dsp_write(Y_mod00,0x4ec);
	
	/* Setup I2S clock on GAFR */
	GAFR|=GPIO_GPIO19;

	/* Setup SSP */
	Ser4SSSR=SSSR_ROR;
	Ser4SSCR0=0x8f;
	Ser4SSCR1=0x30;

	/* Set volume */
	dsp_write(Y_VAT,0x800);
	dsp_write(Y_VGA,0xf80);
}

int __init audio_empeg_init( void )
{
	audio_shared *shared = &shared_global;
	int err;
	
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME ": audio_sa1100_init\n");
#endif
	
	/* initialize global state */
	audio_hardware_initial_setup();
	audio_shared_init( shared );
	audio_dev_init( &audio_global, shared, AUDIO_MINOR, AUDIO_OUTPUT );
	mixer_dev_init(&mixer_global, MIXER_MINOR);

	/* register device */
	if(( err = register_chrdev( AUDIO_MAJOR, AUDIO_NAME, &generic_fops )) != 0)
	{
		/* log error and fail */
		printk(AUDIO_NAME ": unable to register major device %d\n", AUDIO_MAJOR);
		return(err);
	}
	
#ifdef	CONFIG_PROC_FS
	/* register procfs devices */
	proc_audio=create_proc_entry("audio",0,0);
	if (proc_audio) proc_audio->read_proc=audio_read_proc;
#endif	/* CONFIG_PROC_FS */
	
	/* log device registration */
	printk(AUDIO_NAME_VERBOSE " initialized\n");
#if AUDIO_DEBUG_VERBOSE
	printk(AUDIO_NAME);
#endif
	
	/* everything OK */
	return(0);
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
	/* No scaling of coefficients */
	{ Y_scalS1_, 0x7ff },
	{ Y_scalS1, 0x7ff },
		
	/* No action */
	{ Y_cpyS1, 0x8fb },	// (copy a to c1)
	{ Y_cpyS1_, 0x8f9 },	// no_action

	/* Just left from A and right from B */
//	{ Y_c0sin, 0x7ff },	// 0x7ff
//	{ Y_c1sin, 0 },
//	{ Y_c2sin, 0x7ff },
//	{ Y_c3sin, 0 },
		
	{ Y_c0sin, 0 },	// 0x7ff
	{ Y_c1sin, 0 },
	{ Y_c2sin, 0 },	// 0x7ff
	{ Y_c3sin, 0 },
		
	/* Full volume */
	{ Y_VLsin, 0x7ff },	// 0x7ff
	{ Y_VRsin, 0x7ff },
		
	/* No clipping on A or B */
	{ Y_IClipAmax, 0x080 },
	{ Y_IClipAmin, 0x080 },
	{ Y_IClipBmax, 0x080 },
	{ Y_IClipBmin, 0x080 },
/*
	{ Y_IClipAmax, 0x018 },
	{ Y_IClipAmin, 0x018 },
	{ Y_IClipBmax, 0x018 },
	{ Y_IClipBmin, 0x018 },
*/
		
	/* Tone frequency */
	{ Y_IcoefAl, 0x089 },
	{ Y_IcoefAh, 0x7e4 },
	{ Y_IcoefBL, 0x089 },
	{ Y_IcoefBH, 0x7e4 },

	/* Coefficients for channel beep volume */
	{ Y_tfnFL, 0x300 },
	{ Y_tfnFR, 0x300 },
	{ Y_tfnBL, 0x300 },
	{ Y_tfnBR, 0x300 },

	/* Attack / decay */
	{ Y_samAttl, 0x18f },
	{ Y_samAtth, 0x7f9 },
	{ Y_deltaA, 0x059 },
	{ Y_switchA, 0x09e },
	{ Y_samDecl, 0x18f },
	{ Y_samDech, 0x7f9 },
	{ Y_deltaD, 0x059 },
	{ Y_switchD, 0x000 },

	{ Y_iSinusWant, 0x82a },
/*	{ Y_sinusMode, 0x897 }, */ /* on */
	{ Y_sinusMode, 0x89a }, /* off */
/*	{ Y_sinusMode, 0x88d },	*/ /* superposition mode */

	{ 0,0 }
};
