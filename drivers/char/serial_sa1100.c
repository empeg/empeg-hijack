/*
 *  linux/drivers/char/serial_sa1100.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Extensively rewritten by Theodore Ts'o, 8/16/92 -- 9/14/92.  Now
 *  much more extensible to support other serial cards based on the
 *  16450/16550A UART's.  Added support for the AST FourPort and the
 *  Accent Async board.  
 *
 *  set_serial_info fixed to set the flags, custom divisor, and uart
 * 	type fields.  Fix suggested by Michael K. Johnson 12/12/92.
 *
 *  11/95: TIOCMIWAIT, TIOCGICOUNT by Angelo Haritsis <ah@doc.ic.ac.uk>
 *
 *  03/96: Modularised by Angelo Haritsis <ah@doc.ic.ac.uk>
 *
 *  rs_set_termios fixed to look also for changes of the input
 *      flags INPCK, BRKINT, PARMRK, IGNPAR and IGNBRK.
 *                                            Bernd Anhäupl 05/17/96.
 *
 *  1/97:  Extended dumb serial ports are a config option now.  
 *         Saves 4k.   Michael A. Griffith <grif@acm.org>
 * 
 *  8/97: Fix bug in rs_set_termios with RTS
 *        Stanislav V. Voronyi <stas@uanet.kharkov.ua>
 *
 *  3/98: Change the IRQ detection, use of probe_irq_o*(),
 *	  supress TIOCSERGWILD and TIOCSERSWILD
 *	  Etienne Lorrain <etienne.lorrain@ibm.net>
 *
 *  4/98: Added changes to support the ARM architecture proposed by
 * 	  Russell King
 *
 *  1/99: Changes for SA1100 internal async UARTs
 *        Hugo Fiennes <hugo@empeg.com>
 *  Lots of changes, based on Deborah Wallace's Itsy/Brutus port, but
 *  with fixes which seem to cure repeated byte problems.
 *  This is no longer a 16x50 UART driver...
 *
 *  2/99: Cleanup: Now this can coexist with the generic serial driver
 *	  and cleanly integrate into regular driver location.
 *	  Nicolas Pitre <nico@cam.org>
 *
 *  6.6.99: handling Break conditions to break endless loops in rs_interrupt_single
 *          Keith  keith@keith-koep.com
 */

/*
 * Serial driver configuration section.  Here are the various options:
 *
 * SERIAL_PARANOIA_CHECK
 * 		Check the magic number for the async_structure where
 * 		ever possible.
 */

#undef SERIAL_PARANOIA_CHECK
#define CONFIG_SERIAL_NOPAUSE_IO
#define SERIAL_DO_RESTART

/* Set of debugging defines */

#undef SERIAL_DEBUG_INTR
#undef SERIAL_DEBUG_OPEN
#undef SERIAL_DEBUG_FLOW
#undef SERIAL_DEBUG_RS_WAIT_UNTIL_SENT

#define RS_STROBE_TIME (10*HZ)
#define RS_ISR_PASS_LIMIT 256

#define IRQ_T(info) ((info->flags & ASYNC_SHARE_IRQ) ? SA_SHIRQ : SA_INTERRUPT)

#define SERIAL_INLINE
  
#if defined(MODULE) && defined(SERIAL_DEBUG_MCOUNT)
#define DBG_CNT(s) printk("(%s): [%x] refc=%d, serc=%d, ttyc=%d -> %s\n", \
 kdevname(tty->device), (info->flags), serial_refcount,info->count,tty->count,s)
#else
#define DBG_CNT(s)
#endif

/*
 * End of serial driver configuration section.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/init.h>
#include <linux/delay.h>
#ifdef CONFIG_SERIAL_SA1100_CONSOLE
#include <linux/console.h>
#endif

#include <asm/system.h>
#include <asm/irq.h>
#include <asm/uaccess.h>
#include <asm/bitops.h>
#include <asm/arch/hardware.h>
#include <asm/arch/serial_reg.h>

#ifdef SERIAL_INLINE
#define _INLINE_ inline
#endif

static char *serial_name = "SA1100 serial driver";
static char *serial_version = "4.27";

static DECLARE_TASK_QUEUE(tq_serial);

static struct tty_driver serial_driver, callout_driver;
static int serial_refcount;

/* number of characters left in xmit buffer before we ask for more */
#define WAKEUP_CHARS 256

/*
 * IRQ_timeout		- How long the timeout should be for each IRQ
 * 				should be after the IRQ has been active.
 */

static struct async_struct *IRQ_ports[NR_IRQS];
static int IRQ_timeout[NR_IRQS];
#ifdef CONFIG_SERIAL_SA1100_CONSOLE
static struct console sercons;
#endif

static void autoconfig(struct serial_state * info);
static void change_speed(struct async_struct *info);
static void rs_wait_until_sent(struct tty_struct *tty, int timeout);

#define PORT_SA1100_UART 1

static struct serial_uart_config uart_config[] = {
        { "unknown", 1, 0 },
	{ "SA1100 UART", 4, 0 }, 
	{ 0, 0}
};

#define BASE_BAUD (3686400 / 16)
#define STD_COM_FLAGS (ASYNC_BOOT_AUTOCONF | ASYNC_SKIP_TEST)

static struct serial_state rs_table[] = {
#if defined( CONFIG_SA1100_BRUTUS )
  { 0, BASE_BAUD, (unsigned long) &Ser1UTCR0, IRQ_Ser1UART, STD_COM_FLAGS },
  { 0, BASE_BAUD, (unsigned long) &Ser3UTCR0, IRQ_Ser3UART, STD_COM_FLAGS }
#elif defined ( CONFIG_SA1100_EMPEG )
  { 0, BASE_BAUD, (unsigned long) &Ser1UTCR0, IRQ_Ser1UART, STD_COM_FLAGS },
  { 0, BASE_BAUD, (unsigned long) &Ser3UTCR0, IRQ_Ser3UART, STD_COM_FLAGS },
  { 0, BASE_BAUD, (unsigned long) &Ser2UTCR0, IRQ_Ser2ICP, STD_COM_FLAGS }
#elif defined( CONFIG_SA1100_TIFON )
  { 0, BASE_BAUD, (unsigned long) &Ser1UTCR0, IRQ_Ser1UART, STD_COM_FLAGS },
  { 0, BASE_BAUD, (unsigned long) &Ser3UTCR0, IRQ_Ser3UART, STD_COM_FLAGS }
#else
  { 0, BASE_BAUD, (unsigned long) &Ser3UTCR0, IRQ_Ser3UART, STD_COM_FLAGS },
  { 0, BASE_BAUD, (unsigned long) &Ser1UTCR0, IRQ_Ser1UART, STD_COM_FLAGS }
#endif
};

#define NR_PORTS	(sizeof(rs_table)/sizeof(struct serial_state))

static struct tty_struct *serial_table[NR_PORTS];
static struct termios *serial_termios[NR_PORTS];
static struct termios *serial_termios_locked[NR_PORTS];

#ifndef MIN
#define MIN(a,b)	((a) < (b) ? (a) : (b))
#endif

/* This is to be compatible with struct async_struct from serialP.h
 * using the read_status_mask and ignore_status_mask fields.
 * Since we need them twice we split them into two 16 bits parts.
 * Those fields are int i.e. 32 bits and we only need 8 bits for each part
 * so we're OK.
 * Ex: instead of info->read_status1_mask, use _V1(info->read_status_mask).
 */
#define _V0( mask )	(((unsigned short *)(&(mask)))[0])
#define _V1( mask )	(((unsigned short *)(&(mask)))[1])

/*
 * tmp_buf is used as a temporary buffer by serial_write.  We need to
 * lock it in case the copy_from_user blocks while swapping in a page,
 * and some other program tries to do a serial write at the same time.
 * Since the lock will only come under contention when the system is
 * swapping and available memory is low, it makes sense to share one
 * buffer across all the serial ports, since it significantly saves
 * memory if large numbers of serial ports are open.
 */
static unsigned char *tmp_buf;
static struct semaphore tmp_buf_sem = MUTEX;

static inline int serial_paranoia_check(struct async_struct *info,
					kdev_t device, const char *routine)
{
#ifdef SERIAL_PARANOIA_CHECK
	static const char *badmagic =
		"Warning: bad magic number for serial struct (%s) in %s\n";
	static const char *badinfo =
		"Warning: null async_struct for (%s) in %s\n";

	if (!info) {
		printk(badinfo, kdevname(device), routine);
		return 1;
	}
	if (info->magic != SERIAL_MAGIC) {
		printk(badmagic, kdevname(device), routine);
		return 1;
	}
#endif
	return 0;
}

/* These all read/write 32 bits as this is what the SA1100 data book says to do */
static inline unsigned int serial_in(struct async_struct *info, int offset)
{
	return ((volatile unsigned long *)info->port)[offset];
}

#define serial_inp	serial_in

static inline void serial_out(struct async_struct *info, int offset, int value)
{
	((volatile unsigned long *)info->port)[offset] = value;
}

#define serial_outp	serial_out


/*
 * ------------------------------------------------------------
 * rs_stop() and rs_start()
 *
 * This routines are called before setting or resetting tty->stopped.
 * They enable or disable transmitter interrupts, as necessary.
 * ------------------------------------------------------------
 */
static void rs_stop(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;

	if (serial_paranoia_check(info, tty->device, "rs_stop"))
		return;
	
	save_flags (flags); cli();
	if (info->IER & UTCR3_TIE) {
		info->IER &= ~UTCR3_TIE;
		serial_out(info, UTCR3, info->IER);
	}
	restore_flags(flags);
}

static void rs_start(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;
	
	if (serial_paranoia_check(info, tty->device, "rs_start"))
		return;
	
	save_flags(flags); cli();
	if (info->xmit_cnt && info->xmit_buf && !(info->IER & UTCR3_TIE)) {
		info->IER |= UTCR3_TIE;
		serial_out(info, UTCR3, info->IER);
	}
	restore_flags(flags);
}

/*
 * ----------------------------------------------------------------------
 *
 * Here starts the interrupt handling routines.  All of the following
 * subroutines are declared as inline and are folded into
 * rs_interrupt().  They were separated out for readability's sake.
 *
 * Note: rs_interrupt() is a "fast" interrupt, which means that it
 * runs with interrupts turned off.  People who may want to modify
 * rs_interrupt() should try to keep the interrupt handler as fast as
 * possible.  After you are done making modifications, it is not a bad
 * idea to do:
 * 
 * gcc -S -DKERNEL -Wall -Wstrict-prototypes -O6 -fomit-frame-pointer serial.c
 *
 * and look at the resulting assemble code in serial.s.
 *
 * 				- Ted Ts'o (tytso@mit.edu), 7-Mar-93
 * -----------------------------------------------------------------------
 */

/*
 * This routine is used by the interrupt handler to schedule
 * processing in the software interrupt portion of the driver.
 */
static _INLINE_ void rs_sched_event(struct async_struct *info,
				  int event)
{
	info->event |= 1 << event;
	queue_task(&info->tqueue, &tq_serial);
	mark_bh(SERIAL_BH);
}

void hijack_serial_rx_insert (const char *buf, int size, int port)
{
#ifdef CONFIG_HIJACK_TUNER
	struct async_struct *info = port ? IRQ_ports[17] : IRQ_ports[15];

	if (!info) {
		printk("hijack_serial_rx_insert: serial port(%d) not currently open\n", port);
	} else {
		struct tty_struct *tty = info->tty;
		struct	async_icount *icount = &info->state->icount;
		unsigned long flags;

		save_flags_clif(flags);
		while (size-- > 0) {
			while (tty->flip.count >= TTY_FLIPBUF_SIZE)
				schedule();	// wait for some room in buffer
			*tty->flip.char_buf_ptr = *buf++;
			icount->rx++;
			*tty->flip.flag_buf_ptr = 0;
			tty->flip.flag_buf_ptr++;
			tty->flip.char_buf_ptr++;
			tty->flip.count++;
		}
		tty_flip_buffer_push(tty);
		restore_flags(flags);
	}
#endif // CONFIG_HIJACK_TUNER
}

extern int hijack_fake_tuner, hijack_trace_tuner;
static int tuner_loopback = 0;

