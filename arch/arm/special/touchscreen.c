/*
 * Touch screen driver for Tifon
 * uses the sa1100 and ucb1200
 *
 * Copyright 1999 Peter Danielsson
 * (codec routines shamelessy stolen from the audio driver)
 *
 */
#include <linux/config.h>

#ifdef  MODULE
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
#include <linux/poll.h>
#include <linux/errno.h>

#ifdef  CONFIG_PROC_FS
#include <linux/stat.h>
#include <linux/proc_fs.h>
#endif

#ifdef  CONFIG_POWERMGR
#include <linux/powermgr.h>
#endif

#include <asm/segment.h>
#include <asm/uaccess.h>
#include <asm/arch/irqs.h>


#include "mcp_common.h"
#include "mcp-sa1100.h"


/* jiffies between samples */
#define SAMPLE_INTERVAL 10

/* minimum delay to register as double-tap */
#define MINTAP (3*SAMPLE_INTERVAL)


/* codec registers */
#define	CODEC_IO_DATA		(0)
#define	CODEC_IO_DIRECTION	(1)
#define CODEC_RE_INT_ENABLE     (2)
#define CODEC_FE_INT_ENABLE     (3)
#define CODEC_INT_STATCLR       (4)
#define CODEC_TELCOM_CTL_A      (5)
#define CODEC_TELCOM_CTL_B      (6)         
#define	CODEC_AUDIO_CTL_A	(7)
#define	CODEC_AUDIO_CTL_B	(8)
#define CODEC_TOUCH_CTL         (9)
#define	CODEC_ADC_CTL		(10)
#define CODEC_ADC_DATA          (11)
#define CODEC_ID                (12)
#define	CODEC_MODE		(13)

#define TSMX_POW        0x1
#define TSPX_POW        0x2
#define TSMY_POW        0x4
#define TSPY_POW        0x8
#define TSMX_GND        0x10
#define TSPX_GND        0x20
#define TSMY_GND        0x40
#define TSPY_GND        0x80
#define TSC_MODE_INT    0x000
#define TSC_MODE_PRES   0x100
#define TSC_MODE_POS    0x200
#define TSC_BIAS_ENA    0x800
#define TSPX_LOW        0x1000
#define TSMX_LOW        0x2000

/* Register CODEC_ADC_CTL */
#define ADC_SYNC_ENA    0x1
#define ADC_INPUT_TSPX  0x0
#define ADC_INPUT_TSMX  0x4
#define ADC_INPUT_TSPY  0x8
#define ADC_INPUT_TSMY  0xc
#define ADC_INPUT_AD0   0x10
#define ADC_INPUT_AD1   0x14
#define ADC_INPUT_AD2   0x18
#define ADC_INPUT_AD3   0x1c
#define EXT_REF_ENA     0x20
#define ADC_START       0x80
#define ADC_ENA         0x8000

#define PRESSED 0
#define P_DONE 1
#define X_DONE 2
#define Y_DONE 3
#define RELEASED 4

#define TS_IRQ 0
#define TS_MAJOR 60
#define TS_NAME "Touchscreen"


/* Register 11 */
#define ADC_DAT_VAL     0x8000
#define GET_DATA(x) (((x)>>5)&0x3ff)

#define ADC_ENA_TYPE (ADC_SYNC_ENA | ADC_ENA)


#define TS_INTERRUPT (TSPX_POW | TSMX_POW | TSPY_GND | \
                      TSMY_GND | TSC_MODE_INT )
#define TS_PRESSURE (TSPX_POW | TSMX_POW | TSPY_GND | \
		    TSC_MODE_PRES | TSC_BIAS_ENA )
#define ADC_PRESSURE ADC_ENA_TYPE
#define TS_XPOS (TSPX_POW | TSMX_GND | TSC_MODE_POS | TSC_BIAS_ENA )
#define ADC_XPOS (ADC_ENA_TYPE | ADC_INPUT_TSMY)
#define TS_YPOS (TSPY_POW | TSMY_GND | TSC_MODE_POS | TSC_BIAS_ENA )
#define ADC_YPOS (ADC_ENA_TYPE | ADC_INPUT_TSMX)

#define BUFSIZE 10

struct ts_data {
	int p;
	int x;
	int y;
	int p_raw;
	int x_raw;
	int y_raw;
	unsigned long time;
};

struct data_packet {
	unsigned short p;
	unsigned short x;
	unsigned short y;
};

struct calibration {
	int x_scale;               /* scale value for x-pos*1024 */
	int y_scale;               /* scale value for y-pos*1024 */
	int p_scale;               /* scale value for pressure*1024 */
	int x_offset;              /* offset for x pos */
	int y_offset;
	int p_offset;
	int x_threshold;           /* threshold before updating pos */
	int y_threshold;
	int p_threshold;
};

