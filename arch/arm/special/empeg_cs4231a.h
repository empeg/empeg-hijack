#ifndef EMPEG_CS4231A_H
#define EMPEG_CS4231A_H

struct cs4231_dev
{
	/* Receive buffer for audio data */
	char *rx_buffer;
	int rx_head;
	int rx_tail;
	int rx_used;
	int rx_free;
	int rx_count;

	/* Queue for FIQ - 128 bytes for overrun */
	short fiq_buffer[256+64];

	/* Blocking queue */
	struct wait_queue *rx_wq;

	/* Stats */
	int samples;

        /* Setup */
	int channel;
	int samplerate;
	int stereo;
	int gain;
        int open;
};

/* Buffer sizes */
#define CS4231_BUFFER_SIZE          65536

/* Declarations */
static ssize_t cs4231_read(struct file*,char*,size_t,loff_t*);
static int cs4231_ioctl(struct inode*,struct file*,unsigned int,unsigned long);
static int cs4231_open(struct inode*,struct file*);
static int cs4231_release(struct inode*,struct file*);
static unsigned int cs4231_poll(struct file *filp, poll_table *table);

/* External initialisation */
void empeg_cs4231_init(void);

/* FIQ handler */
extern void *empeg_fiq4231_start,*empeg_fiq4231_end;

/* Bits in the index register */
#define INDEX_MCE	       	0x40
#define INDEX_TRD		0x20

#define LEFT_INPUT              0
#define RIGHT_INPUT             1
#define FS_AND_DATAFORMAT	8
#define INTERFACE_CONFIG	9
#define PIN_CONTROL             10
#define ERROR_STATUS		11
#define MODE_ID		        12
#define ALT_FEATURE_I		16
#define ALT_FEATURE_II		17
#define VERSION                 25
#define CAPTURE_FORMAT          28
#define CDMA_HIGH               30
#define CDMA_LOW                31

#endif