static _INLINE_ void receive_chars(struct async_struct *info,
				 int *status0, int *status1)
{
	struct tty_struct *tty = info->tty;
	unsigned char ch;
	int ignored = 0;
	struct	async_icount *icount;

	icount = &info->state->icount;
	do {
		ch = serial_inp(info, UART_RX);
		if (tty->flip.count >= TTY_FLIPBUF_SIZE)
			break;
		*tty->flip.char_buf_ptr = ch;
		icount->rx++;

#ifdef CONFIG_HIJACK_TUNER
		// Feed Tuner packets into Hijack
		if (info == IRQ_ports[15]) {	// Tuner interface (ttyS0)
			static int pktlen = 0;
			static unsigned int stalk = 0;
			extern int hijack_stalk_enabled;
			if (tuner_loopback || !hijack_stalk_enabled)
				goto ignore_char;
			if (hijack_trace_tuner)
				printk("tuner: in=%02x\n", ch);
			if (pktlen) {
				--pktlen;
				if (stalk) {
					stalk = (stalk << 8) | ch;
					if (pktlen == 0) {
						extern void hijack_intercept_stalk(unsigned int);
						hijack_intercept_stalk(htonl(stalk));
						stalk = 0;
					}
					goto ignore_char;
				}
			} else if (ch == 0x02) {
				stalk  = ch;
				pktlen = 3;
				goto ignore_char;
			} else if (ch == 0x01 || ch == 0x03) {	// FIXME: what do RDS packets use?
				pktlen = 3;
			}
			//
			// Since we're getting REAL tuner responses,
			// we want to turn off the "fake_tuner" (if enabled).
			// But we can only safely do so after the END of
			// the first full tuner packet (which we discard here,
			// to avoid duplication from the fake_tuner repsonses.
			//
			if (hijack_fake_tuner) {
				if (pktlen == 0) {
					hijack_fake_tuner = 0;
					printk("Hijack: setting fake_tuner=0\n");
				}
				goto ignore_char;
			}
		}
#endif // CONFIG_HIJACK_TUNER

#ifdef SERIAL_DEBUG_INTR
		printk("DR%02x:%02x/%02x...", ch, *status0, *status1);
#endif
		*tty->flip.flag_buf_ptr = 0;
		if (
                    (*status1 & (UTSR1_PRE | UTSR1_FRE | UTSR1_ROR))) {
			/*
			 * For statistics only
			 */
			if (*status1 & UTSR1_PRE)
				icount->parity++;
			else if (*status1 & UTSR1_FRE)
				icount->frame++;
			if (*status1 & UTSR1_ROR)
				icount->overrun++;

			/*
			 * Now check to see if character should be
			 * ignored, and mask off conditions which
			 * should be ignored.
			 */
			if (*status1 & _V1(info->ignore_status_mask)) {
				if (++ignored > 100)
					break;
				goto ignore_char;
			}
			*status1 &= _V1(info->read_status_mask);
		
			if (*status0 & (UTSR0_RBB)) {
#ifdef SERIAL_DEBUG_INTR
				printk("handling break....");
#endif
				*tty->flip.flag_buf_ptr = TTY_BREAK;
				if (info->flags & ASYNC_SAK)
					do_SAK(tty);
			} else if (*status1 & UTSR1_PRE)
				*tty->flip.flag_buf_ptr = TTY_PARITY;
			else if (*status1 & UTSR1_FRE)
				*tty->flip.flag_buf_ptr = TTY_FRAME;
			if (*status1 & UTSR1_ROR) {
				/*
				 * Overrun is special, since it's
				 * reported immediately, and doesn't
				 * affect the current character
				 */
				if (tty->flip.count < TTY_FLIPBUF_SIZE) {
					tty->flip.count++;
					tty->flip.flag_buf_ptr++;
					tty->flip.char_buf_ptr++;
					*tty->flip.flag_buf_ptr = TTY_OVERRUN;
				}
			}
		}
		tty->flip.flag_buf_ptr++;
		tty->flip.char_buf_ptr++;
		tty->flip.count++;
	ignore_char:
		*status1 = serial_inp(info, UTSR1);
	} while (*status1 & UTSR1_RNE);
	tty_flip_buffer_push(tty);

	/* Clear receiver idle state if necessary */
	if (*status0 & UTSR0_RID)
	  {
  	  /* need to write a 1 back to clear it */
	  serial_out(info, UTSR0, UTSR0_RID);
	  }
}

#ifdef CONFIG_HIJACK_TUNER

static char cmd4[] = {0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x39,0x43,0x4a,0x4e,0x51,0x3f,0x40,0x48,0x00};
static char rsp4[] = {0x07,0x00,0x35,0x22,0x3a,0x44,0x40,0x66,0x48,0x89,0x51,0xab,0x5a,0xcd,0x66,0x64,0x74,0x36,0x07,0x08,0x03,0xde,0x6c,0x00};

static void
fake_tuner (unsigned char c)
{
	static unsigned char state = 0, ctype = 0, eat = 0;
	unsigned int response, i;

	switch (state) {
		case 0:
			if (c == 0x01)
				state = 1;
			else if (hijack_trace_tuner)
				printk("fake_tuner: ignored=%02x\n", c);
			return;
		case 1:
			state = 2;
			ctype = c;
			switch (ctype) {
				default:
					printk("fake_tuner: unknown msg type, ignored=%02x\n", c);
					state = 0;
					return;
				case 0x04: eat = 2; break;
				case 0x05: eat = 1; break;
				case 0x03: eat = 4; break;
				case 0x01: eat = 9; break;
				case 0x00: eat =10; break; // read module id, and set LED on/off
				case 0xff: eat = 1; break;
				case 0x09: eat = 1; break; // read tuner dial (tuner ID)
			}
			// fall thru
		default:
			if (--eat)
				return;
			state = 0;
			switch (ctype) {
				case 0x04:
					cmd4[sizeof(cmd4)-1] = c;
					for (i = 0; cmd4[i] != c; ++i);
					c = rsp4[i];
					response = 0x00000401 | (c << 16);
					break;
				case 0x05: response = 0x00000501; break;
				case 0x03: response = 0x00000301; break;
				case 0x01: response = 0x00000101; break;
				case 0x00: response = 0x00030001; break; // read module id, and set LED on/off: tuner == 0x03
				case 0xff: response = 0x0027ff01; break;
				case 0x09: response = 0x00080901; break; // read tuner dial: pretend it's set to '8'
				default:
					printk("fake_tuner: unknown ctype=%02x\n", ctype);
					return;
			}
			response |= ((response >> 16) + (response >> 8)) << 24;
			if (hijack_trace_tuner)
				printk("fake_tuner: insert=%08x\n", ntohl(response));
			hijack_serial_rx_insert ((char *)&response, 4, 0);
	}
}
#endif CONFIG_HIJACK_TUNER

static _INLINE_ void transmit_chars(struct async_struct *info, int *intr_done)
{
	int count;
	
	if (info->x_char) {
		serial_outp(info, UART_TX, info->x_char);
		info->state->icount.tx++;
		info->x_char = 0;
		if (intr_done)
			*intr_done = 0;
		return;
	}
	if ((info->xmit_cnt <= 0) || info->tty->stopped ||
	    info->tty->hw_stopped) {
   	        if (info->IER&UTCR3_TIE) {
		    info->IER &= ~UTCR3_TIE;
		    serial_out(info, UTCR3, info->IER);
                }
 
                /* No more TFS events to service */
		_V0(info->read_status_mask)&=~UTSR0_TFS;
		return;
	}
	
	/* Tried using FIFO (not checking TNF) for fifo fill: still had the
	 * '4 bytes repeated' problem.
	 */
	count = info->xmit_fifo_size;
	while(serial_inp(info, UTSR1) & UTSR1_TNF) {
		char ch = info->xmit_buf[info->xmit_tail++];
#ifdef CONFIG_HIJACK_TUNER
		if (info == IRQ_ports[15]) {
			if (hijack_trace_tuner)
				printk("tuner:out=%02x\n", ch);
			if (hijack_fake_tuner)
				fake_tuner(ch);
		}
#endif // CONFIG_HIJACK_TUNER
		serial_out(info, UART_TX, ch);
		info->xmit_tail &= (SERIAL_XMIT_SIZE-1);
                info->state->icount.tx++;
		if (--info->xmit_cnt <= 0)
		  break;
	};

	if (info->xmit_cnt < WAKEUP_CHARS)
		rs_sched_event(info, RS_EVENT_WRITE_WAKEUP);

#ifdef SERIAL_DEBUG_INTR
	printk("THRE...");
#endif
	if (intr_done)
		*intr_done = 0;

	if (info->xmit_cnt <= 0) {
	        if (info->IER & UTCR3_TIE) {
		    info->IER &= ~UTCR3_TIE;
		    serial_out(info, UTCR3, info->IER);
		}

		/* No more TFS events to service */
                _V0(info->read_status_mask)&=~UTSR0_TFS;
	}
}

#ifdef CONFIG_SMC9194_TIFON	// Mk2 or later? (Mk1 has no ethernet chip)
extern unsigned long	jiffies_since(unsigned long);

static int
hijack_read_serial (volatile unsigned long *tuner_port, unsigned long interval)
{
	unsigned long	timestamp = jiffies;

	while (jiffies_since(timestamp) < interval) {
		if (tuner_port[UTSR1] & UTSR1_RNE) {
			int c = tuner_port[UART_RX];
			//printk("hijack_read_serial: 0x%02x\n", c);
			return c;
		}
		schedule();
	}
	//printk("hijack_read_serial: timed-out\n");
	return -1;	// timed out
}

void
hijack_read_tuner_id (int *loopback, int *tuner_id)
{
	int			rc;
	unsigned char		pattern = 0x5a;
	volatile unsigned long	*tuner_port = (unsigned long *)&Ser1UTCR0;

	//
	// Initialize the UART from scratch
	//
	tuner_port[UTCR3] = 0x00;		// turn off rx/tx
	tuner_port[UTCR0] = 0x0a;		// 8N1
	tuner_port[UTCR1] = 0x00;		// 19200
	tuner_port[UTCR2] = 0x0b;		//
	tuner_port[UTSR0] = 0xff;		// clear errors
	tuner_port[UTCR3] = UTCR3_RXE|UTCR3_TXE; // turn on rx/tx
	//
	// Perform loopback test, looking for a docking station.
	//
	tuner_port[UART_TX] = pattern;
	rc = hijack_read_serial(tuner_port, HZ/4);
	if (rc == pattern) {
		//
		// first loopback appeared to work, try it again to make sure
		//
		pattern = 0x33;
		tuner_port[UART_TX] = pattern;
		rc = hijack_read_serial(tuner_port, HZ/4);
	}
	//
	// If we get our data echoed back, then we've found a docking station
	//
	if (rc == pattern) {
		*loopback = 1;
		tuner_loopback = 1;
	} else if (rc != -1) {
		//
		// Tuner appears to be present.  It starts up with a stream of data,
		// but we ignore that and just issue the "read knob/jumpers" command.
		//
		while (-1 != hijack_read_serial(tuner_port, HZ/16));

		tuner_port[UART_TX] = 0x01;
		tuner_port[UART_TX] = 0x09;
		//
		// Now loop, discarding data until we see a new response beginning with 0x01
		//
		do {
			rc = hijack_read_serial(tuner_port, HZ/8);
		} while (rc != -1 && rc != 0x01);
		//
		// Now grab the entire 4-byte response, check validity, and extract the tuner_id
		//
		if (rc == 0x01 && hijack_read_serial(tuner_port, HZ/16) == 0x09) {
			int id, chk;
			id  = hijack_read_serial(tuner_port, HZ/16);
			chk = hijack_read_serial(tuner_port, HZ/16);
			if (id != -1 && chk != -1 && ((id + 9) & 0xff) == chk) {
				*tuner_id = id;
			}
		}
	}
}
#endif // CONFIG_SMC9194_TIFON

/*
 * This is the serial driver's interrupt routine for a single port
 */
static void rs_interrupt_single(int irq, void *dev_id, struct pt_regs * regs)
{
	int status0, status1;
	int pass_counter = 0;
	struct async_struct * info;
	
#ifdef SERIAL_DEBUG_INTR
	printk("rs_interrupt_single(%d)...", irq);
#endif
	
	info = IRQ_ports[irq];
	if (!info || !info->tty)
		return;

	/* We want to know about TFS events for now: we'll clear this later
	   if we have nothing to send */
	_V0(info->read_status_mask)|=UTSR0_TFS;
	do {
		status0 = serial_inp(info, UTSR0) & _V0(info->read_status_mask);
		status1 = serial_inp(info, UTSR1) & _V1(info->read_status_mask);
#ifdef SERIAL_DEBUG_INTR
		printk("status0 = %x (%x), status1 = %x (%x)...", status0, serial_inp(info,UTSR0), status1, serial_inp(info,UTSR1));
#endif
		if ((status1 & UTSR1_RNE) || (status0 & UTSR0_RID))
			receive_chars(info, &status0, &status1);
		if (status0 & UTSR0_TFS)
			transmit_chars(info, 0);
		if (status0 & (UTSR0_RBB | UTSR0_REB)) {
		        struct async_icount *icount=&info->state->icount;
			struct tty_struct *tty = info->tty;

		        if (status0 & UTSR0_RBB) {
			        icount->brk++;

				/* Clear break signal by writing break bit */
				serial_outp(info,UTSR0,UTSR0_RBB);
#ifdef SERIAL_DEBUG_INTR
				printk("handling break....");
#endif
				*tty->flip.flag_buf_ptr = TTY_BREAK;
				if (info->flags & ASYNC_SAK)
					do_SAK(tty);
			} else if (status0 & UTSR0_REB) {
                                /* Clear break signal by writing bit */
			        serial_outp(info,UTSR0,UTSR0_REB);
			}
		}
		if (pass_counter++ > RS_ISR_PASS_LIMIT) {
			if (status0 || status1) printk("out-status0 = %x, status1 = %x...", status0, status1);
			printk("rs_single loop break.\n");
			break;
		}
		status0 = serial_inp(info, UTSR0) & _V0(info->read_status_mask);
		status1 = serial_inp(info, UTSR1) & _V1(info->read_status_mask);
#ifdef SERIAL_DEBUG_INTR
		if (status0 || status1) printk("out-status0 = %x, status1 = %x...", status0, status1);
#endif

	} while (status0 || status1);
	info->last_active = jiffies;

#ifdef SERIAL_DEBUG_INTR
	printk("end.\n");
#endif
}