struct touchinfo {
	mcp_reg *reg;             /* physical address to mcp register */
	int mcp;
	int id;                   /* codec id */
	int count;                /* usage count */
	int state;                /* current state of driver */
	struct timer_list timer;  /* timer used in delays */
	struct wait_queue *proc_list;
	struct calibration cal;
	struct ts_data buf[BUFSIZE];
	struct fasync_struct *fasync;
	int head;
	int tail;
};

struct ts_data cur_data;
struct touchinfo ts;
unsigned int irq_count;

/* mcp/codec operations */

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

static int readADC( void )
{
	/* we shouldn't have to check if conversion is ready */
	return GET_DATA(codec_read(ts.reg, CODEC_ADC_DATA));
}


static int pen_up( void )
{
	return (codec_read(ts.reg, CODEC_TOUCH_CTL ) & (3 << 12));
}

/* data processing routines */

void new_data( void )
{

	if( ts.head != ts.tail ){
		int last = ts.head--;
		if( last < 0 )
			last = BUFSIZE-1;
		if( cur_data.p == ts.buf[last].p &&
		    cur_data.y == ts.buf[last].y &&
		    cur_data.x == ts.buf[last].x &&
		    abs(jiffies-ts.buf[last].time) < MINTAP){
			return;
		}
	}
	cur_data.time = jiffies;
	ts.buf[ts.head]=cur_data;
	ts.head++;
	if( ts.head == BUFSIZE )
		ts.head = 0;
	if( ts.head == ts.tail ){
		ts.tail++;
		if( ts.tail == BUFSIZE )
			ts.tail = 0;
	}
	if( ts.fasync )
		kill_fasync( ts.fasync, SIGIO );
	wake_up_interruptible( &ts.proc_list );
}

struct ts_data get_data()
{
	int last = ts.tail++;
	if(ts.tail == BUFSIZE )
		ts.tail = 0;
	return ts.buf[last];
}


int no_data( void )
{
	return (ts.head == ts.tail);
}

void store_p(int p)
{
	cur_data.p_raw = p;
	if( p > ts.cal.p_threshold )
		cur_data.p = (p*ts.cal.p_scale)/1024 + ts.cal.p_offset;
}

void store_x(int x)
{
	cur_data.x_raw = x;
	if( x > ts.cal.x_threshold )
		cur_data.x = (x*ts.cal.x_scale)/1024 + ts.cal.x_offset;
}

void store_y(int y)
{
	cur_data.y_raw = y;
	if( y > ts.cal.y_threshold )
		cur_data.y = (y*ts.cal.y_scale)/1024 + ts.cal.y_offset;
}

/* interrupt handling routines */

void wait_for_action( void )
{
	/* set up for pen_down detection */
	ts.state = 0;
	codec_write(ts.reg, CODEC_TOUCH_CTL, TS_INTERRUPT );
	codec_write(ts.reg, 2, 0 );
	codec_write(ts.reg, 3, 0x3000 );
}

void start_chain(void)
{
	/* set up for pressure reading; */
	ts.state = P_DONE;
	codec_write(ts.reg, CODEC_TOUCH_CTL, TS_PRESSURE );
	codec_write(ts.reg, 2, 1 << 11 );
	codec_write(ts.reg, 3, 0 );
	codec_write(ts.reg, CODEC_ADC_CTL, ADC_PRESSURE );
}

static void handle_timer( unsigned long arg )
{
	if( pen_up() )
		wait_for_action();
	else
		start_chain();
}


void start_timer( void )
{
	init_timer( &ts.timer );
	ts.timer.expires = jiffies + SAMPLE_INTERVAL;
	ts.timer.function = handle_timer;
	add_timer( &ts.timer );
}

static void ts_interrupt( int irq, void *dev_id, struct pt_regs *regs)
{
	/* clear interrupts */
	codec_write(ts.reg, 4, 0 );
	codec_write(ts.reg, 4, 0xffff );

	switch( ts.state ){
	case PRESSED:
		start_chain();
		break;
	case P_DONE:
		/* set up for x reading; */
		store_p( readADC() );
		codec_write(ts.reg, CODEC_TOUCH_CTL, TS_XPOS );
		codec_write(ts.reg, CODEC_ADC_CTL, ADC_XPOS );
		codec_write(ts.reg, 2, 1 << 11 );
		ts.state++;
		break;
	case X_DONE:
		/* set up for y reading */
		store_x( readADC() );
		codec_write(ts.reg, CODEC_TOUCH_CTL, TS_YPOS );
		codec_write(ts.reg, CODEC_ADC_CTL, ADC_YPOS );
		codec_write(ts.reg, 2, 1 << 11 );
		ts.state++;
		break;
	case Y_DONE:
		store_y( readADC() );
		/* set up for pen_up detection */
		codec_write(ts.reg, CODEC_TOUCH_CTL, TS_INTERRUPT );
		codec_write(ts.reg, 2, 0x3000 );
		ts.state++;
		new_data();
		start_timer();
		break;
	case RELEASED:
		wait_for_action();
		break;
	default:
		panic("Unknown Touchscreen state\n");
	}

}


