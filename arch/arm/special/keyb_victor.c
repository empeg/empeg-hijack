/* keyb_victor.c : Keyboard and power switch driver for Victor
 * Dominic Labbé <DominicL@visuaide.com>
 * Nicolas Pitre <nico@visuaide.com>
 * Copyright (C) 1999 VisuAide, Inc.
 */


#include <linux/config.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fcntl.h>
#include <linux/kd.h>
#include <linux/interrupt.h>
#include <linux/timer.h>

#include <asm/segment.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <asm/irq.h>
#include <asm/arch/hardware.h>


// #define KB_DEBUG 

#ifdef KB_DEBUG
#define PRINTK(x...) printk(##x)
#else
#define PRINTK(x...) /* nothing */
#endif



/* Power supply management */

#define GPIO24_26	( GPIO_GPIO24 | GPIO_GPIO25 | GPIO_GPIO26 )
#define GPIO24_27	( GPIO_GPIO24 | GPIO_GPIO25 | GPIO_GPIO26 | GPIO_GPIO27 )

#define         READNOVERSION   0
#define         ADAPTORPLUG     4
#define         FASTCHARGE      5
#define         LOWBAT1         6
#define         LOWBAT2         7
#define         SWITCHOFF       8
#define         SHUTDOWN        9

static int PowerCtl( int function )
{
#ifdef CONFIG_VICTOR_BOARD1
   int ret = 0, i;

   switch( function ) {
    case READNOVERSION:
      for( i = 0; i < 4; i++ ) {
         GPCR = GPIO24_26;
         GPSR = i << 24;
         ret |= ( (GPLR & GPIO_GPIO27) ? 1 : 0 ) << i;
      }
      break;
    case ADAPTORPLUG:
    case FASTCHARGE:
    case LOWBAT1:
    case LOWBAT2:
      GPCR = GPIO24_26;
      GPSR = function << 24;
      ret = GPLR & GPIO_GPIO27 ? 1 : 0;
      break;
    default:
      break;
   }
   return ret;

#else

   int ret = 0;

   switch( function ) {
    case READNOVERSION:
      ret = ( ( GPLR & GPIO24_27 ) >> 24 );
      break;
    case ADAPTORPLUG:
      /* 0 =  branché */
      ret = (GPLR & GPIO_GPIO22) ? 1 : 0;
      break;
    case FASTCHARGE:
      /* 0 = charge rapide */
      ret = (GPLR & GPIO_GPIO20) ? 1 : 0;
      break;
    case LOWBAT1:
      /* 0 = lowbat1 */
      ret = (GPLR & GPIO_GPIO1) ? 1 : 0;
      break;
    case LOWBAT2:
      /* 0 = lowbat2 */
      ret = (GPLR & GPIO_GPIO0) ? 1 : 0;
      break;
    default:
      break;
   }
   return ret;
#endif
}



#define KB_MAJOR	61
#define KB_IRQ		IRQ_GPIO11_27

/* The keyboard is mapped on a 6 by 5 matrix */
#define KeybRows 	6
#define KeybColumns	5

#define K_ROW0 	GPIO_GPIO8
#define K_ROW1 	GPIO_GPIO9
#define K_ROW2 	GPIO_GPIO10
#define K_ROW3 	GPIO_GPIO11
#define K_ROW4 	GPIO_GPIO12
#define K_ROW5 	GPIO_GPIO13

#define K_COL0	GPIO_GPIO14
#define K_COL1	GPIO_GPIO15
#define K_COL2	GPIO_GPIO16
#define K_COL3	GPIO_GPIO17
#define K_COL4  GPIO_GPIO18

#define K_ROW( x ) GPIO_GPIO( 8+x )
#define K_COL( x ) GPIO_GPIO( 14+x )

#define AllCols (K_COL0 | K_COL1 | K_COL2 | K_COL3 | K_COL4)
#define AllRows (K_ROW0 | K_ROW1 | K_ROW2 | K_ROW3 | K_ROW4 | K_ROW5)


/* Typematic settings: wait .25 sec, then 30 cps */
#define REPEAT_INIT	1		/* initial count value */
#define REPEAT_EXPIRE	(HZ/4)		/* when reached, the key is repeated */
#define REPEAT_REINIT	(REPEAT_EXPIRE - (HZ/30))
					/* count value after the key has been repeated */


/* Power switch behavior */
#define POWERSW_DELAY	(1*HZ)		/* hold to simulate a CTRL-ALT_DEL */
#define POWERSW_DELAY_BRUTEFORCE (5*HZ)	/* hold to pull the power supply */

/* ... to send power switch events to user space apps through keyb interface */
static enum { NONE=0, PRESSED, RELEASED } power_sw_ev = NONE;