/*
 * -------------------------------------------------------------------
 * Here ends the serial interrupt routines.
 * -------------------------------------------------------------------
 */

/*
 * This routine is used to handle the "bottom half" processing for the
 * serial driver, known also the "software interrupt" processing.
 * This processing is done at the kernel interrupt level, after the
 * rs_interrupt() has returned, BUT WITH INTERRUPTS TURNED ON.  This
 * is where time-consuming activities which can not be done in the
 * interrupt driver proper are done; the interrupt driver schedules
 * them using rs_sched_event(), and they get done here.
 */
static void do_serial_bh(void)
{
	run_task_queue(&tq_serial);
}

static void do_softint(void *private_)
{
	struct async_struct	*info = (struct async_struct *) private_;
	struct tty_struct	*tty;
	
	tty = info->tty;
	if (!tty)
		return;

	if (test_and_clear_bit(RS_EVENT_WRITE_WAKEUP, &info->event)) {
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
		    tty->ldisc.write_wakeup)
			(tty->ldisc.write_wakeup)(tty);
		wake_up_interruptible(&tty->write_wait);
	}
}

/*
 * This subroutine is called when the RS_TIMER goes off.  It is used
 * by the serial driver to handle ports that do not have an interrupt
 * (irq=0).  This doesn't work very well for 16450's, but gives barely
 * passable results for a 16550A.  (Although at the expense of much
 * CPU overhead).
 */
static void rs_timer(void)
{
	static unsigned long last_strobe = 0;
	struct async_struct *info;
	unsigned int	i;
	unsigned long flags;

	if ((jiffies - last_strobe) >= RS_STROBE_TIME) {
		for (i=0; i < NR_IRQS; i++) {
			info = IRQ_ports[i];
			if (!info)
				continue;
			save_flags(flags); cli();
				rs_interrupt_single(i, NULL, NULL);
			restore_flags(flags);
		}
	}
	last_strobe = jiffies;
	timer_table[RS_TIMER].expires = jiffies + RS_STROBE_TIME;
	timer_active |= 1 << RS_TIMER;

	if (IRQ_ports[0]) {
		save_flags(flags); cli();
		rs_interrupt_single(0, NULL, NULL);
		restore_flags(flags);

		timer_table[RS_TIMER].expires = jiffies + IRQ_timeout[0] - 2;
	}
}

/*
 * ---------------------------------------------------------------
 * Low level utility subroutines for the serial driver:  routines to
 * figure out the appropriate timeout for an interrupt chain, routines
 * to initialize and startup a serial port, and routines to shutdown a
 * serial port.  Useful stuff like that.
 * ---------------------------------------------------------------
 */

/*
 * This routine figures out the correct timeout for a particular IRQ.
 * It uses the smallest timeout of all of the serial ports in a
 * particular interrupt chain.  Now only used for IRQ 0....
 */
static void figure_IRQ_timeout(int irq)
{
	struct	async_struct	*info;
	int	timeout = 60*HZ;	/* 60 seconds === a long time :-) */

	info = IRQ_ports[irq];
	if (!info) {
		IRQ_timeout[irq] = 60*HZ;
		return;
	}
	while (info) {
		if (info->timeout < timeout)
			timeout = info->timeout;
		info = info->next_port;
	}
	if (!irq)
		timeout = timeout / 2;
	IRQ_timeout[irq] = timeout ? timeout : 1;
}

static int startup(struct async_struct * info)
{
	unsigned long flags;
	int	retval=0;
	void (*handler)(int, void *, struct pt_regs *);
	struct serial_state *state= info->state;
	unsigned long page;

	page = get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	save_flags(flags); cli();

	if (info->flags & ASYNC_INITIALIZED) {
		free_page(page);
		goto errout;
	}

	if (!state->port || !state->type) {
		if (info->tty)
			set_bit(TTY_IO_ERROR, &info->tty->flags);
		free_page(page);
		goto errout;
	}
	if (info->xmit_buf)
		free_page(page);
	else
		info->xmit_buf = (unsigned char *) page;

#ifdef SERIAL_DEBUG_OPEN
	printk("starting up ttys%d (irq %d)...", info->line, state->irq);
#endif

	
	/*
	 * Allocate the IRQ if necessary
	 */
	if (state->irq && (!IRQ_ports[state->irq] ||
			  !IRQ_ports[state->irq]->next_port)) {
		if (IRQ_ports[state->irq]) {
			retval = -EBUSY;
			goto errout;
		} else 
			handler = rs_interrupt_single;

		retval = request_irq(state->irq, handler, IRQ_T(info),
				     "serial", NULL);
		if (retval) {
			if (capable(CAP_SYS_ADMIN)) {
				if (info->tty)
					set_bit(TTY_IO_ERROR,
						&info->tty->flags);
				retval = 0;
			}
			goto errout;
		}
	}

	/*
	 * Insert serial port into IRQ chain.
	 */
	info->prev_port = 0;
	info->next_port = IRQ_ports[state->irq];
	if (info->next_port)
		info->next_port->prev_port = info;
	IRQ_ports[state->irq] = info;
	figure_IRQ_timeout(state->irq);

	/*
	 * Clear the interrupt registers.
	 */
        /* Clear out any error by writing the line status register */
        serial_outp(info, UTSR0, 0xff);

	/*
	 * Now, initialize the UART 
	 */
	/* set the baud rate and a word length of 8 (default for now) */
	//serial_outp(info, UTCR3, 0);
	serial_outp(info, UTCR1, 0);  
	serial_outp(info, UTCR2, 0x1);  /* 115200 */  

	serial_outp(info, UTCR0, UTCR0_DSS) ;

	/*
	 * Finally, enable interrupts
	 */
	info->IER = UTCR3_TXE | UTCR3_RXE | UTCR3_RIE;
	serial_outp(info, UTCR3, info->IER);	/* enable interrupts */
	
	/*
	 * And clear the interrupt registers again for luck.
	 */
	serial_outp(info, UTSR0, 0xff) ;

	if (info->tty)
		clear_bit(TTY_IO_ERROR, &info->tty->flags);
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;

	/*
	 * Set up serial timers...
	 */
	timer_table[RS_TIMER].expires = jiffies + 2*HZ/100;
	timer_active |= 1 << RS_TIMER;

	/*
	 * Set up the tty->alt_speed kludge
	 */
	if (info->tty) {
		if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
			info->tty->alt_speed = 57600;
		if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
			info->tty->alt_speed = 115200;
		if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_SHI)
			info->tty->alt_speed = 230400;
		if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_WARP)
			info->tty->alt_speed = 460800;
	}
	
	/*
	 * and set the speed of the serial port
	 */
	change_speed(info);

	info->flags |= ASYNC_INITIALIZED;
	restore_flags(flags);
	return 0;
	
errout:
	restore_flags(flags);
	return retval;
}

/*
 * This routine will shutdown a serial port; interrupts are disabled, and
 * DTR is dropped if the hangup on close termio flag is on.
 */
static void shutdown(struct async_struct * info)
{
	unsigned long	flags;
	struct serial_state *state;
	int		retval;

	if (!(info->flags & ASYNC_INITIALIZED))
		return;

	state = info->state;

#ifdef SERIAL_DEBUG_OPEN
	printk("Shutting down serial port %d (irq %d)....", info->line,
	       state->irq);
#endif
	
	save_flags(flags); cli(); /* Disable interrupts */

	/*
	 * clear delta_msr_wait queue to avoid mem leaks: we may free the irq
	 * here so the queue might never be waken up
	 */
	wake_up_interruptible(&info->delta_msr_wait);
	
	/*
	 * First unlink the serial port from the IRQ chain...
	 */
	if (info->next_port)
		info->next_port->prev_port = info->prev_port;
	if (info->prev_port)
		info->prev_port->next_port = info->next_port;
	else
		IRQ_ports[state->irq] = info->next_port;
	figure_IRQ_timeout(state->irq);
	
	/*
	 * Free the IRQ, if necessary
	 */
	if (state->irq && (!IRQ_ports[state->irq] ||
			  !IRQ_ports[state->irq]->next_port)) {
		if (IRQ_ports[state->irq]) {
			free_irq(state->irq, NULL);
			retval = request_irq(state->irq, rs_interrupt_single,
					     IRQ_T(info), "serial", NULL);
			
			if (retval)
				printk("serial shutdown: request_irq: error %d"
				       "  Couldn't reacquire IRQ.\n", retval);
		} else
			free_irq(state->irq, NULL);
	}

	if (info->xmit_buf) {
		free_page((unsigned long) info->xmit_buf);
		info->xmit_buf = 0;
	}

	info->IER = 0;
	serial_outp(info, UTCR3, 0x00);	/* disable all intrs */
	
	/* disable break condition */
	//serial_out(info, UART_LCR, serial_inp(info, UART_LCR) & ~UART_LCR_SBC);
	
	if (info->tty)
		set_bit(TTY_IO_ERROR, &info->tty->flags);

	/* Enter sleep mode? */

	info->flags &= ~ASYNC_INITIALIZED;
	restore_flags(flags);
}

/*
 * This routine is called to set the UART divisor registers to match
 * the specified baud rate for a serial port.
 */
static void change_speed(struct async_struct *info)
{
	unsigned int port;
	int	quot = 0, baud_base, baud;
	unsigned cflag, cval;
	int	bits=0;
	unsigned long	flags;

	if (!info->tty || !info->tty->termios)
		return;

	cflag = info->tty->termios->c_cflag;
	if (!(port = info->port))
		return;

	/* byte size and parity */
	switch (cflag & CSIZE) {
	      case CS7: cval = 0x00; break;
	      case CS8:
	      default:  cval = UTCR0_DSS; break;
	}
	if (cflag & CSTOPB) {
		cval |= UTCR0_SBS;
	}
	if (cflag & PARENB)
		cval |= UTCR0_PE;
	if (!(cflag & PARODD))
		cval |= UTCR0_OES;

	/* Determine divisor based on baud rate */
	baud = tty_get_baud_rate(info->tty);
	baud_base = info->state->baud_base;
	if (baud == 38400 &&
	    ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST))
		quot = info->state->custom_divisor;
	else {
		if (baud == 134)
			/* Special case since 134 is really 134.5 */
			quot = (2*baud_base / 269);
		else if (baud)
			quot = baud_base / baud;
	}
	/* If the quotient is ever zero, default to 9600 bps */
	if (!quot)
		quot = baud_base / 9600;
	info->quot = quot;
	info->timeout = ((info->xmit_fifo_size*HZ*bits*quot) / baud_base);
	info->timeout += HZ/50;		/* Add .02 seconds of slop */

	/* CTS flow control flag - we have no modem status */
	if (cflag & CRTSCTS) {
		info->flags |= ASYNC_CTS_FLOW;
	} else
		info->flags &= ~ASYNC_CTS_FLOW;
	if (cflag & CLOCAL)
		info->flags &= ~ASYNC_CHECK_CD;
	else {
		info->flags |= ASYNC_CHECK_CD;
	}
	serial_out(info, UTCR3, info->IER);

	/*
	 * Set up parity check flag
	 */