/* interface functions */

static ssize_t ts_read( struct file *file, char *buf,
			size_t count, loff_t *offset)
{
	/* return position values from the buffer */
	int i;
	struct wait_queue wait = { current, NULL };

	if( count % 6 )
		return -EIO;
	if( no_data() ){
		if( file->f_flags & O_NONBLOCK )
			return -EAGAIN;
		add_wait_queue( &ts.proc_list, &wait );
		current->state = TASK_INTERRUPTIBLE;
		while( no_data() && ! signal_pending(current) ){
			schedule();
			current->state = TASK_INTERRUPTIBLE;
		}
		current->state = TASK_RUNNING;
		remove_wait_queue(&ts.proc_list, &wait );
	}
	i=count;
	while( i > 5 && !no_data() ){
		struct data_packet p;
		struct ts_data t = get_data();
		p.x = t.x;
		p.y = t.y;
		p.p = t.p;
		i -= 6;
		copy_to_user( buf, &p, 6 );
		buf += 6;
	}
	if( count-i ){
		file->f_dentry->d_inode->i_atime = CURRENT_TIME;
		return count - i;
	}
	return 0;	
}

static unsigned int ts_poll( struct file *filp, poll_table *wait)
{
	/* wait for data */
	poll_wait(filp, &ts.proc_list, wait);
	if(!no_data())
		return POLLIN | POLLRDNORM;
	return 0;
}

static int ts_ioctl( struct inode *inode, struct file *file,
		     unsigned int command, unsigned long argument)
{
	/* serve ioctl requests for calibration */
	return -EINVAL;
}

static int ts_fasync( int fd, struct file *file, int on )
{
	/* handle asynchronous notification */
	int retval;

	retval = fasync_helper( fd, file, on, &ts.fasync );
	if( retval < 0 )
		return retval;
	return 0;
}

static int ts_open( struct inode *inode, struct file *file)
{
	/* initialize mcp and interrupts */
	if( ts.count == 0 ){
		mcp_common_enable(ts.mcp);
		GRER |= 1 << TS_IRQ;
		GFER &= ~(1 << TS_IRQ );
		GEDR = 1 << TS_IRQ;
		GPDR &= ~(1 << TS_IRQ);
		if( request_irq(TS_IRQ, ts_interrupt, SA_INTERRUPT,
				"Touchscreen", NULL) ){
			return -EBUSY;
		}
		codec_write(ts.reg, 2, 0 );
		codec_write(ts.reg, 3, 0 );
		wait_for_action();
		ts.head = ts.tail = 0;
	}
	MOD_INC_USE_COUNT;
	ts.count++;
	return 0;
}

static int ts_release( struct inode *inode, struct file *file)
{
	/* close mcp and free interrupts */
	ts.count--;
	if( ts.count == 0 ){
		mcp_common_disable(ts.mcp);
		free_irq( TS_IRQ, NULL );
	}
	ts_fasync(-1, file, 0);
	MOD_DEC_USE_COUNT;
	return 0;
}


static struct file_operations ts_fops = {
	NULL,            /* seek */
	ts_read,
	NULL,            /* write */
	NULL,            /* readdir */
	ts_poll,
	ts_ioctl,
	NULL,            /* mmap */
	ts_open,
	NULL,            /* flush */
	ts_release,
	NULL,            /* fsync */
	ts_fasync
};

int touchscreen_init( void )
{
	int err;
	
	memset(&ts, 0, sizeof(ts));
	memset(&ts.cal, 1, sizeof(struct calibration));
	ts.reg = (mcp_reg *)&Ser4MCCR0;
	if((err = register_chrdev(TS_MAJOR, TS_NAME, &ts_fops))){
		printk("Unable to get major %d for device %s\n",
		       TS_MAJOR, TS_NAME);
		return err;
	}

	if((ts.mcp = mcp_common_register()) == MCP_COMMON_ID_INVALID){
		printk("%s could not get access to mcp\n", TS_NAME);
		unregister_chrdev( TS_MAJOR, TS_NAME );
		return -1;
	}
	
	mcp_common_enable(ts.mcp);
	ts.id = codec_read( ts.reg, 12 );
	mcp_common_disable(ts.mcp);
	
	if(ts.id == 0){
		printk("%s device id is zero - aborting\n", TS_NAME);
		mcp_common_unregister( ts.mcp );
		unregister_chrdev( TS_MAJOR, TS_NAME );
		return -1;
	}

	printk("Touchscreen driver, device type %x, version %d\n",
	       (ts.id>>6) & 0x3ff, ts.id & 0x3f);

	return 0;
}


#ifdef MODULE
	
int init_module( void )
{
	touchscreen_init();
	return 0;
}

void cleanup_module( void )
{
	unregister_chrdev(TS_MAJOR, TS_NAME);
	mcp_common_unregister( ts.mcp );
}

#endif;