/* Keyboard translation and queueing */
#define KB_BUFSZ	32		/* keyboard buffer size */
static char kb_buffer[KB_BUFSZ];
static int kb_in, kb_out;		/* index in kbd buffer */
static volatile int kb_nbr;		/* number of keys queued */
static struct wait_queue *kb_waitq;	/* for processes waiting on keypresses */
static int kb_state[KeybRows*KeybColumns];  /* state (time) for each keys */
static int kb_mode;			/* raw or xlate */

/* Scan code to ascii translation */
static const char kb_table[] = "?BPE 123tT456sS789  *0#vV<=>Q ~";



static int kb_open(struct inode *inode, struct file *filp)
{
   PRINTK( "kb: kb_open called\n" );

   /* initialisation */
   memset( kb_state, 0, sizeof(kb_state) );
   kb_in = kb_out = kb_nbr = 0;
   kb_mode = K_XLATE;

   /* enable irqs on edge detection for appropriate GPIOs */
   GEDR = AllCols;
   GRER |= AllCols;
   GFER |= AllCols;

   return 0;
}


static int kb_ioctl( struct inode *inode, struct file *file,
			uint cmd, ulong arg )
{
    switch( cmd ) {
      case KDGKBMODE:
	put_user( kb_mode, (long*)arg );
	break;
      case KDSKBMODE:
	kb_mode = (int)arg;
	break;
      case 0x1234:	/* get different states on the power supply */
	return PowerCtl( arg );
      default:
	return -EINVAL;
    }
    return 0;
}


static int kb_release(struct inode *inode, struct file *filp)
{
   PRINTK( "kb: kb_close called\n" );

   /* disable edge detection */
   GRER &= ~AllCols;
   GFER &= ~AllCols;
   return 0;
}


static int kb_read( struct file *file, char *buf, size_t count, loff_t *ppos )
{
    int cnt = 0;

    while( count ) {
	if( kb_nbr > 0 ) {
	    /* copy data from keyboard buffer */
	    put_user( kb_buffer[kb_out], buf );
	    PRINTK( "kb: put(%#02X)\n", kb_buffer[kb_out] );
	    buf++;
	    kb_out++;
	    kb_out %= KB_BUFSZ;
	    kb_nbr--;
	    cnt++;
	    count--;
	}else{
	    /* No data available yet */
	    /* don't wait if non-blocking */
	    if( file->f_flags & O_NONBLOCK ) {
		return( cnt ? cnt : -EAGAIN );
	    }
	    interruptible_sleep_on( &kb_waitq );
	    if( signal_pending(current) )
		return( cnt ? cnt : -EINTR );
	}
    }

    return cnt;
}


static struct file_operations kb_fops = {
   NULL,	/* lseek */
   kb_read,	/* read */
   NULL,	/* write */
   NULL,	/* readdir */
   NULL,	/* poll */
   kb_ioctl,	/* ioctl */
   NULL,	/* mmap */   
   kb_open,	/* open */
   NULL,	/* flush */
   kb_release	/* release */
};


/* Keyboard scanning task */
static void kb_scan( void *dummy );
static struct tq_struct kb_task = { NULL, 0, kb_scan, 0 };

static void kb_scan( void *dummy )
{
   int kb_nbr0 = kb_nbr;
   int r, c, code;
   long valcols;

   PRINTK( "kb_scan entered\n" );

#if 0
   /* process power switch events */
   if( power_sw_ev != NONE ) {
      code = KeybRows * KeybColumns;
      if( power_sw_ev == RELEASED ) {
	code |= 0x80;	/* released */
      }
      if( !(kb_mode == K_XLATE && power_sw_ev == RELEASED) ) {
	/* add power switch to key buffer */
	if( kb_nbr < KB_BUFSZ ) {
	   kb_buffer[kb_in] = (kb_mode == K_XLATE) ? kb_table[code] : code;
	   kb_in++;
	   kb_in %= KB_BUFSZ;
	   kb_nbr++;
	}
      }
      power_sw_ev = NONE;
   }
#endif

   /* scan all keyboard rows */
   for( r = 0; r < KeybRows; r++ )
     {
	/* We set only one row to output while all the others are 
	 * set as input i.e. high impedence to avoid shorts.
	 */
	GPDR &= ~AllRows;
	GPDR |= K_ROW(r);
	udelay(100);	/* setup time */
	valcols = GPLR;		/* get column values */

	/* check all columns */
	for( c = 0; c < KeybColumns; c++ )
	  {
	     code = r * KeybColumns + c;

	     /* look for pressed keys */
	     if( ((valcols & K_COL(c)) && !kb_state[code]) ||
		 (kb_state[code] > REPEAT_EXPIRE) )
	       {
		  kb_state[code] = (kb_state[code]) ? REPEAT_REINIT : REPEAT_INIT;
		  PRINTK( "kb: %d (%d,%d) pressed\n", code, c, r );
		  if( kb_nbr < KB_BUFSZ )
		    {
		       kb_buffer[kb_in] = 
				(kb_mode == K_XLATE) ? kb_table[code] : code;
		       kb_in++;
		       kb_in %= KB_BUFSZ;
		       kb_nbr++;
		    }
	       }
	     
	     /* look for released keys */
	     if( !(valcols & K_COL(c)) && kb_state[code] )
	       {
		  kb_state[code] = 0;
		  PRINTK( "kb: %d (%d,%d) released\n", code, c, r );
		  if( kb_nbr < KB_BUFSZ && kb_mode != K_XLATE )
		    {
		       kb_buffer[kb_in] = code | 0x80;
		       kb_in++;
		       kb_in %= KB_BUFSZ;
		       kb_nbr++;
		    }
	       }
	     
	     if( kb_state[code] ) {
		/* key still pressed, update typematic counters */
		kb_state[code]++;
		/* schedule another keyboard scan for next timer tick */
		queue_task( &kb_task, &tq_timer );
	     }
	  }
     }

   /* put all rows back to output mode and clear edge detection */
   GPSR = AllRows;   
   GPDR |= AllRows;
   GEDR = AllCols;

   /* wake up any possible process waiting for keypresses if we got any */
   if( kb_nbr != kb_nbr0 ) wake_up_interruptible( &kb_waitq );
}