#define RELEVANT_IFLAG(iflag) (iflag & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK))

	_V0(info->read_status_mask) = UTSR0_TFS | UTSR0_RFS | UTSR0_RID | UTSR0_EIF;
	_V1(info->read_status_mask) = /*UTSR1_TBY |*/ UTSR1_RNE | /*UTSR1_TNF |*/ UTSR1_ROR;
	if (I_INPCK(info->tty))
		_V1(info->read_status_mask) |= UTSR1_PRE | UTSR1_FRE;

	/* We always need to know about breaks, so we can clear the bits */
	/*if (I_BRKINT(info->tty) || I_PARMRK(info->tty))*/
	_V0(info->read_status_mask) |= UTSR0_RBB | UTSR0_REB;
	
	/*
	 * Characters to ignore
	 */
	_V0(info->ignore_status_mask) = 0;
	_V1(info->ignore_status_mask) = 0;

	if (I_IGNBRK(info->tty)) {
		_V0(info->ignore_status_mask) |= UTSR0_RBB | UTSR0_REB;
		_V0(info->read_status_mask) |= UTSR0_RBB | UTSR0_REB;
		/*
		 * If we're ignore parity and break indicators, ignore 
		 * overruns too.  (For real raw support).
		 */
		if (I_IGNPAR(info->tty)) {
			_V1(info->ignore_status_mask) |= UTSR1_PRE | UTSR1_FRE | UTSR1_ROR;
			_V1(info->read_status_mask) |= UTSR1_PRE | UTSR1_FRE | UTSR1_ROR;
		}
	}

	/* Change the speed */
	{
	  unsigned int status1, IER_copy;
	  save_flags(flags); cli();
	  do {
	    status1 = serial_inp(info, UTSR1);
	  } while ( status1 & UTSR1_TBY );
    
	  /* must disable rx and tx enable to switch baud rate */
	  IER_copy = serial_inp(info, UTCR3) ;
	  serial_outp(info, UTCR3, 0) ;

	  /* set the parity, stop bits, data size */
	  serial_outp(info, UTCR0, cval);

	  /* set the baud rate */
	  quot -= 1;	/* divisor = divisor - 1 */
	  serial_outp(info, UTCR1, (quot >> 8)&0xf);	/* MS of divisor */
	  serial_outp(info, UTCR2, quot & 0xff);	/* LS of divisor */

	  /* clear out any errors by writing the line status register */
	  serial_outp(info, UTSR0, 0xff) ;

	  /* enable rx, tx and error interrupts */
	  serial_outp(info, UTCR3, IER_copy) ;

#ifdef CONFIG_SA1100_EMPEG
	  /* On flateric & above we need to configure up the endec too */
	  if (empeg_hardwarerevision()>=5 && info->port==(int)&Ser2UTCR0) {
		  static int endecset[]={115200,57600,19200,9600,38400,4800,2400,230400,0};
		  int a=0;

		  /* Match speed */
		  while(baud!=endecset[a] && endecset[a]!=0) a++;

		  /* Match? */
		  if (endecset[a]) {
			  /* Set outputs appropriately */
			  GPCR=(EMPEG_SIRSPEED0|EMPEG_SIRSPEED1|EMPEG_SIRSPEED2);
			  GPSR=((a&1)?EMPEG_SIRSPEED0:0)|((a&2)?EMPEG_SIRSPEED1:0)|((a&4)?EMPEG_SIRSPEED2:0);
		  }
	  }
#endif

	  restore_flags(flags);
	}
}

#ifdef CONFIG_SMC9194_TIFON	// Mk2 or later? (Mk1 has no ethernet chip)
extern int hijack_serial_notify (const unsigned char *, int);
#include <asm/arch/hijack.h>
#endif

static void rs_put_char(struct tty_struct *tty, unsigned char ch)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;

	if (serial_paranoia_check(info, tty->device, "rs_put_char"))
		return;

	if (!tty || !info->xmit_buf)
		return;
#ifdef CONFIG_SMC9194_TIFON	// Mk2 or later? (Mk1 has no ethernet chip)
	if (hijack_serial_notify(&ch, 1))
		return;
#endif

	save_flags(flags); cli();
	if (info->xmit_cnt >= SERIAL_XMIT_SIZE - 1) {
		restore_flags(flags);
		return;
	}

	info->xmit_buf[info->xmit_head++] = ch;
	info->xmit_head &= SERIAL_XMIT_SIZE-1;
	info->xmit_cnt++;
	restore_flags(flags);
}

static void rs_flush_chars(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;
				
	if (serial_paranoia_check(info, tty->device, "rs_flush_chars"))
		return;

	if (info->xmit_cnt <= 0 || tty->stopped || tty->hw_stopped ||
	    !info->xmit_buf)
		return;

	if (!(info->IER&UTCR3_TIE)) {
  	    save_flags(flags); cli();
	    info->IER |= UTCR3_TIE;
	    serial_out(info, UTCR3, info->IER);
	    restore_flags(flags);
	}
}

static int rs_write(struct tty_struct * tty, int from_user,
		    const unsigned char *buf, int count)
{
	int	c, ret = 0;
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;
				
	if (serial_paranoia_check(info, tty->device, "rs_write"))
		return 0;

	if (!tty || !info->xmit_buf || !tmp_buf)
		return 0;

	save_flags(flags);

#ifdef CONFIG_SMC9194_TIFON	// Mk2 or later? (Mk1 has no ethernet chip)
	//
	// v3alphas seem to have console/notify streams reversed from v2final.  Weird.
	//
	if (from_user == (player_version > MK2_PLAYER_v2final) && hijack_serial_notify(buf, count))
		ret = count;
	else
#endif
	if (from_user) {
		down(&tmp_buf_sem);
		while (1) {
			c = MIN(count,
				MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
				    SERIAL_XMIT_SIZE - info->xmit_head));
			if (c <= 0)
				break;

			c -= copy_from_user(tmp_buf, buf, c);
			if (!c) {
				if (!ret)
					ret = -EFAULT;
				break;
			}
			cli();
			c = MIN(c, MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
				       SERIAL_XMIT_SIZE - info->xmit_head));
			memcpy(info->xmit_buf + info->xmit_head, tmp_buf, c);
			info->xmit_head = ((info->xmit_head + c) &
					   (SERIAL_XMIT_SIZE-1));
			info->xmit_cnt += c;
			restore_flags(flags);
			buf += c;
			count -= c;
			ret += c;
		}
		up(&tmp_buf_sem);
	} else {
		while (1) {
			cli();		
			c = MIN(count,
				MIN(SERIAL_XMIT_SIZE - info->xmit_cnt - 1,
				    SERIAL_XMIT_SIZE - info->xmit_head));
			if (c <= 0) {
				restore_flags(flags);
				break;
			}
			memcpy(info->xmit_buf + info->xmit_head, buf, c);
			info->xmit_head = ((info->xmit_head + c) &
					   (SERIAL_XMIT_SIZE-1));
			info->xmit_cnt += c;
			restore_flags(flags);
			buf += c;
			count -= c;
			ret += c;

		}
	}
	if (info->xmit_cnt && !tty->stopped && !tty->hw_stopped &&
	    !(info->IER & UTCR3_TIE)) {
		info->IER |= UTCR3_TIE;
		serial_out(info, UTCR3, info->IER);
	}
	return ret;
}

static int rs_write_room(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	int	ret;
				
	if (serial_paranoia_check(info, tty->device, "rs_write_room"))
		return 0;
	ret = SERIAL_XMIT_SIZE - info->xmit_cnt - 1;
	if (ret < 0)
		ret = 0;
	return ret;
}

static int rs_chars_in_buffer(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
				
	if (serial_paranoia_check(info, tty->device, "rs_chars_in_buffer"))
		return 0;
	return info->xmit_cnt;
}

static void rs_flush_buffer(struct tty_struct *tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;
				
	if (serial_paranoia_check(info, tty->device, "rs_flush_buffer"))
		return;
	save_flags(flags); cli();
	info->xmit_cnt = info->xmit_head = info->xmit_tail = 0;
	restore_flags(flags);
	wake_up_interruptible(&tty->write_wait);
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);
}

/*
 * This function is used to send a high-priority XON/XOFF character to
 * the device
 */
static void rs_send_xchar(struct tty_struct *tty, char ch)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;

	if (serial_paranoia_check(info, tty->device, "rs_send_char"))
		return;

	info->x_char = ch;
	if (ch) {
		/* Make sure transmit interrupts are on */
		info->IER |= UTCR3_TXE;
		serial_out(info, UTCR3, info->IER);
	}
}

/*
 * ------------------------------------------------------------
 * rs_throttle()
 * 
 * This routine is called by the upper-layer tty layer to signal that
 * incoming characters should be throttled.
 * ------------------------------------------------------------
 */
static void rs_throttle(struct tty_struct * tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;
#ifdef SERIAL_DEBUG_THROTTLE
	char	buf[64];
	
	printk("throttle %s: %d....\n", tty_name(tty, buf),
	       tty->ldisc.chars_in_buffer(tty));
#endif

	if (serial_paranoia_check(info, tty->device, "rs_throttle"))
		return;
	
	if (I_IXOFF(tty))
		rs_send_xchar(tty, STOP_CHAR(tty));
#if 0 /* ZZZ not implemented - no hw lines */
	if (tty->termios->c_cflag & CRTSCTS)
		info->MCR &= ~UART_MCR_RTS;
#endif

	save_flags(flags); cli();
#if 0
	serial_out(info, UART_MCR, info->MCR);
#endif
	restore_flags(flags);
}

static void rs_unthrottle(struct tty_struct * tty)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;
#ifdef SERIAL_DEBUG_THROTTLE
	char	buf[64];
	
	printk("unthrottle %s: %d....\n", tty_name(tty, buf),
	       tty->ldisc.chars_in_buffer(tty));
#endif

	if (serial_paranoia_check(info, tty->device, "rs_unthrottle"))
		return;
	
	if (I_IXOFF(tty)) {
		if (info->x_char)
			info->x_char = 0;
		else
			rs_send_xchar(tty, START_CHAR(tty));
	}

#if 0 /* ZZZ not implemented */
	if (tty->termios->c_cflag & CRTSCTS)
		info->MCR |= UART_MCR_RTS;
#endif
	save_flags(flags); cli();
#if 0
	serial_out(info, UART_MCR, info->MCR);
#endif
	restore_flags(flags);
}

/*
 * ------------------------------------------------------------
 * rs_ioctl() and friends
 * ------------------------------------------------------------
 */

static int get_serial_info(struct async_struct * info,
			   struct serial_struct * retinfo)
{
	struct serial_struct tmp;
	struct serial_state *state = info->state;
   
	if (!retinfo)
		return -EFAULT;
	memset(&tmp, 0, sizeof(tmp));
	tmp.type = state->type;
	tmp.line = state->line;
	tmp.port = state->port;
	tmp.irq = state->irq;
	tmp.flags = state->flags;
	tmp.xmit_fifo_size = state->xmit_fifo_size;
	tmp.baud_base = state->baud_base;
	tmp.close_delay = state->close_delay;
	tmp.closing_wait = state->closing_wait;
	tmp.custom_divisor = state->custom_divisor;
	tmp.hub6 = state->hub6;
	if (copy_to_user(retinfo,&tmp,sizeof(*retinfo)))
		return -EFAULT;
	return 0;
}

static int set_serial_info(struct async_struct * info,
			   struct serial_struct * new_info)
{
	struct serial_struct new_serial;
 	struct serial_state old_state, *state;
	unsigned int		i,change_irq,change_port;
	int 			retval = 0;

	if (copy_from_user(&new_serial,new_info,sizeof(new_serial)))
		return -EFAULT;
	state = info->state;
	old_state = *state;
  
	change_irq = new_serial.irq != state->irq;
	change_port = (new_serial.port != state->port) ||
		(new_serial.hub6 != state->hub6);
  
	if (!capable(CAP_SYS_ADMIN)) {
		if (change_irq || change_port ||
		    (new_serial.baud_base != state->baud_base) ||
		    (new_serial.type != state->type) ||
		    (new_serial.close_delay != state->close_delay) ||
		    (new_serial.xmit_fifo_size != state->xmit_fifo_size) ||
		    ((new_serial.flags & ~ASYNC_USR_MASK) !=
		     (state->flags & ~ASYNC_USR_MASK)))
			return -EPERM;
		state->flags = ((state->flags & ~ASYNC_USR_MASK) |
			       (new_serial.flags & ASYNC_USR_MASK));
		state->custom_divisor = new_serial.custom_divisor;
		goto check_and_exit;
	}

	new_serial.irq = irq_cannonicalize(new_serial.irq);

	if ((new_serial.irq >= NR_IRQS) ||
	    (new_serial.type < PORT_UNKNOWN) ||
	    (new_serial.type > PORT_MAX)) {
		return -EINVAL;
	}

	if ((new_serial.type != state->type) ||
	    (new_serial.xmit_fifo_size <= 0))
		new_serial.xmit_fifo_size =
			uart_config[state->type].dfl_xmit_fifo_size;

	/* Make sure address is not already in use */
	if (new_serial.type) {
		for (i = 0 ; i < NR_PORTS; i++)
			if ((state != &rs_table[i]) &&
			    (rs_table[i].port == new_serial.port) &&
			    rs_table[i].type)
				return -EADDRINUSE;
	}

