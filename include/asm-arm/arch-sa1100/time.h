/*
 * linux/include/asm-arm/arch-sa1100/time.h
 *
 * Copyright (C) 1998 Deborah Wallach.
 * Twiddles  (C) 1999 Hugo Fiennes <hugo@empeg.com>
 *
 */

#include <asm/arch/hardware.h>
#include <asm/arch/irqs.h>

static volatile unsigned long next_os_timer_match = 0;
static volatile unsigned long last_os_timer_match = 0;

/* IRQs are disabled before entering here from do_gettimeofday() */
extern __inline__ unsigned long gettimeoffset (void)
{
	/* Vastly simplified workings here: instead of trying to work out if
	   there has been an unserviced interrupt during this time, our
           reset_timer() routine just keeps track of the *last* timer match
	   value (ie, the one where we last updated the OS timer). Even if
           we've missed an interrupt, this should be fine, as the offset we
	   return here will be greater than one tick interval (ie LATCH).
	   hugo@empeg.com 990703
	*/
	unsigned long offset;

	/* Get ticks since last timer service */
	offset=OSCR-last_os_timer_match;
	
	return (offset*tick)/LATCH;
}

/*
 * Reset the timer every time to get centisecond interrupts
 */
extern __inline__ int reset_timer (void)
{
	unsigned long flags;

	/* Time at which we can safely set a timer match interrupt - not too
	   close to current time for safety */
	const unsigned long safeperiod = 2;
	unsigned long safetime = OSCR+safeperiod;

	/* Disable IRQs during this timer update: this is done because
	   previously problems could occur with an IRQ from another source
	   interrupting this update routine, causing the match value to have
	   been passed by the time the OSMR was written to.
	   hugo@empeg.com
        */
	save_flags_cli(flags);
	last_os_timer_match=OSMR0;

        /* Clear match on timer 0 */
	OSSR=1;

	next_os_timer_match=last_os_timer_match+LATCH;

#if 0
	    if (next_os_timer_match >= safetime)
	    {
		if (next_os_timer_match - safetime < 0x80000000)
		    break;
	    }
	    else if (safetime - next_os_timer_match > 0x80000000)
		break;
#endif

	while (((next_os_timer_match >= safetime) && (next_os_timer_match - safetime > 0x80000000))
	       || ((next_os_timer_match < safetime) && (safetime - next_os_timer_match < 0x80000000))) {
		/* Too close to next interrupt, back off one. Reset last_os_timer_match to
		   the timer value which we would have set if it wasn't too close as we've
		   incremented lost_ticks & gettimeofday uses this as a reference point */
	        last_os_timer_match=next_os_timer_match;
		next_os_timer_match+=LATCH;
		lost_ticks++;
#if 0
		printk("last_os_timer_match=%lu, next_os_timer_match=%lu, lost_ticks=%d\n",
		       last_os_timer_match, next_os_timer_match, lost_ticks);
#endif
	}
	OSMR0=next_os_timer_match;
	restore_flags(flags);
	
	return(1);
}

/*
 * timer_interrupt() needs to keep up the real-time clock,
 * as well as call the "do_timer()" routine every clocktick.
 */
static void timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	if (reset_timer ())
		do_timer(regs);
}

static struct irqaction irqtimer = 
	{timer_interrupt, 0, 0, "timer", NULL, NULL};

/* Several routines to talk to the empeg mk2 (rev6 and onwards) RTC chip:
   this stores both a 32-bit time_t style value and a power on hours count.
   Currently, we don't bother with the POH count. The RTC is a Dallas 1602,
   hooked up to 2 spare output pins on the CS4231 sound controller: it then
   shares the I2C data line */
static __inline__ void setbits(int b)
{
	static volatile unsigned char *index=(unsigned char*)0xe0000040;
	static volatile unsigned char *data=(unsigned char*)0xe0000044;
	int t;

	/* Talk to RTC */
	*index=10;
	t=*data;
	*index=10; 
	*data=(t&0x3f)|(b<<6);
}

static __inline__ void rtc_sendcommand(int command)
{
	int a;

	for(a=0;a<8;a++) {
		/* Reset high, clock low */
		setbits(1);
	
		/* Set bit */
		if (command&(1<<a)) {
			/* Need a long delay after clearing bit due to rise
			   time of signal */
			GPCR=EMPEG_I2CDATA;
			udelay(9);
		} else GPSR=EMPEG_I2CDATA;
		
		/* Do the clock */
		udelay(1);
		setbits(3);
		udelay(1);
		setbits(1);
		udelay(1);
	}
}

static __inline__ unsigned long rtc_readdata(void)
{
	int a; unsigned long b=0;
	
	/* Reset high, clock low */
	setbits(1);
	
	/* Ensure data line is floating */
	GPCR=EMPEG_I2CDATA;
	udelay(10);

	for(a=0;a<32;a++) {
		/* Fetch data */
		udelay(1);
		b|=((GPLR&EMPEG_I2CDATAIN)?0:1)<<a;
		
		/* Do the clock */
		setbits(3);
		udelay(1);
		setbits(1);
		udelay(1);
	}
	
	/* Return value */
	return(b);
}

extern int set_rtc_time(unsigned long nowtime)
{
	/* Send "write rtc" command to rtc */
	rtc_sendcommand(0x80);

	/* Send new time */
	rtc_sendcommand((nowtime    )&0xff);
	rtc_sendcommand((nowtime>> 8)&0xff);
	rtc_sendcommand((nowtime>>16)&0xff);
	rtc_sendcommand((nowtime>>24)&0xff);
	
	/* End command */
	setbits(0);
}

extern unsigned long get_rtc_time(void)
{
	unsigned long t;

	/* Reset RTC */
	setbits(0);
	udelay(1);

	/* Send "read rtc" command to rtc */
	rtc_sendcommand(0x81);
	t=rtc_readdata();

	/* End command */
	setbits(0);
	return(t);
}

extern __inline__ unsigned long setup_timer (void)
{
  OSCR = 0;             /* initialize free-running timer register */
  OSMR0 = 0;            /* set up match register */
  OSSR = 0xf;  		/* clear status on all timers */
  OIER |= OIER_E0;      /* enable match on timer 0 to cause interrupts */
  reset_timer();

  setup_arm_irq(IRQ_OST0, &irqtimer);

  /*
   * Default the date to 1 Jan 1970 0:0:0
   * You will have to run a time daemon to set the
   * clock correctly at bootup
   */
  return mktime(1970, 1, 1, 0, 0, 0);
}