static void kb_interrupt( int irq, void *dev_id, struct pt_regs *regs )
{
    GEDR = AllCols;
    queue_task( &kb_task, &tq_timer );
}



static void powersw_task( unsigned long tries )
{
    printk( "CTRL-ALT_DEL simulated\n" );
    ctrl_alt_del();
}

static struct timer_list powersw_timer;

static void powersw_irq( int irq, void *dev_id, struct pt_regs *regs )
{
    static int sw_state = 0;
    static int sw_tries = 0;
    static long sw_time = 0;

    if( !sw_state && (GPLR & GPIO_GPIO2) ) {
	/* power switch has just been pressed */
	sw_state = 1;
	power_sw_ev = PRESSED;
	sw_tries++;
	sw_time = jiffies;
	/* schedule a CTRL-ALT_DEL if the power switch is being held long enough */
	powersw_timer.expires = sw_time + POWERSW_DELAY;
	powersw_timer.data = sw_tries;
	powersw_timer.function = powersw_task;
	add_timer( &powersw_timer );
    }else if( sw_state ) {
	/* power switch has just been released */
	sw_state = 0;
	power_sw_ev = RELEASED;
	if( jiffies - sw_time < POWERSW_DELAY ) {
	    /* it was not pressed long enough, abort CTRL-ALT_DEL */
	    sw_tries--;
	    del_timer( &powersw_timer );
	}else if( jiffies - sw_time > POWERSW_DELAY_BRUTEFORCE ) {
	    /* drop power */
	    GPCR = GPIO_GPIO23;
	    panic( "bruteforce power off\n" );
	}
    }
    /* To queue power switch event into keyboard buffer */
    //queue_task( &kb_task, &tq_timer );  
}



static void __init powersw_init( void )
{
    int ret;

    /* set up interrupt on power switch line */
    GPDR &= ~GPIO_GPIO2;
    GEDR = GPIO_GPIO2;
    GRER |= GPIO_GPIO2;
    GFER |= GPIO_GPIO2;

    ret = request_irq( 2, powersw_irq, 0, "power switch", NULL );
    if( ret ) {
	panic( "unable to acquire irq for power switch" );
    }

    /* set GPIOs for power state stuff */
#ifdef CONFIG_VICTOR_BOARD1
    GPDR &= ~GPIO_GPIO27;
    GPDR |= GPIO24_26;
#else
    GPDR &= ~(GPIO_GPIO0 | GPIO_GPIO1);
    GPDR &= ~(GPIO_GPIO20 | GPIO_GPIO22 | GPIO24_27 );
#endif
}



int __init kb_victor_init( void )
{
    int ret;
   
    powersw_init();

    ret = register_chrdev( KB_MAJOR, "kb", &kb_fops );
    if( ret ) {
	printk( "kb: register_chrdev failed.\n" );
	return ret;
    } 
    ret = request_irq( KB_IRQ, kb_interrupt, 0, "kb", NULL );
    if( ret ) {
	printk( "kb: unable to acquire irq %d\n", KB_IRQ );
	unregister_chrdev( KB_MAJOR, "kb" );
	return ret;
    }

    /* Inittialize keyboard matrix lines */
    GPDR |= AllRows;
    GPDR &= ~( AllCols );
    GPSR = AllRows;
    GEDR  = AllCols;

    printk( "Victor keyboard initialized\n" );
   
    return 0;
} 