	if ((change_port || change_irq) && (state->count > 1))
		return -EBUSY;

	/*
	 * OK, past this point, all the error checking has been done.
	 * At this point, we start making changes.....
	 */

	state->baud_base = new_serial.baud_base;
	state->flags = ((state->flags & ~ASYNC_FLAGS) |
			(new_serial.flags & ASYNC_FLAGS));
	info->flags = ((state->flags & ~ASYNC_INTERNAL_FLAGS) |
		       (info->flags & ASYNC_INTERNAL_FLAGS));
	state->custom_divisor = new_serial.custom_divisor;
	state->type = new_serial.type;
	state->close_delay = new_serial.close_delay * HZ/100;
	state->closing_wait = new_serial.closing_wait * HZ/100;
	info->tty->low_latency = (info->flags & ASYNC_LOW_LATENCY) ? 1 : 0;
	info->xmit_fifo_size = state->xmit_fifo_size =
		new_serial.xmit_fifo_size;

	release_region(state->port,8);
	if (change_port || change_irq) {
		/*
		 * We need to shutdown the serial port at the old
		 * port/irq combination.
		 */
		shutdown(info);
		state->irq = new_serial.irq;
		info->port = state->port = new_serial.port;
		info->hub6 = state->hub6 = new_serial.hub6;
	}
	if (state->type != PORT_UNKNOWN)
		request_region(state->port,8,"serial(set)");

	
check_and_exit:
	if (!state->port || !state->type)
		return 0;
	if (state->flags & ASYNC_INITIALIZED) {
		if (((old_state.flags & ASYNC_SPD_MASK) !=
		     (state->flags & ASYNC_SPD_MASK)) ||
		    (old_state.custom_divisor != state->custom_divisor)) {
			if ((state->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
				info->tty->alt_speed = 57600;
			if ((state->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
				info->tty->alt_speed = 115200;
			if ((state->flags & ASYNC_SPD_MASK) == ASYNC_SPD_SHI)
				info->tty->alt_speed = 230400;
			if ((state->flags & ASYNC_SPD_MASK) == ASYNC_SPD_WARP)
				info->tty->alt_speed = 460800;
			change_speed(info);
		}
	} else
		retval = startup(info);
	return retval;
}


/*
 * get_lsr_info - get line status register info
 *
 * Purpose: Let user call ioctl() to get info when the UART physically
 * 	    is emptied.  On bus types like RS485, the transmitter must
 * 	    release the bus after transmitting. This must be done when
 * 	    the transmit shift register is empty, not be done when the
 * 	    transmit holding register is empty.  This functionality
 * 	    allows an RS485 driver to be written in user space. 
 */
static int get_lsr_info(struct async_struct * info, unsigned int *value)
{
	unsigned char status1;
	unsigned int result;
	unsigned long flags;

	save_flags(flags); cli();
	status1 = serial_in(info, UTSR1);
	restore_flags(flags);
	result = ((status1 & UTSR1_TBY) ? 0 : TIOCSER_TEMT);
	return put_user(result,value);
}


static int get_modem_info(struct async_struct * info, unsigned int *value)
{
	unsigned int result=0;
#if 0 /* ZZZ no modem lines */
	unsigned char control, status;
	unsigned long flags;

	control = info->MCR;
	save_flags(flags); cli();
	status = serial_in(info, UART_MSR);
	restore_flags(flags);
	result =  ((control & UART_MCR_RTS) ? TIOCM_RTS : 0)
		| ((control & UART_MCR_DTR) ? TIOCM_DTR : 0)
#ifdef TIOCM_OUT1
		| ((control & UART_MCR_OUT1) ? TIOCM_OUT1 : 0)
		| ((control & UART_MCR_OUT2) ? TIOCM_OUT2 : 0)
#endif
		| ((status  & UART_MSR_DCD) ? TIOCM_CAR : 0)
		| ((status  & UART_MSR_RI) ? TIOCM_RNG : 0)
		| ((status  & UART_MSR_DSR) ? TIOCM_DSR : 0)
		| ((status  & UART_MSR_CTS) ? TIOCM_CTS : 0);
#endif
	return put_user(result,value);
}

static int set_modem_info(struct async_struct * info, unsigned int cmd,
			  unsigned int *value)
{
	int error;
	unsigned int arg;
	unsigned long flags;

	error = get_user(arg, value);
	if (error)
		return error;
	switch (cmd) {
#if 0 /* NO LINES! */
	case TIOCMBIS: 
		if (arg & TIOCM_RTS)
			info->MCR |= UART_MCR_RTS;
		if (arg & TIOCM_DTR)
			info->MCR |= UART_MCR_DTR;
		break;
	case TIOCMBIC:
		if (arg & TIOCM_RTS)
			info->MCR &= ~UART_MCR_RTS;
		if (arg & TIOCM_DTR)
			info->MCR &= ~UART_MCR_DTR;
		break;
	case TIOCMSET:
		info->MCR = ((info->MCR & ~(UART_MCR_RTS |
					    UART_MCR_DTR))
			     | ((arg & TIOCM_RTS) ? UART_MCR_RTS : 0)
			     | ((arg & TIOCM_DTR) ? UART_MCR_DTR : 0));
		break;
#endif
	default:
		return -EINVAL;
	}

	save_flags(flags); cli();
#if 0
	serial_out(info, UART_MCR, info->MCR);
#endif
	restore_flags(flags);
	return 0;
}

static int do_autoconfig(struct async_struct * info)
{
	int			retval;
	
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	
	if (info->state->count > 1)
		return -EBUSY;
	
	shutdown(info);

	autoconfig(info->state);
#if 0
	if ((info->state->flags & ASYNC_AUTO_IRQ) &&
	    (info->state->port != 0) &&
	    (info->state->type != PORT_UNKNOWN))
		info->state->irq = detect_uart_irq(info->state);
#endif
	retval = startup(info);
	if (retval)
		return retval;
	return 0;
}

/*
 * rs_break() --- routine which turns the break handling on or off
 */
static void rs_break(struct tty_struct *tty, int break_state)
{
	struct async_struct * info = (struct async_struct *)tty->driver_data;
	unsigned long flags;
	
	if (serial_paranoia_check(info, tty->device, "rs_break"))
		return;

	if (!info->port)
		return;
	save_flags(flags); cli();
	if (break_state == -1)
		serial_out(info, UTCR3,
			   serial_inp(info, UTCR3) | UTCR3_BRK);
	else
		serial_out(info, UTCR3,
			   serial_inp(info, UTCR3) & ~UTCR3_BRK);
	restore_flags(flags);
}

static int rs_ioctl(struct tty_struct *tty, struct file * file,
                    unsigned int cmd, unsigned long arg)
{
        int error;
        struct async_struct * info = (struct async_struct *)tty->driver_data;
        struct async_icount cprev, cnow;        /* kernel counter temps */
        struct serial_icounter_struct *p_cuser; /* user space */
        unsigned long flags;

        if (serial_paranoia_check(info, tty->device, "rs_ioctl"))
                return -ENODEV;

        if ((cmd != TIOCGSERIAL) && (cmd != TIOCSSERIAL) &&
            (cmd != TIOCSERCONFIG) && (cmd != TIOCSERGSTRUCT) &&
            (cmd != TIOCMIWAIT) && (cmd != TIOCGICOUNT)) {
                if (tty->flags & (1 << TTY_IO_ERROR))
                    return -EIO;
        }

        switch (cmd) {
                case TIOCMGET:
                        return get_modem_info(info, (unsigned int *) arg);
                case TIOCMBIS:
                case TIOCMBIC:
                case TIOCMSET:
                        return set_modem_info(info, cmd, (unsigned int *) arg);
                case TIOCGSERIAL:
                        return get_serial_info(info,
                                               (struct serial_struct *) arg);
                case TIOCSSERIAL:
			return set_serial_info(info,
					       (struct serial_struct *) arg);
		case TIOCSERCONFIG:
			return do_autoconfig(info);

		case TIOCSERGETLSR: /* Get line status register */
			return get_lsr_info(info, (unsigned int *) arg);

		case TIOCSERGSTRUCT:
			if (copy_to_user((struct async_struct *) arg,
					 info, sizeof(struct async_struct)))
				return -EFAULT;
			return 0;
				
		/*
		 * Wait for any of the 4 modem inputs (DCD,RI,DSR,CTS) to change
		 * - mask passed in arg for lines of interest
 		 *   (use |'ed TIOCM_RNG/DSR/CD/CTS for masking)
		 * Caller should use TIOCGICOUNT to see which one it was
		 */
		case TIOCMIWAIT:
			save_flags(flags); cli();
			/* note the counters on entry */
			cprev = info->state->icount;
			restore_flags(flags);
			while (1) {
				interruptible_sleep_on(&info->delta_msr_wait);
				/* see if a signal did it */
				if (signal_pending(current))
					return -ERESTARTSYS;
				save_flags(flags); cli();
				cnow = info->state->icount; /* atomic copy */
				restore_flags(flags);
				if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr && 
				    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts)
					return -EIO; /* no change => error */
				if ( ((arg & TIOCM_RNG) && (cnow.rng != cprev.rng)) ||
				     ((arg & TIOCM_DSR) && (cnow.dsr != cprev.dsr)) ||
				     ((arg & TIOCM_CD)  && (cnow.dcd != cprev.dcd)) ||
				     ((arg & TIOCM_CTS) && (cnow.cts != cprev.cts)) ) {
					return 0;
				}
				cprev = cnow;
			}
			/* NOTREACHED */

		/* 
		 * Get counter of input serial line interrupts (DCD,RI,DSR,CTS)
		 * Return: write counters to the user passed counter struct
		 * NB: both 1->0 and 0->1 transitions are counted except for
		 *     RI where only 0->1 is counted.
		 */
		case TIOCGICOUNT:
			save_flags(flags); cli();
			cnow = info->state->icount;
			restore_flags(flags);
			p_cuser = (struct serial_icounter_struct *) arg;
			error = put_user(cnow.cts, &p_cuser->cts);
			if (error) return error;
			error = put_user(cnow.dsr, &p_cuser->dsr);
			if (error) return error;
			error = put_user(cnow.rng, &p_cuser->rng);
			if (error) return error;
			error = put_user(cnow.dcd, &p_cuser->dcd);
			if (error) return error;
			error = put_user(cnow.rx, &p_cuser->rx);
			if (error) return error;
			error = put_user(cnow.tx, &p_cuser->tx);
			if (error) return error;
			error = put_user(cnow.frame, &p_cuser->frame);
			if (error) return error;
			error = put_user(cnow.overrun, &p_cuser->overrun);
			if (error) return error;
			error = put_user(cnow.parity, &p_cuser->parity);
			if (error) return error;
			error = put_user(cnow.brk, &p_cuser->brk);
			if (error) return error;
			error = put_user(cnow.buf_overrun, &p_cuser->buf_overrun);
			if (error) return error;
			return 0;

		case TIOCSERGWILD:
		case TIOCSERSWILD:
			/* "setserial -W" is called in Debian boot */
			printk ("TIOCSER?WILD ioctl obsolete, ignored.\n");
			return 0;

		default:
			return -ENOIOCTLCMD;
		}
	return 0;
}

static void rs_set_termios(struct tty_struct *tty, struct termios *old_termios)
{
	struct async_struct *info = (struct async_struct *)tty->driver_data;
	unsigned long flags;

	if (   (tty->termios->c_cflag == old_termios->c_cflag)
	    && (   RELEVANT_IFLAG(tty->termios->c_iflag) 
		== RELEVANT_IFLAG(old_termios->c_iflag)))
	  return;

	change_speed(info);

	/* Handle transition to B0 status */
	if ((old_termios->c_cflag & CBAUD) &&
	    !(tty->termios->c_cflag & CBAUD)) {
#if 0  /* no serial control lines */
		info->MCR &= ~(UART_MCR_DTR|UART_MCR_RTS);
#endif
		save_flags(flags); cli();
#if 0
		serial_out(info, UART_MCR, info->MCR);
#endif
		restore_flags(flags);
	}
	
	/* Handle transition away from B0 status */
	if (!(old_termios->c_cflag & CBAUD) &&
	    (tty->termios->c_cflag & CBAUD)) {
#if 0
		info->MCR |= UART_MCR_DTR;
		if (!(tty->termios->c_cflag & CRTSCTS) ||
		    !test_bit(TTY_THROTTLED, &tty->flags)) {
			info->MCR |= UART_MCR_RTS;
		}
#endif
		save_flags(flags); cli();
#if 0
		serial_out(info, UART_MCR, info->MCR);
#endif
		restore_flags(flags);
	}
	
	/* Handle turning off CRTSCTS */
	if ((old_termios->c_cflag & CRTSCTS) &&
	    !(tty->termios->c_cflag & CRTSCTS)) {
		tty->hw_stopped = 0;
		rs_start(tty);
	}

#if 0
	/*
	 * No need to wake up processes in open wait, since they
	 * sample the CLOCAL flag once, and don't recheck it.
	 * XXX  It's not clear whether the current behavior is correct
	 * or not.  Hence, this may change.....
	 */
	if (!(old_termios->c_cflag & CLOCAL) &&
	    (tty->termios->c_cflag & CLOCAL))
		wake_up_interruptible(&info->open_wait);
#endif
}

/*
 * ------------------------------------------------------------
 * rs_close()
 * 
 * This routine is called when the serial port gets closed.  First, we
 * wait for the last remaining data to be sent.  Then, we unlink its
 * async structure from the interrupt chain if necessary, and we free
 * that IRQ if nothing is left in the chain.
 * ------------------------------------------------------------
 */
static void rs_close(struct tty_struct *tty, struct file * filp)
{
	struct async_struct * info = (struct async_struct *)tty->driver_data;
	struct serial_state *state;
	unsigned long flags;

	if (!info || serial_paranoia_check(info, tty->device, "rs_close"))
		return;

	state = info->state;
	
	save_flags(flags); cli();
	
	if (tty_hung_up_p(filp)) {
		DBG_CNT("before DEC-hung");
		MOD_DEC_USE_COUNT;
		restore_flags(flags);
		return;
	}
	
#ifdef SERIAL_DEBUG_OPEN
	printk("rs_close ttys%d, count = %d\n", info->line, state->count);
#endif
	if ((tty->count == 1) && (state->count != 1)) {
		/*
		 * Uh, oh.  tty->count is 1, which means that the tty
		 * structure will be freed.  state->count should always
		 * be one in these conditions.  If it's greater than
		 * one, we've got real problems, since it means the
		 * serial port won't be shutdown.
		 */
		printk("rs_close: bad serial port count; tty->count is 1, "
		       "state->count is %d\n", state->count);
		state->count = 1;
	}
	if (--state->count < 0) {
		printk("rs_close: bad serial port count for ttys%d: %d\n",
		       info->line, state->count);
		state->count = 0;
	}
	if (state->count) {
		DBG_CNT("before DEC-2");
		MOD_DEC_USE_COUNT;
		restore_flags(flags);
		return;
	}
	info->flags |= ASYNC_CLOSING;
	/*
	 * Save the termios structure, since this port may have
	 * separate termios for callout and dialin.
	 */
	if (info->flags & ASYNC_NORMAL_ACTIVE)
		info->state->normal_termios = *tty->termios;
	if (info->flags & ASYNC_CALLOUT_ACTIVE)
		info->state->callout_termios = *tty->termios;
	/*
	 * Now we wait for the transmit buffer to clear; and we notify 
	 * the line discipline to only process XON/XOFF characters.
	 */
	tty->closing = 1;
	if (info->closing_wait != ASYNC_CLOSING_WAIT_NONE)
		tty_wait_until_sent(tty, info->closing_wait);
	/*
	 * At this point we stop accepting input.  To do this, we
	 * disable the receive line status interrupts, and tell the
	 * interrupt driver to stop checking the data ready bit in the
	 * line status register.
	 */
	info->IER &= ~UTCR3_RIE;
	_V0(info->read_status_mask) &= ~UTSR0_RFS;
	_V1(info->read_status_mask) &= ~UTSR1_RNE;
	if (info->flags & ASYNC_INITIALIZED) {
		serial_out(info, UTCR3, info->IER);
		/*
		 * Before we drop DTR, make sure the UART transmitter
		 * has completely drained; this is especially
		 * important if there is a transmit FIFO!
		 */
		rs_wait_until_sent(tty, info->timeout);
	}
	shutdown(info);
	if (tty->driver.flush_buffer)
		tty->driver.flush_buffer(tty);
	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);
	tty->closing = 0;
	info->event = 0;
	info->tty = 0;
	if (info->blocked_open) {
		if (info->close_delay) {
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(info->close_delay);
		}
		wake_up_interruptible(&info->open_wait);
	}
	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE|
			 ASYNC_CLOSING);
	wake_up_interruptible(&info->close_wait);
	MOD_DEC_USE_COUNT;
	restore_flags(flags);
}

/*
 * rs_wait_until_sent() --- wait until the transmitter is empty
 */
static void rs_wait_until_sent(struct tty_struct *tty, int timeout)
{
	struct async_struct * info = (struct async_struct *)tty->driver_data;
	unsigned long orig_jiffies, char_time;
	int lsr;
	
	if (serial_paranoia_check(info, tty->device, "rs_wait_until_sent"))
		return;

	if (info->state->type == PORT_UNKNOWN)
		return;

	if (info->xmit_fifo_size == 0)
		return; /* Just in case.... */

	orig_jiffies = jiffies;
	/*
	 * Set the check interval to be 1/5 of the estimated time to
	 * send a single character, and make it at least 1.  The check
	 * interval should also be less than the timeout.
	 * 
	 * Note: we have to use pretty tight timings here to satisfy
	 * the NIST-PCTS.
	 */
	char_time = (info->timeout - HZ/50) / info->xmit_fifo_size;
	char_time = char_time / 5;
	if (char_time == 0)
		char_time = 1;
	if (timeout)
	  char_time = MIN(char_time, timeout);
#ifdef SERIAL_DEBUG_RS_WAIT_UNTIL_SENT
	printk("In rs_wait_until_sent(%d) check=%lu...", timeout, char_time);
	printk("jiff=%lu...", jiffies);
#endif
	while (!((lsr = serial_inp(info, UTSR1)) & UTSR1_TBY)) {
#ifdef SERIAL_DEBUG_RS_WAIT_UNTIL_SENT
		printk("lsr = %d (jiff=%lu)...", lsr, jiffies);
#endif
		current->state = TASK_INTERRUPTIBLE;
		current->counter = 0;	/* make us low-priority */
		schedule_timeout(char_time);
		if (signal_pending(current))
			break;
		if (timeout && ((orig_jiffies + timeout) < jiffies))
			break;
	}
	current->state = TASK_RUNNING;
#ifdef SERIAL_DEBUG_RS_WAIT_UNTIL_SENT
	printk("lsr = %d (jiff=%lu)...done\n", lsr, jiffies);
#endif
}

/*
 * rs_hangup() --- called by tty_hangup() when a hangup is signaled.
 */
static void rs_hangup(struct tty_struct *tty)
{
	struct async_struct * info = (struct async_struct *)tty->driver_data;
	struct serial_state *state = info->state;
	
	if (serial_paranoia_check(info, tty->device, "rs_hangup"))
		return;

	state = info->state;
	
	rs_flush_buffer(tty);
	shutdown(info);
	info->event = 0;
	state->count = 0;
	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE);
	info->tty = 0;
	wake_up_interruptible(&info->open_wait);
}

/*
 * ------------------------------------------------------------
 * rs_open() and friends
 * ------------------------------------------------------------
 */
static int block_til_ready(struct tty_struct *tty, struct file * filp,
			   struct async_struct *info)
{
	struct serial_state *state = info->state;
	int		retval=0;
	int		do_clocal = 0;
#if 0
	struct wait_queue wait = { current, NULL };
	int             extra_count = 0;
	unsigned long	flags;
#endif

	/*
	 * If the device is in the middle of being closed, then block
	 * until it's done, and then try again.
	 */
	if (tty_hung_up_p(filp) ||
	    (info->flags & ASYNC_CLOSING)) {
		if (info->flags & ASYNC_CLOSING)
			interruptible_sleep_on(&info->close_wait);
#ifdef SERIAL_DO_RESTART
		return ((info->flags & ASYNC_HUP_NOTIFY) ?
			-EAGAIN : -ERESTARTSYS);
#else
		return -EAGAIN;
#endif
	}

	/*
	 * If this is a callout device, then just make sure the normal
	 * device isn't being used.
	 */
	if (tty->driver.subtype == SERIAL_TYPE_CALLOUT) {
		if (info->flags & ASYNC_NORMAL_ACTIVE)
			return -EBUSY;
		if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (info->flags & ASYNC_SESSION_LOCKOUT) &&
		    (info->session != current->session))
		    return -EBUSY;
		if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (info->flags & ASYNC_PGRP_LOCKOUT) &&
		    (info->pgrp != current->pgrp))
		    return -EBUSY;
		info->flags |= ASYNC_CALLOUT_ACTIVE;
		return 0;
	}
	
	/*
	 * If non-blocking mode is set, or the port is not enabled,
	 * then make the check up front and then exit.
	 */
	if ((filp->f_flags & O_NONBLOCK) ||
	    (tty->flags & (1 << TTY_IO_ERROR))) {
		if (info->flags & ASYNC_CALLOUT_ACTIVE)
			return -EBUSY;
		info->flags |= ASYNC_NORMAL_ACTIVE;
		return 0;
	}

	if (info->flags & ASYNC_CALLOUT_ACTIVE) {
		if (state->normal_termios.c_cflag & CLOCAL)
			do_clocal = 1;
	} else {
		if (tty->termios->c_cflag & CLOCAL)
			do_clocal = 1;
	}

#if 0 /* We have no DCD */	
	/*
	 * Block waiting for the carrier detect and the line to become
	 * free (i.e., not in use by the callout).  While we are in
	 * this loop, state->count is dropped by one, so that
	 * rs_close() knows when to free things.  We restore it upon
	 * exit, either normal or abnormal.
	 */
	retval = 0;
	add_wait_queue(&info->open_wait, &wait);
#ifdef SERIAL_DEBUG_OPEN
	printk("block_til_ready before block: ttys%d, count = %d\n",
	       state->line, state->count);
#endif
	save_flags(flags); cli();
	if (!tty_hung_up_p(filp)) {
		extra_count = 1;
		state->count--;
	}
	restore_flags(flags);
	info->blocked_open++;
	while (1) {
		save_flags(flags); cli();
		if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (tty->termios->c_cflag & CBAUD))
			serial_out(info, UART_MCR,
				   serial_inp(info, UART_MCR) |
				   (UART_MCR_DTR | UART_MCR_RTS));
		restore_flags(flags);
		current->state = TASK_INTERRUPTIBLE;
		if (tty_hung_up_p(filp) ||
		    !(info->flags & ASYNC_INITIALIZED)) {
#ifdef SERIAL_DO_RESTART
			if (info->flags & ASYNC_HUP_NOTIFY)
				retval = -EAGAIN;
			else
				retval = -ERESTARTSYS;	
#else
			retval = -EAGAIN;
#endif
			break;
		}
		if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    !(info->flags & ASYNC_CLOSING) &&
		    (do_clocal || (serial_in(info, UART_MSR) &
				   UART_MSR_DCD)))
			break;
		if (signal_pending(current)) {
			retval = -ERESTARTSYS;
			break;
		}
#ifdef SERIAL_DEBUG_OPEN
		printk("block_til_ready blocking: ttys%d, count = %d\n",
		       info->line, state->count);
#endif
		schedule();
	}
	current->state = TASK_RUNNING;
	remove_wait_queue(&info->open_wait, &wait);
	if (extra_count)
		state->count++;
	info->blocked_open--;
#ifdef SERIAL_DEBUG_OPEN
	printk("block_til_ready after blocking: ttys%d, count = %d\n",
	       info->line, state->count);
#endif

#endif /* DCD stuff we if 0'ed */

	if (retval)
		return retval;
	info->flags |= ASYNC_NORMAL_ACTIVE;
	return 0;
}

static int get_async_struct(int line, struct async_struct **ret_info)
{
	struct async_struct *info;
	struct serial_state *sstate;

	sstate = rs_table + line;
	sstate->count++;
	if (sstate->info) {
		*ret_info = sstate->info;
		return 0;
	}
	info = kmalloc(sizeof(struct async_struct), GFP_KERNEL);
	if (!info) {
		sstate->count--;
		return -ENOMEM;
	}
	memset(info, 0, sizeof(struct async_struct));
	info->magic = SERIAL_MAGIC;
	info->port = sstate->port;
	info->flags = sstate->flags;
	info->xmit_fifo_size = sstate->xmit_fifo_size;
	info->line = line;
	info->tqueue.routine = do_softint;
	info->tqueue.data = info;
	info->state = sstate;
	if (sstate->info) {
		kfree_s(info, sizeof(struct async_struct));
		*ret_info = sstate->info;
		return 0;
	}
	*ret_info = sstate->info = info;
	return 0;
}

/*
 * This routine is called whenever a serial port is opened.  It
 * enables interrupts for a serial port, linking in its async structure into
 * the IRQ chain.   It also performs the serial-specific
 * initialization for the tty structure.
 */
static int rs_open(struct tty_struct *tty, struct file * filp)
{
	struct async_struct	*info;
	int 			retval, line;
	unsigned long		page;

	MOD_INC_USE_COUNT;
	line = MINOR(tty->device) - tty->driver.minor_start;
	if ((line < 0) || (line >= NR_PORTS))
		return -ENODEV;
	retval = get_async_struct(line, &info);
	if (retval)
		return retval;
	tty->driver_data = info;
	info->tty = tty;
	if (serial_paranoia_check(info, tty->device, "rs_open"))
		return -ENODEV;

#ifdef SERIAL_DEBUG_OPEN
	printk("rs_open %s%d, count = %d\n", tty->driver.name, info->line,
	       info->state->count);
#endif
	info->tty->low_latency = (info->flags & ASYNC_LOW_LATENCY) ? 1 : 0;

	if (!tmp_buf) {
		page = get_free_page(GFP_KERNEL);
		if (!page)
			return -ENOMEM;
		if (tmp_buf)
			free_page(page);
		else
			tmp_buf = (unsigned char *) page;
	}

	/*
	 * If the port is the middle of closing, bail out now
	 */
	if (tty_hung_up_p(filp) ||
	    (info->flags & ASYNC_CLOSING)) {
		if (info->flags & ASYNC_CLOSING)
			interruptible_sleep_on(&info->close_wait);
#ifdef SERIAL_DO_RESTART
		return ((info->flags & ASYNC_HUP_NOTIFY) ?
			-EAGAIN : -ERESTARTSYS);
#else
		return -EAGAIN;
#endif
	}

	/*
	 * Start up serial port
	 */
	retval = startup(info);
	if (retval)
		return retval;

	retval = block_til_ready(tty, filp, info);
	if (retval) {
#ifdef SERIAL_DEBUG_OPEN
		printk("rs_open returning after block_til_ready with %d\n",
		       retval);
#endif
		return retval;
	}

	if ((info->state->count == 1) &&
	    (info->flags & ASYNC_SPLIT_TERMIOS)) {
		if (tty->driver.subtype == SERIAL_TYPE_NORMAL)
			*tty->termios = info->state->normal_termios;
		else 
			*tty->termios = info->state->callout_termios;
		change_speed(info);
	}
#ifdef CONFIG_SERIAL_SA1100_CONSOLE
	if (sercons.cflag && sercons.index == line) {
		tty->termios->c_cflag = sercons.cflag;
		sercons.cflag = 0;
		change_speed(info);
	}
#endif
	info->session = current->session;
	info->pgrp = current->pgrp;

#ifdef SERIAL_DEBUG_OPEN
	printk("rs_open ttys%d successful...", info->line);
#endif
	return 0;
}

/*
 * /proc fs routines....
 */

static inline int line_info(char *buf, struct serial_state *state)
{
	struct async_struct *info = state->info, scr_info;
	char	stat_buf[30];
	int	ret;
#if 0
        char    control, status;
	unsigned long flags;
#endif

	ret = sprintf(buf, "%d: uart:%s port:%X irq:%d",
		      state->line, uart_config[state->type].name, 
		      state->port, state->irq);

	if (!state->port || (state->type == PORT_UNKNOWN)) {
		ret += sprintf(buf+ret, "\n");
		return ret;
	}

	/*
	 * Figure out the current RS-232 lines
	 */
	if (!info) {
		info = &scr_info;	/* This is just for serial_{in,out} */

		info->magic = SERIAL_MAGIC;
		info->port = state->port;
		info->flags = state->flags;
		info->quot = 0;
		info->tty = 0;
	}
#if 0
	save_flags(flags); cli();
	status = serial_in(info, UART_MSR);
	control = info ? info->MCR : serial_in(info, UART_MCR);
	restore_flags(flags);
	
	stat_buf[0] = 0;
	stat_buf[1] = 0;
	if (control & UART_MCR_RTS)
		strcat(stat_buf, "|RTS");
	if (status & UART_MSR_CTS)
		strcat(stat_buf, "|CTS");
	if (control & UART_MCR_DTR)
		strcat(stat_buf, "|DTR");
	if (status & UART_MSR_DSR)
		strcat(stat_buf, "|DSR");
	if (status & UART_MSR_DCD)
		strcat(stat_buf, "|CD");
	if (status & UART_MSR_RI)
		strcat(stat_buf, "|RI");
#else
	stat_buf[0] = 0;
	stat_buf[1] = 0;
#endif

	if (info->quot) {
		ret += sprintf(buf+ret, " baud:%d",
			       state->baud_base / info->quot);
	}

	ret += sprintf(buf+ret, " tx:%d rx:%d",
		      state->icount.tx, state->icount.rx);

	if (state->icount.frame)
		ret += sprintf(buf+ret, " fe:%d", state->icount.frame);
	
	if (state->icount.parity)
		ret += sprintf(buf+ret, " pe:%d", state->icount.parity);
	
	if (state->icount.brk)
		ret += sprintf(buf+ret, " brk:%d", state->icount.brk);	

	if (state->icount.overrun)
		ret += sprintf(buf+ret, " oe:%d", state->icount.overrun);

	/*
	 * Last thing is the RS-232 status lines
	 */
	ret += sprintf(buf+ret, " %s\n", stat_buf+1);
	return ret;
}

int rs_read_proc(char *page, char **start, off_t off, int count,
		 int *eof, void *data)
{
	int i, len = 0, l;
	off_t	begin = 0;

	len += sprintf(page, "serinfo:1.0 driver:%s\n", serial_version);
	for (i = 0; i < NR_PORTS && len < 4000; i++) {
		l = line_info(page + len, &rs_table[i]);
		len += l;
		if (len+begin > off+count)
			goto done;
		if (len+begin < off) {
			begin += len;
			len = 0;
		}
	}
	*eof = 1;
done:
	if (off >= len+begin)
		return 0;
	*start = page + (begin-off);
	return ((count < begin+len-off) ? count : begin+len-off);
}

/*
 * ---------------------------------------------------------------------
 * sa1100_rs_init() and friends
 *
 * sa1100_rs_init() is called at boot-time to initialize the serial driver.
 * ---------------------------------------------------------------------
 */

/*
 * This routine prints out the appropriate serial driver version
 * number, and identifies which options were configured into this
 * driver.
 */
static _INLINE_ void show_serial_version(void)
{
 	printk(KERN_INFO "%s version %s with", serial_name, serial_version);
	printk(" no serial options enabled\n");
}

/*
 * This routine is called by sa1100_rs_init() to initialize a specific serial
 * port.  It determines what type of UART chip this serial port is
 * using: 8250, 16450, 16550, 16550A.  The important question is
 * whether or not this UART is a 16550A or not, since this will
 * determine whether or not we can use its FIFO features or not.
 */
static void autoconfig(struct serial_state * state)
{
	struct async_struct *info, scr_info;
	unsigned long flags;

	state->type = PORT_UNKNOWN;
	
	if (!state->port)
		return;
		
	info = &scr_info;	/* This is just for serial_{in,out} */

	info->magic = SERIAL_MAGIC;
	info->port = state->port;
	info->flags = state->flags;

	save_flags(flags); cli();

	/* Assume SA1100 uart - pretty safe as this is the SA1100 uart driver... */
	state->type=PORT_SA1100_UART;
	
	switch(info->port) {
	case (int)&Ser3UTCR0:
		/* uart serial port 3 on sa1100 */
#ifdef CONFIG_SA1100_ITSY
		GPSR = 0x01800000;   /* bits 23 and 24 = 1 */
#endif
		break;
	case (int)&Ser1UTCR0:
		/* uart serial port 1 on sa1100 */
#ifdef CONFIG_SA1100_BRUTUS
		/* set up for using alternate UART */
		GAFR |= (GPIO_GPIO14 | GPIO_GPIO15);
		GPDR |= GPIO_GPIO14;
		GPDR &= ~GPIO_GPIO15;
		PPAR |= PPAR_UPR;
#elif defined( CONFIG_SA1100_TIFON )
		GAFR |= (GPIO_GPIO14 | GPIO_GPIO15);
		GPDR |= GPIO_GPIO14;
		GPDR &= ~GPIO_GPIO15;
		PPAR |= PPAR_UPR;
#else
		Ser1SDCR0 |= SDCR0_UART;
#endif
		break;
#ifdef CONFIG_SA1100_EMPEG
	case (int)&Ser2UTCR0:
		/* Check hardware revision */
		if (empeg_hardwarerevision()>=5) {
			/* flateric & later use external HP endec: program up
			   outputs */
			GPDR|=(EMPEG_SIRSPEED0|EMPEG_SIRSPEED1|EMPEG_SIRSPEED2);

			/* setting the speed will program up the endec
			   correctly */
		} else {
			/* uart serial port 2 on sa1100: HP-SIR */
			Ser2HSCR0=0;
			Ser2HSSR0=0x27; /* Clear bits */
			Ser2UTCR4=UTCR4_HPSIR;
		}
	        break;
#endif
	    default:
		restore_flags(flags);
		return;
	}
	if (state->type == PORT_UNKNOWN) {
		restore_flags(flags);
		return;
	}

	request_region(info->port,32,"serial(auto)");

	/*
	 * Reset the UART.
	 */
	//serial_out(info, UTCR3, 0); not at the moment!
	
	restore_flags(flags);
}

/*
 * The serial driver boot-time initialization code!
 */
__initfunc(int sa1100_rs_init(void))
{
	int i;
	struct serial_state * state;

	init_bh(SERIAL_BH, do_serial_bh);
	timer_table[RS_TIMER].fn = rs_timer;
	timer_table[RS_TIMER].expires = 0;

	for (i = 0; i < NR_IRQS; i++) {
		IRQ_ports[i] = 0;
		IRQ_timeout[i] = 0;
	}
#ifdef CONFIG_SERIAL_SA1100_CONSOLE
	/*
	 *	The interrupt of the serial console port
	 *	can't be shared.
	 */
	if (sercons.flags & CON_CONSDEV) {
		for(i = 0; i < NR_PORTS; i++)
			if (i != sercons.index &&
			    rs_table[i].irq == rs_table[sercons.index].irq)
				rs_table[i].irq = 0;
	}
#endif
	show_serial_version();

	/* Initialize the tty_driver structure */
	memset(&serial_driver, 0, sizeof(struct tty_driver));
	serial_driver.magic = TTY_DRIVER_MAGIC;
	serial_driver.driver_name = "serial";
	serial_driver.name = "ttyS";
	serial_driver.major = TTY_MAJOR;
	serial_driver.minor_start = 64;
	serial_driver.num = NR_PORTS;
	serial_driver.type = TTY_DRIVER_TYPE_SERIAL;
	serial_driver.subtype = SERIAL_TYPE_NORMAL;
	serial_driver.init_termios = tty_std_termios;
	serial_driver.init_termios.c_cflag =
#if defined( CONFIG_SA1100_EMPEG ) || defined( CONFIG_SA1100_VICTOR ) || defined( CONFIG_SA1100_CITYGO )
	/* Life in the fast lane */
		B115200
#else
	/* A little pedestrian... */
		B9600
#endif
		| CS8 | CREAD | HUPCL | CLOCAL;
	serial_driver.flags = TTY_DRIVER_REAL_RAW;
	serial_driver.refcount = &serial_refcount;
	serial_driver.table = serial_table;
	serial_driver.termios = serial_termios;
	serial_driver.termios_locked = serial_termios_locked;

	serial_driver.open = rs_open;
	serial_driver.close = rs_close;
	serial_driver.write = rs_write;
	serial_driver.put_char = rs_put_char;
	serial_driver.flush_chars = rs_flush_chars;
	serial_driver.write_room = rs_write_room;
	serial_driver.chars_in_buffer = rs_chars_in_buffer;
	serial_driver.flush_buffer = rs_flush_buffer;
	serial_driver.ioctl = rs_ioctl;
	serial_driver.throttle = rs_throttle;
	serial_driver.unthrottle = rs_unthrottle;
	serial_driver.send_xchar = rs_send_xchar;
	serial_driver.set_termios = rs_set_termios;
	serial_driver.stop = rs_stop;
	serial_driver.start = rs_start;
	serial_driver.hangup = rs_hangup;
	serial_driver.break_ctl = rs_break;
	serial_driver.wait_until_sent = rs_wait_until_sent;
	serial_driver.read_proc = rs_read_proc;
	
	/*
	 * The callout device is just like normal device except for
	 * major number and the subtype code.
	 */
	callout_driver = serial_driver;
	callout_driver.name = "cua";
	callout_driver.major = TTYAUX_MAJOR;
	callout_driver.subtype = SERIAL_TYPE_CALLOUT;
	callout_driver.read_proc = 0;
	callout_driver.proc_entry = 0;

	if (tty_register_driver(&serial_driver))
		panic("Couldn't register serial driver\n");
	if (tty_register_driver(&callout_driver))
		panic("Couldn't register callout driver\n");
	
	for (i = 0, state = rs_table; i < NR_PORTS; i++,state++) {
		state->magic = SSTATE_MAGIC;
		state->line = i;
		state->type = PORT_UNKNOWN;
		state->custom_divisor = 0;
		state->close_delay = 5*HZ/10;
		state->closing_wait = 30*HZ;
		state->callout_termios = callout_driver.init_termios;
		state->normal_termios = serial_driver.init_termios;
		state->icount.cts = state->icount.dsr = 
			state->icount.rng = state->icount.dcd = 0;
		state->icount.rx = state->icount.tx = 0;
		state->icount.frame = state->icount.parity = 0;
		state->icount.overrun = state->icount.brk = 0;
		state->irq = irq_cannonicalize(state->irq);
		if (check_region(state->port,8))
			continue;
		if (state->flags & ASYNC_BOOT_AUTOCONF)
			autoconfig(state);
	}
	/*
	 * Detect the IRQ only once every port is initialised,
	 * because some 16450 do not reset to 0 the MCR register.
	 */
	for (i = 0, state = rs_table; i < NR_PORTS; i++,state++) {
		if (state->type == PORT_UNKNOWN)
			continue;
		printk(KERN_INFO "ttyS%02d%s at 0x%04x (irq = %d) is a %s\n",
		       state->line,
		       (state->flags & ASYNC_FOURPORT) ? " FourPort" : "",
		       state->port, state->irq,
		       uart_config[state->type].name);
	}
	return 0;
}

#ifdef MODULE
int init_module(void)
{
	return sa1100_rs_init();
}

void cleanup_module(void) 
{
	unsigned long flags;
	int e1, e2;
	int i;

	/* printk("Unloading %s: version %s\n", serial_name, serial_version); */
	save_flags(flags);
	cli();
	timer_active &= ~(1 << RS_TIMER);
	timer_table[RS_TIMER].fn = NULL;
	timer_table[RS_TIMER].expires = 0;
        remove_bh(SERIAL_BH);
	if ((e1 = tty_unregister_driver(&serial_driver)))
		printk("SERIAL: failed to unregister serial driver (%d)\n",
		       e1);
	if ((e2 = tty_unregister_driver(&callout_driver)))
		printk("SERIAL: failed to unregister callout driver (%d)\n", 
		       e2);
	restore_flags(flags);

	for (i = 0; i < NR_PORTS; i++) {
		if (rs_table[i].type != PORT_UNKNOWN)
			release_region(rs_table[i].port, 8);
	}
	if (tmp_buf) {
		free_page((unsigned long) tmp_buf);
		tmp_buf = NULL;
	}
}
#endif /* MODULE */


/*
 * ------------------------------------------------------------
 * Serial console driver
 * ------------------------------------------------------------
 */
#ifdef CONFIG_SERIAL_SA1100_CONSOLE

static volatile unsigned long *serial_echo_port = 0;

/*
 *	Print a string to the serial port trying not to disturb
 *	any possible real use of the port...
 */
static void serial_console_write(struct console *co, const char *s,
				unsigned count)
{
	int ier;
	unsigned i;

	/*
	 *	First save the IER then disable the interrupts
	 */
	ier = serial_echo_port[UTCR3];
	serial_echo_port[UTCR3]=UTCR3_TXE;

	/*
	 *	Now, do each character
	 */
	for (i = 0; i < count; i++, s++) {
		while( !(serial_echo_port[UTSR1] & UTSR1_TNF) );

		/*
		 *	Send the character out.
		 *	If a LF, also do CR...
		 */
		serial_echo_port[UART_TX] = *s;

		if (*s == '\n') {
			while( !(serial_echo_port[UTSR1] & UTSR1_TNF) );
			serial_echo_port[UART_TX] = '\r';
		}
	}

	/*
	 *	Finally, Wait for transmitter & holding register to empty
	 * 	and restore the IER
	 */
	while((serial_echo_port[UTSR1]&UTSR1_TBY));

	serial_echo_port[UTCR3] = ier;
}

/*
 *	Receive character from the serial port
 */
static int serial_console_wait_key(struct console *co)
{
	int ier;
	int c;

	/*
	 *	First save the IER then disable the interrupts so
	 *	that the real driver for the port does not get the
	 *	character.
	 */
	ier = serial_echo_port[UTCR3];
	serial_echo_port[UTCR3] = UTCR3_RXE;

	/* wait for a character to come in */
	while( !(serial_echo_port[UTSR1] & UTSR1_RNE) );

	c = serial_echo_port[UART_RX];

	/*
	 *	Restore the interrupts
	 */
	serial_echo_port[UTCR3] = ier;

	return c;
}

static kdev_t serial_console_device(struct console *c)
{
	return MKDEV(TTY_MAJOR, 64 + c->index);
}

/*
 *	Setup initial baud/bits/parity. We do two things here:
 *	- construct a cflag setting for the first rs_open()
 *	- initialize the serial port
 *	Return non-zero if we didn't find a serial port.
 */
__initfunc(static int serial_console_setup(struct console *co, char *options))
{

#if defined( CONFIG_SA1100_ARNOLD )
#warning("compiling for Arnold");
    serial_echo_port= (unsigned long *)&Ser3UTCR0;
    serial_echo_port[UTCR3] = 0;	/* Disable all interrupts for now */
    serial_echo_port[UTCR0] = 0x08;	/* No parity, 8 bits, 1 stop */
    serial_echo_port[UTCR1] = 0;	/* 38400 baud */
    serial_echo_port[UTCR2] = 0x5;
    serial_echo_port[UTCR3] = UTCR3_TXE;  /* Turn on transmitter */
#elif defined( CONFIG_SA1100_TIFON )
    serial_echo_port= (unsigned long *)&Ser3UTCR0;
    serial_echo_port[UTCR3] = 0;	/* Disable all interrupts for now */
    serial_echo_port[UTCR0] = 0x08;	/* No parity, 8 bits, 1 stop */
    serial_echo_port[UTCR1] = 0;	/* 9600 baud */
    serial_echo_port[UTCR2] = 0x17;
    serial_echo_port[UTCR3] = UTCR3_TXE;  /* Turn on transmitter */
#elif defined( CONFIG_SA1100_BRUTUS )
    serial_echo_port = (unsigned long *)&Ser1UTCR0;
    /* set up for using alternate UART */
    GAFR |= (GPIO_GPIO14 | GPIO_GPIO15);
    GPDR |= GPIO_GPIO14;
    GPDR &= ~GPIO_GPIO15;
    PPAR |= PPAR_UPR;

    serial_echo_port[UTCR3] = 0;	/* Disable all interrupts for now */
    serial_echo_port[UTCR0] = 0x08;	/* No parity, 8 bits, 1 stop */
    serial_echo_port[UTCR1] = 0;	/* 9600 baud */
    serial_echo_port[UTCR2] = 0x17;
    serial_echo_port[UTCR3] = UTCR3_TXE;  /* Turn on transmitter */
#elif defined( CONFIG_SA1100_VICTOR ) || defined( CONFIG_SA1100_CITYGO )
    serial_echo_port= (unsigned long *)&Ser3UTCR0;
    serial_echo_port[UTCR3] = 0;	/* Disable all interrupts for now */
    serial_echo_port[UTCR0] = 0x08;	/* No parity, 8 bits, 1 stop */
    serial_echo_port[UTCR1] = 0;	/* 115200 baud */
    serial_echo_port[UTCR2] = 0x1;
    serial_echo_port[UTCR3] = UTCR3_TXE;  /* Turn on transmitter */
#elif defined( CONFIG_SA1100_THINCLIENT )
    serial_echo_port= (unsigned long *)&Ser3UTCR0;
    serial_echo_port[UTCR3] = 0;	/* Disable all interrupts for now */
    serial_echo_port[UTCR0] = 0x08;	/* No parity, 8 bits, 1 stop */
    serial_echo_port[UTCR1] = 0;	/* 9600 baud */
    serial_echo_port[UTCR2] = 0x17;
    serial_echo_port[UTCR3] = UTCR3_TXE;  /* Turn on transmitter */
#else
    serial_echo_port= (unsigned long *)&Ser3UTCR0;
    /* We're assuming the linux booter has set this bit up... */
#endif

    /* Clear errors */
    serial_echo_port[UTSR0] = 0xff;

#if defined(CONFIG_ARCH_EMPEG) && defined(CONFIG_EMPEG_SER2IRDA)
    /* It should be noted here that SiR doesn't behave 100% on pre-revG StrongARMs
       due to a silicon problem (worked with my Nokia 9000 & Series 5, but not with
       my PC) */
    {
	    volatile unsigned long *ir=(unsigned long*)&Ser2UTCR0;
	    int a;
	    
	    /* uart serial port 2 on sa1100: HP-SIR */
	    Ser2HSCR0=0;
	    Ser2HSSR0=0x27; /* Clear bits */
	    Ser2UTCR4=UTCR4_HPSIR;
	    ir[UTCR0] = 0x08;	/* No parity, 8 bits, 1 stop */
	    ir[UTCR1] = 0;	/* 115k2 */
	    ir[UTCR2] = 1;
	    ir[UTSR0] = 0xff;  /* Clear sticky flags */
	    ir[UTCR3] = 0;	/* Disable all interrupts for now */
	    ir[UTCR3] = UTCR3_TXE|UTCR3_RXE;  /* Turn on transmitter */
	    
	    /* For no aparrent reason, this seemed to help IrDA to get out of bed */
	    for(a=32;a<127;a++) {
		    while( !(ir[UTSR1] & UTSR1_TNF) );
		    ir[UART_TX]=a;
		    serial_echo_port[UART_TX]=a;
	    }
    }
#endif

#if 0
	struct serial_state *ser;
	unsigned cval;
	int	baud = 9600;
	int	bits = 8;
	int	parity = 'n';
	int	cflag = CREAD | HUPCL | CLOCAL;
	int	quot = 0;
	char	*s;

	if (options) {
		baud = simple_strtoul(options, NULL, 10);
		s = options;
		while(*s >= '0' && *s <= '9')
			s++;
		if (*s) parity = *s++;
		if (*s) bits   = *s - '0';
	}

	/*
	 *	Now construct a cflag setting.
	 */
	switch(baud) {
		case 1200:
			cflag |= B1200;
			break;
		case 2400:
			cflag |= B2400;
			break;
		case 4800:
			cflag |= B4800;
			break;
		case 19200:
			cflag |= B19200;
			break;
		case 38400:
			cflag |= B38400;
			break;
		case 57600:
			cflag |= B57600;
			break;
		case 115200:
			cflag |= B115200;
			break;
		case 230400:
			cflag |= B230400;
			break;
		case 9600:
		default:
			cflag |= B9600;
			break;
	}
	switch(bits) {
		case 7:
			cflag |= CS7;
			break;
		default:
		case 8:
			cflag |= CS8;
			break;
	}
	switch(parity) {
		case 'o': case 'O':
			cflag |= PARODD;
			break;
		case 'e': case 'E':
			cflag |= PARENB;
			break;
	}
	co->cflag = cflag;

	/*
	 *	Divisor, bytesize and parity
	 */
	ser = rs_table + co->index;
	quot = ser->baud_base / baud;
	cval = cflag & (CSIZE | CSTOPB);
	cval >>= 4;
	if (cflag & PARENB)
		cval |= UART_LCR_PARITY;
	if (!(cflag & PARODD))
		cval |= UART_LCR_EPAR;

	/*
	 *	Disable UART interrupts, set DTR and RTS high
	 *	and set speed.
	 */
	outb(cval | UART_LCR_DLAB, ser->port + UART_LCR);	/* set DLAB */
	outb(quot & 0xff, ser->port + UART_DLL);	/* LS of divisor */
	outb(quot >> 8, ser->port + UART_DLM);		/* MS of divisor */
	outb(cval, ser->port + UART_LCR);		/* reset DLAB */
	outb(0, ser->port + UART_IER);
	outb(UART_MCR_DTR | UART_MCR_RTS, ser->port + UART_MCR);

	/*
	 *	If we read 0xff from the LSR, there is no UART here.
	 */
	if (inb(ser->port + UART_LSR) == 0xff)
		return -1;
#endif
	return 0;
}

static struct console sercons = {
	"ttyS",
	serial_console_write,
	NULL,
	serial_console_device,
	serial_console_wait_key,
	NULL,
	serial_console_setup,
	CON_PRINTBUFFER,
	-1,
	0,
	NULL
};

/*
 *	Register console.
 */
__initfunc (long serial_sa1100_console_init(long kmem_start, long kmem_end))
{
	register_console(&sercons);
	return kmem_start;
}
#endif
