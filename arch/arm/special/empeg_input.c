/*
 * empeg-car Infrared 205/70VR15 support
 *
 * (C) 1999-2000 empeg ltd
 *
 * Authors:
 *   Mike Crowe <mac@empeg.com>
 *
 * This driver supports infrared remote control devices on the
 * empegCar. It currently has support only for the Kenwood credit card
 * remote control (and presumably other Kenwood remotes) and a capture
 * mode to decipher the format of other remote controls.
 *
 * The repeat handling code is biased towards the Kenwood remote and
 * other controls may need a different approach.
 *
 * 1999/02/11 MAC Code tidied up and symbols made static.
 *
 * 1999/02/11 MAC Comments added. Now limitted to only one open.
 *
 * 1999/02/12 MAC Magic sequence support added. It doesn't actually
 *                work yet so it's disabled by default.
 *
 * 1999/03/01 MAC Added support for a repeat timeout. If a repeat code
 *                is received a long time after the previous repeat
 *                code/button code then it is ignored. The actual
 *                data for the repeat is now stored inside the remote
 *                specific function so that the repeat is for the
 *                correct control.
 *
 * 1999/06/02 MAC Generation of repeats are no longer cancelled in
 *                Kenwood handler if dodgy data is received. So we
 *                are relying on the timeout to stop the wrong code
 *                being repeated.
 *
 * 1999/07/03 MAC Various fixes in the IR handlers to reduce the
 *                chances of missed messages. Widened the
 *                acceptable values for the start sequence on
 *                button presses.
 *
 * 1999/07/04 MAC Added bounds checking on kenwood handler so that
 *                repeat codes are now much less likely to come
 *                from the sky.
 *
 * 2000/04/04 MAC Changed to use timing queue, FIQ option.
 *
 * 2000/09/19 MAC Complete overhaul to send button up and button down
 *                codes rather that doing all the repeat stuff here.
 *
 * 2001/10/24 MAC Added anti-glitch support for rotary control. It's a
 *                bit of a hack but it might just work. See later for
 *                full details of * the problem.
 *
 * 2001/10/25 MAC Honed rotary anti-glitch support by testing against
 *                a player that suffered from it.
 *
 * */

/* Output format.
 *
 * Codes are put into the buffer as 32 bit quantities.
 *
 * Infra-red remote controls always generate 24 bits of data ( 16 bits
 * of manufacturer code and 8 bits of error checked button code)
 * therefore the 32 bit quantity always has the top  bits set to
 * zero for button presses. The top bit is set for button up codes.
 *
 * So, in summary:
 *
 *   0000 0000 mmmm mmmm  mmmm mmmm xxxx xxxx = Button down
 *   1000 0000 mmmm mmmm  mmmm mmmm xxxx xxxx = Button up */

/* Since we now use jiffies for the repeat handling we're assuming
   that the device won't be up for 497 days :-) */

#include <linux/sched.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <asm/ptrace.h>
#include <asm/fiq.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/init.h>
#include <asm/system.h>
#include <asm/arch/hardware.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <linux/empeg.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>
#include "empeg_input.h"

#define MS_TO_JIFFIES(MS) ((MS)/(1000/HZ))
#define JIFFIES_TO_MS(J) (((J)*1000)/HZ)
#define US_TO_TICKS(US) ((368 * (US))/100)
#define TICKS_TO_US(T) ((100 * (T))/368)

#define IR_DEBUG 0

#define USE_TIMING_QUEUE 1

/* The CS4231 in the Mk2 uses FIQs */
#ifndef CONFIG_EMPEG_CS4231
#define USE_TIMING_QUEUE_FIQS 1
#else
#define USE_TIMING_QUEUE_FIQS 0
#endif

#ifndef USE_TIMING_QUEUE
#error Non timing queue is no longer supported
#endif

#define IR_TYPE_DEFAULT IR_TYPE_KENWOOD

/* Delay before repeating default */
//#define IR_RPTDELAY_DEFAULT MS_TO_JIFFIES(300) /* .3 seconds */

/* Delay between repeats default */
//#define IR_RPTINT_DEFAULT MS_TO_JIFFIES(100) /*.1 seconds */

/* Timeout for getting a repeat code and still repeating */
#define IR_REPEAT_TIMEOUT MS_TO_JIFFIES(500) /* 0.5 seconds */

/* Freeing up the IRQ breaks on Henry due to the IRQ hack. This means
 * that we can't request and free on open and close. Ultimately we
 * should probably do so but it needs testing on Sonja first.
 */   
//#define REQUEST_IRQ_ON_OPEN 0

#define REMOTE_BUTTON_UP_TIMEOUT MS_TO_JIFFIES(150) /* .15 seconds */

/* Each button press when in one of the control specific modes
 * generates a 4 byte code. When in capture mode each transition
 * received generates a 4 byte code.
 *
 * Therefore, this buffer has room for 256 keypresses or
 * transitions. This should be enough since capturing is only meant as
 * a diagnostic and development feature and therefore should not be
 * attempted if you can't guarantee reading it often enough.
 */
#define IR_BUFFER_SIZE 256

/* The type used for IR codes returned to the user process. */
typedef __u32 input_code;

/* We only support one device */
struct input_dev
{
#if USE_TIMING_QUEUE
	volatile int timings_used;	/* Do not move */
        volatile int timings_free;	/* Do not move */
        volatile int timings_head;      /* Do not move */
	volatile int timings_tail;      /* Do not move */
	volatile unsigned long *timings_buffer;     /* Do not move */
#endif
	struct wait_queue *wq;          /* Blocking queue */
	struct tq_struct timer;         /* Timer queue */
	
	int remote_type;

	/* Repeat support */
	unsigned long last_ir_jiffies;
	unsigned long current_button_down;
	unsigned long last_code_received;

	/* Rotary deglitching support */
	unsigned long last_rotary_jiffies;
	input_code last_rotary_code;
	
	/* Statistics */
	unsigned long count_valid;
	unsigned long count_repeat;
	unsigned long count_badrepeat;
	unsigned long count_spurious;
	unsigned long count_malformed;
	unsigned long count_missed;
#if USE_TIMING_QUEUE
	unsigned long timings_hwm;
#endif
};

/* Rotary control deglitching support. When the rotary controls get
 * old and worn out they start giving false codes in the opposite
 * direction at the end or possibly in the middle of rotation - this
 * causes odd behaviour. So we arrange to ignore the codes indicating
 * an opposite direction if they occur within a certain time of the
 * previous one. */

#define ROTARY_CLOCKWISE_CODE (0xa)
#define ROTARY_ANTICLOCKWISE_CODE (0xb)
#define ROTARY_GLITCH_TIMEOUT_JIFFIES (HZ/4)

static struct input_dev input_devices[1];

/* Used to disallow multiple opens. */
static int users = 0;
#if USE_TIMING_QUEUE_FIQS
static struct fiq_handler fh = { NULL, "empeg_input", NULL, NULL };
#endif

/* Bottom bit must be clear for switch statement */
#define IR_STATE_IDLE 0x00
#define IR_STATE_START1 0x02
#define IR_STATE_START2 0x04
#define IR_STATE_START3 0x06
#define IR_STATE_DATA1  0x08
#define IR_STATE_DATA2 0x0a

/* Work out how many jiffies have passed since the parameter. This
 * means that if past_jiffies is actually in the future it will appear
 * to be hugely in the past. */
unsigned long jiffies_since(unsigned long past_jiffies)
{
	/* Since jiffies is volatile we need to make sure we are using
         * a consistent value for it for the whole function. */
	const unsigned long now_jiffies = jiffies;
	if (past_jiffies <= now_jiffies) {
		/* Simple case */
		return now_jiffies - past_jiffies;
	} else {
		/* Wrap around case */
		return ULONG_MAX - past_jiffies + now_jiffies;
	}
}

extern void input_append_code(void *dev, unsigned long data); /* in hijack.c */
extern int hijack_ir_debug; /* in hijack.c */

static void input_on_remote_repeat(struct input_dev *dev)
{
	if (dev->current_button_down == 0) {
		/* We got a repeat code but nothing was held, if it
	           wasn't too long ago assume that we accidentally
	           sent a button up and send another button down to
	           compensate. */
		if (jiffies_since(dev->last_ir_jiffies) < IR_REPEAT_TIMEOUT) {
			dev->current_button_down = dev->last_code_received;
			input_append_code(dev, dev->current_button_down);
			dev->last_ir_jiffies = jiffies;
		}
	} else {
		/* Since we've had a repeat, make sure we know when it was. */
		dev->last_ir_jiffies = jiffies;
	}
}

static void input_on_remote_up(struct input_dev *dev)
{
	if (dev->current_button_down) {
		input_append_code(dev, (1 << 31) | dev->current_button_down);
		dev->current_button_down = 0;
	}
}

static void input_on_remote_code(struct input_dev *dev,
				 input_code data)
{
	if (dev->current_button_down != data)
		input_on_remote_up(dev);
	dev->current_button_down = data;
	dev->last_code_received = data;
	input_append_code(dev, data);

	// We've got a repeat, notify how long ago our last repeat was.
	dev->last_ir_jiffies = jiffies;
}

#if IR_DEBUG
struct recent_entry {
	int state;
	int interval;
};
#define ENTRY_COUNT 32

static void dump_entries(struct recent_entry *e, int start, int end)
{
	int i = start;
	printk("State level interval\n");
	while (i != end)
	{
		printk("%5x %5d %8d\n", e[i].state & ~1, e[i].state & 1, e[i].interval);
		++i;
		if (i >= ENTRY_COUNT)
			i = 0;
	}
}
#endif

static inline void input_buttons_interrupt(struct input_dev *dev, int level,
					   unsigned long span)
{
	static int state = IR_STATE_IDLE;
	static int unit_time = 1; /* not zero in case we accidentally use it */
	static unsigned short bit_position = 0;
	static __u8 data = 0;
	
#if IR_DEBUG
	static int entry_rp = 0, entry_wp = 0;
	static struct recent_entry entries[ENTRY_COUNT];
#endif

	/* Check to see if the last interrupt was so long ago that
	 * we should restart the state machine.
	 */

	if (span >= US_TO_TICKS(40000)) {
		bit_position = 0;
		state = IR_STATE_IDLE;
	}

#if IR_DEBUG
	entries[entry_wp].state = state | (level ? 1 : 0);
	entries[entry_wp].interval = span;

	entry_wp++;
	if (entry_wp >= ENTRY_COUNT)
		entry_wp = 0;

	if (entry_wp == entry_rp) {
		entry_rp++;
		if (entry_rp >= ENTRY_COUNT)
			entry_rp = 0;
	}
#endif
	
#if IR_DEBUG
#define ON_RECOVER dump_entries(entries, entry_rp, entry_wp)
#define ON_VALID entry_rp = entry_wp = 0
#else
#define ON_RECOVER
#define ON_VALID
#endif
	
	//old_state = state | (level ? 1 : 0);
	/* Now we can actually do something */

#if IR_DEBUG
	{
		int result = (1<<31);
		if (level)
		    result |= (1<<30);
		result |= ((state & 0xF) << 26);
		result |= (span & 0x3ffff);
		ir_append_data(dev, result);
	}
#endif

			
retry:
	switch(state | (level ? 1 : 0))
	{
	case IR_STATE_IDLE | 1:
		/* Going high in idle doesn't mean anything */
		break;
		
	case IR_STATE_IDLE | 0:
		/* Going low in idle is the start of a start sequence */
		state = IR_STATE_START1;
		break;

	case IR_STATE_START1 | 1:
		/* Going high, that should be after 4T */
		unit_time = span / 4;
		if (unit_time > US_TO_TICKS(50) && unit_time < US_TO_TICKS(550))
			state = IR_STATE_START2;
		else
			state = IR_STATE_IDLE; /* There's no point in recovering immediately
						  since this can't be the start of a new
						  sequence. */
		break;

	case IR_STATE_START1 | 0:
		/* Shouldn't ever go low in START1, recover */
		/*state = IR_STATE_IDLE;*/
		/* We jump straight back to START1 since this might still */
		/* be the start of a sequence */
		ON_RECOVER;
		state = IR_STATE_IDLE;
		++dev->count_missed;
		goto retry;

	case IR_STATE_START2 | 1:
		/* Shouldn't ever go high in START2, recover */
		state = IR_STATE_IDLE;
		++dev->count_missed;
		break;

	case IR_STATE_START2 | 0:
		/* If this forms the end of the start sequence then we
		 * should have been high for around 8T time.
		 */
		if (span > 7 * unit_time && span < 9 * unit_time) {
			/* It's the start of one of the unit button codes. */
			data = 0;
			bit_position = 0;
			state = IR_STATE_DATA1;
		} else {
			/* We're out of bounds. Give up, but this might be the
			   start of a valid start sequence. */
			ON_RECOVER;
			state = IR_STATE_IDLE;
			goto retry; /* try again */
		}
		break;

	case IR_STATE_DATA1 | 0:
		/* This should never happen */
		state = IR_STATE_IDLE;
		++dev->count_missed;
		break;
		
	case IR_STATE_DATA1 | 1:
		data <<= 1;
		if (span <= 3 * unit_time) {
			/* It's a zero bit */
			bit_position++;
			state = IR_STATE_DATA2;
		} else if (span > 3 * unit_time && span < 5 * unit_time) {
			/* It's a one bit */
			data |= 1;
			bit_position++;
			state = IR_STATE_DATA2;
		} else {
			/* This can't be the start of a start sequence since
			   we're going high, so just give up */
			ON_RECOVER;
			state = IR_STATE_START1;
			goto retry;
		}

		/* Now process the bit */
		if (bit_position > 7) {
			/* Does it pass the validity check */
			if (((data>>4)^(data&0xf))==0xf)
			{
				/* We don't do anything with repeats so go
				   straight to the real code */

				input_code code = (data >> 4);

				if ((code == ROTARY_CLOCKWISE_CODE) || (code == ROTARY_ANTICLOCKWISE_CODE)) {
					/* We need to do special
					 * handling here 'cos when the
					 * rotary controls get old and
					 * worn out they start giving
					 * false codes in the opposite
					 * direction sometimes.
					 */
					
					input_code previous_rotary_code = dev->last_rotary_code;
					unsigned long previous_rotary_jiffies = dev->last_rotary_jiffies;

					/* We want to do multiple
                                         * codes within the timeout
                                         * because it appears that
                                         * now I've seen a player
                                         * that really suffered from
                                         * the problem it may
                                         * generate up to two bad
                                         * codes in a row.
					 */
					dev->last_rotary_jiffies = jiffies;
					
					if ((code != previous_rotary_code)
					    && (jiffies_since(previous_rotary_jiffies) < ROTARY_GLITCH_TIMEOUT_JIFFIES)) {
						/* Don't use the code then. */
						break;
					}
					dev->last_rotary_code = code;
				}
				input_append_code(dev, data >> 4);
				ON_VALID;
			        state = IR_STATE_IDLE;
			}
			else
			{
				/* Report CRC failures */
				//ir_append_data(dev, tv_now, 0xFF00 | data);
				ON_RECOVER;
				state = IR_STATE_START1;
				goto retry;
			}
		}
		break;

	case IR_STATE_DATA2 | 0:
		if (span < 2 * unit_time) {
			/* It's a correct inter-bit gap */
			state = IR_STATE_DATA1;
		} else {
			ON_RECOVER;
			/* It's out of bounds. It might be the start
			   of another valid sequence so try again. */
			state = IR_STATE_IDLE;
			goto retry;
		}
		break;

	case IR_STATE_DATA2 | 1:
		/* Should never get here */
		ON_RECOVER;
		state = IR_STATE_IDLE;
		++dev->count_missed;
		break;
		
	default:
#if IR_DEBUG
		printk("Buttons handler got into impossible state. Recovering.\n");
		state = IR_STATE_IDLE;
#endif
		break;
	}
}

static inline void input_kenwood_interrupt(struct input_dev *dev, int level,
					   unsigned long span)
{
	static int state = IR_STATE_IDLE;
	static int unit_time = 1; /* not zero in case we accidentally use it */
	static unsigned short bit_position = 0;
	static __u32 data = 0;
	static input_code decoded_data = 0;
	static int repeat_valid = FALSE;
	//static struct timeval tv_last_valid;
	
	/* Check to see if the last interrupt was so long ago that
	 * we should restart the state machine.
	 */

	if (span >= US_TO_TICKS(40000)) {
		bit_position = 0;
		state = IR_STATE_IDLE;
	}

	/* Now we can actually do something */

 retry:
	switch(state | (level ? 1 : 0))
	{
	case IR_STATE_IDLE | 1:
		/* Going high in idle doesn't mean anything */
		break;
		
	case IR_STATE_IDLE | 0:
		/* Going low in idle is the start of a start sequence */
		state = IR_STATE_START1;
		break;

	case IR_STATE_START1 | 1:
		/* Going high, that should be after 8T */
		unit_time = span / 8;
		/* But some bounds on it so we don't start receiving magic
		   codes from the sky */
		if (unit_time > US_TO_TICKS(500) && unit_time < US_TO_TICKS(1500))
			state = IR_STATE_START2;
		else
			state = IR_STATE_IDLE;
		break;

	case IR_STATE_START1 | 0:
		/* Shouldn't ever go low in START1, recover */
		state = IR_STATE_IDLE;
		++dev->count_missed;
		break;

	case IR_STATE_START2 | 1:
		/* Shouldn't ever go low in START2, recover */
		state = IR_STATE_IDLE;
		++dev->count_missed;
		break;

	case IR_STATE_START2 | 0:
		/* If this forms the end of the start sequence then we
		 * should have been high for around 4T time.
		 */
		if ((span >= 3 * unit_time) &&
			(span < 5 * unit_time)) {
			state = IR_STATE_START3;
			/*repeat_valid = FALSE;*/
		} else if (span > unit_time && span < 3 * unit_time) {
			/* This means that the last code is repeated - just
			   send out the last code again with the top bit set to indicate
			   a repeat. */
			if (repeat_valid)
			{
				input_on_remote_repeat(dev);
				++dev->count_repeat;
			}
			else
			{
				++dev->count_badrepeat;
			}
			state = IR_STATE_IDLE;
		} else {
			/* We're out of bounds. Recover */
			state = IR_STATE_IDLE;
			/* But it could be the start of a new sequence
                           so try again */
			++dev->count_spurious;
			goto retry;
		}
		break;

	case IR_STATE_START3 | 0:
		/* Shouldn't happen */
		state = IR_STATE_IDLE;
		++dev->count_missed;
		break;

	case IR_STATE_START3 | 1:
		/* Data will follow this */
		if (span < unit_time) {
			bit_position = 0;
			data = 0;
			state = IR_STATE_DATA1;
		} else {
			/* We're out of bounds. It might be the start
			   of a new sequence so try it again. */
			state = IR_STATE_IDLE;
			++dev->count_spurious;
			goto retry;
		}
		break;
		
	case IR_STATE_DATA1 | 1:
		/* Shouldn't get this. Recover */
		state = IR_STATE_IDLE;
		++dev->count_missed;
		break;

	case IR_STATE_DATA1 | 0:
		/* The actual data bit is encoded in the length of this.
		 */
		if (span < unit_time) {
			/* It's a zero */
			bit_position++;
			state = IR_STATE_DATA2;
		} else if (span < 2 * unit_time) {
			/* It's a one */
			data |= (1<<bit_position);
			bit_position++;
			state = IR_STATE_DATA2;
		} else {
			/* Not valid. It might be the start of a new
                           sequence though. */
			state = IR_STATE_IDLE;
			++dev->count_spurious;
			goto retry;
		}
		break;

	case IR_STATE_DATA2 | 1:
		/* This marks the end of the post-data space
		 * It is a consistent length
		 */
		if (span < unit_time) {
			/* It's a valid space */
			if (bit_position >= 32) {
				__u16 mfr;
				__u8 data1, data2;
				__u32 cpu_data = be32_to_cpu(data);

				// On modern remotes the manufacturer code is not protected.
				mfr = cpu_data >> 16;
				data1 = (cpu_data >> 8) & 0xff;
				data2 = cpu_data & 0xff;

				/* We've finished getting data, confirm
				   it passes the validity check */
				if (data1 == ((__u8)(~data2))) {
					decoded_data = ((__u32)(mfr) << 8) | data1;
					input_on_remote_code(dev, decoded_data);
					repeat_valid = TRUE;
					++dev->count_valid;
				} else {
#if IR_DEBUG
					printk("Got an invalid sequence %08lx (%04lx, %04lx)\n",
					       (unsigned long)data, (unsigned long)data1,
					       (unsigned long)data2);
					printk("(%04lx %04lx, %04lx %04lx)\n",
					       (unsigned long)data1, (unsigned long)~data1,
					       (unsigned long)data2, (unsigned long)~data2);
#endif
					++dev->count_malformed;
				}
				state = IR_STATE_IDLE;
			}
			else
				state = IR_STATE_DATA1;
		} else {
			/* It's too long to be valid. Give up and try again. */
			state = IR_STATE_IDLE;
			++dev->count_spurious;
			goto retry;
		}
		break;

	case IR_STATE_DATA2 | 0:
		/* Shouldn't get this. Recover */
		state = IR_STATE_IDLE;
		++dev->count_missed;
		break;

	default:
		state = IR_STATE_IDLE;
		break;
	}
}

static inline void input_capture_interrupt(struct input_dev *dev, int level,
					unsigned long span)
{
	unsigned long us = TICKS_TO_US(span);
	/* Just capture it straight to the output buffer */
	if (level)
		input_append_code(dev, us | (1<<30) | (1<<31));
	else
		input_append_code(dev, (us & ~(1<<30)) | (1<<31));
}

static inline void ir_transition(struct input_dev *dev, int level, unsigned long span)
{
	/* Call buttons interrupt handler */
	if (dev->remote_type != IR_TYPE_CAPTURE)
		input_buttons_interrupt(dev, level, span);
	
	/* Call remote specific interrupt handler */

	switch (dev->remote_type)
	{
	case IR_TYPE_CAPTURE:
		input_capture_interrupt(dev, level, span);
		break;
	case IR_TYPE_KENWOOD:
		input_kenwood_interrupt(dev, level, span);
		break;
	default:
		/* Hmm, I wonder what it was supposed to be */
		dev->remote_type = IR_TYPE_KENWOOD;
		break;
	}
}

#if USE_TIMING_QUEUE_FIQS
static void input_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	/* Not needed if we're running on FIQs */
	printk("BAD!\n");
}
#else
static void input_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	int level;
	unsigned long now; 
	unsigned long entry;
	unsigned long flags;
	struct input_dev *dev = input_devices;
	
	save_flags_cli(flags);

	level = (GPLR & EMPEG_IRINPUT)?1:0;
	now = OSCR;

	entry = (now & ~1) | level;
	
	if (dev->timings_free) {
		--dev->timings_free;
		dev->timings_buffer[dev->timings_head] = entry;
		if (++dev->timings_head >= TIMINGS_BUFFER_SIZE)
			dev->timings_head = 0;
		++dev->timings_used;
	}

	restore_flags(flags);
	
}
#endif

static void input_check_buffer(void *dev_id)
{
	/* This stores the time of the last actual interrupt, not the time this
	 * routine was called
	 */
	static unsigned long last_interrupt;
	
	struct input_dev *dev = dev_id;

	while (dev->timings_used) {
		unsigned long flags;
		unsigned long entry;
		unsigned long span;
		unsigned long interrupt_time;
		int level;

		/* Keep track of the hwm */
		{
			int used = dev->timings_used;
			if (used > dev->timings_hwm)
				dev->timings_hwm = used;
		}

		/* Safe to do even if an interrupt happens during it. */
		entry = dev->timings_buffer[dev->timings_tail];
		
		/* Disable interrupts while we tidy up the pointers */
		save_flags_clif(flags);
		++dev->timings_free;
		--dev->timings_used;
		restore_flags(flags);

		if (++dev->timings_tail >= TIMINGS_BUFFER_SIZE)
			dev->timings_tail = 0;

		/* Now, we have our entry, go and deal with it. */
		interrupt_time = entry & ~1;
		level = entry & 1;

		span = interrupt_time - last_interrupt;
		last_interrupt = interrupt_time;

		//printk("Transition(%d): %d %5ld\n", dev->timings_tail, level, span);
		ir_transition(dev, level, span);
	}

	/* If we haven't had a repeat code for a while send a button up. */
	if ((dev->current_button_down != 0) && (jiffies_since(dev->last_ir_jiffies) > REMOTE_BUTTON_UP_TIMEOUT))
		input_on_remote_up(dev);
	
	/* Requeue me */
	queue_task(&dev->timer, &tq_timer);
}

static int input_open(struct inode *inode, struct file *filp)
{
	struct input_dev *dev = input_devices;

	if (users)
		return -EBUSY;

	users++;
	MOD_INC_USE_COUNT;
	
	/* This shouldn't be necessary, but there's something (IDE, audio?)
	 * that's setting rather than or'ing these and breaking it after
	 * initialisation.
	 */
	GRER|=EMPEG_IRINPUT;
	GFER|=EMPEG_IRINPUT;
	GEDR=EMPEG_IRINPUT;

	dev->remote_type = IR_TYPE_DEFAULT;
	filp->private_data = dev;
	
	return 0;
}

static int input_release(struct inode *inode, struct file *filp)
{
	--users;
	MOD_DEC_USE_COUNT;
	return 0;
}

void input_wakeup_waiters (void)	// called from hijack.c
{
	if (hijack_ir_debug)
		printk("Wakeup\n");
	wake_up_interruptible(&input_devices->wq);
}

int hijack_playerq_deq (unsigned int *rbutton);	// returns 1 if no button available
/*
 * Read some bytes from the IR buffer. The count should be a multiple
 * of four bytes - if it isn't then it is rounded down. If the
 * rounding means that it is zero then EINVAL will result.
 */
static ssize_t input_read(struct file *filp, char *dest, size_t count,
		       loff_t *ppos)
{
	struct input_dev *dev = filp->private_data;
	unsigned int data, n = 0;

	// count must be a non-zero multiple of 4 here
	count >>= 2;
	if (count == 0)
		return -EINVAL;

	if (hijack_playerq_deq(&data)) {	// no data available?
		int sig = 0;
		struct wait_queue wait = { current, NULL };

		// If we're nonblocking then return immediately
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		// Wait for some data to turn up - this method avoids race
		// conditions see p209 of Linux device drivers.
		if (hijack_ir_debug)
			printk("Waiting..\n");
		add_wait_queue(&dev->wq, &wait);
		current->state = TASK_INTERRUPTIBLE;	
		while (hijack_playerq_deq(&data)) {
			sig = signal_pending(current);
			if (sig)
				break;
			schedule();
		}
		current->state = TASK_RUNNING;
		remove_wait_queue(&dev->wq, &wait);
		if (sig)
	    		return -ERESTARTSYS;
	}

	do {
		// Need to put one byte at a time since there are
		//  no alignment requirements on parameters to read.
		__put_user((data      ) & 0xFF, dest++);
		__put_user((data >>  8) & 0xFF, dest++);
		__put_user((data >> 16) & 0xFF, dest++);
		__put_user((data >> 24) & 0xFF, dest++);
	} while (++n < count && !hijack_playerq_deq(&data));
	return n << 2;
}

unsigned int input_poll(struct file *filp, poll_table *wait)
{
	struct input_dev *dev = filp->private_data;

	/* Add ourselves to the wait queue */
	if (wait && hijack_ir_debug)
		printk("Poll wait\n");
	poll_wait(filp, &dev->wq, wait);

	/* Check if we've got data to read */
	if (!hijack_playerq_deq(NULL)) {
		if (hijack_ir_debug)
			printk("Poll ok\n");
		return POLLIN | POLLRDNORM;
	}
	if (wait && hijack_ir_debug)
		printk("Poll fail\n");
	return 0;
}

int input_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
	     unsigned long arg)
{
	struct input_dev *dev = filp->private_data;

	switch(cmd)
	{
	case EMPEG_IR_WRITE_TYPE:
		if (arg >= IR_TYPE_COUNT)
			return -EINVAL;
		dev->remote_type = arg;
		return 0;
		
	case EMPEG_IR_READ_TYPE:
		return put_user(dev->remote_type, (int *)arg);
	default:
		return -EINVAL;
	}
}

static struct file_operations input_fops = {
	NULL, /* ir_lseek */
	input_read,
	NULL,	//input_write, /* for DisplayServer */
	NULL, /* ir_readdir */
	input_poll, /* ir_poll */
	input_ioctl,
	NULL, /* ir_mmap */
	input_open,
	NULL, /* ir_flush */
	input_release,
};

int input_read_procmem(char *buf, char **start, off_t offset, int len, int unused)
{
	struct input_dev *dev = input_devices;
	len = 0;

	len += sprintf(buf+len, "Valid sequences:      %ld\n", dev->count_valid);
	len += sprintf(buf+len, "Repeated sequences:   %ld\n", dev->count_repeat);
	len += sprintf(buf+len, "Unfulfilled repeats:  %ld\n", dev->count_badrepeat);
	len += sprintf(buf+len, "Malformed sequences:  %ld\n", dev->count_malformed);
	len += sprintf(buf+len, "Spurious transitions: %ld\n", dev->count_spurious);
	len += sprintf(buf+len, "Missed interrupts:    %ld\n", dev->count_missed);
	len += sprintf(buf+len, "Timings buffer hwm:   %ld\n", dev->timings_hwm);

	return len;
}

struct proc_dir_entry input_proc_entry = {
	0,			/* inode (dynamic) */
	8, "empeg_ir",  	/* length and name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	0, 			/* size */
	NULL, 			/* use default operations */
	&input_read_procmem, 	/* function used to read data */
};

void __init empeg_input_init(void)
{
	struct input_dev *dev = input_devices;
	int result;
#if USE_TIMING_QUEUE_FIQS
	struct pt_regs regs;
	extern char empeg_input_fiq, empeg_input_fiqend;
#endif

	result = register_chrdev(EMPEG_IR_MAJOR, "empeg_input", &input_fops);
	if (result < 0) {
		printk(KERN_WARNING "empeg IR: Major number %d unavailable.\n",
			   EMPEG_IR_MAJOR);
		return;
	}

	/* First grab the memory buffer */
	dev->wq = NULL;
	dev->remote_type = IR_TYPE_DEFAULT;

	dev->last_ir_jiffies = 0;
	dev->current_button_down = 0;
	dev->last_code_received = 0;
	
	dev->last_rotary_jiffies = 0;
	dev->last_rotary_code = 0;
	
	dev->count_valid = 0;
	dev->count_repeat = 0;
	dev->count_badrepeat = 0;
	dev->count_spurious = 0;
	dev->count_malformed = 0;
	dev->count_missed = 0;

#if USE_TIMING_QUEUE
	dev->timings_hwm = 0;

	dev->timings_free = TIMINGS_BUFFER_SIZE;
	dev->timings_used = 0;
	dev->timings_head = 0;
	dev->timings_tail = 0;
	dev->timings_buffer = vmalloc(TIMINGS_BUFFER_SIZE * sizeof(unsigned long));
	
	/* Set up timer routine to check the buffer */
	dev->timer.sync = 0;
	dev->timer.routine = input_check_buffer;
	dev->timer.data = dev;
	queue_task(&dev->timer, &tq_timer);

#if USE_TIMING_QUEUE_FIQS
	/* Install FIQ handler */
	regs.ARM_r9=(int)dev;
	regs.ARM_r10=(int)&OSCR; 	
	regs.ARM_fp=0; 		/* r11 */
	regs.ARM_ip=0;	 	/* r12 */
	regs.ARM_sp=(int)&GPLR;
	set_fiq_regs(&regs);

	set_fiq_handler(&empeg_input_fiq,(&empeg_input_fiqend-&empeg_input_fiq));
	claim_fiq(&fh);
#endif
#endif
       	/* No interrupts yet */
	GRER&=~EMPEG_IRINPUT;
	GFER&=~EMPEG_IRINPUT;
	GEDR=EMPEG_IRINPUT;

	/* IRQs shouldn't be reenabled, the routine is very fast */
	result = request_irq(EMPEG_IRQ_IR, input_interrupt, SA_INTERRUPT,
			     "empeg_input", dev);
	
	if (result != 0) {
		printk(KERN_ERR "Can't get empeg IR IRQ %d.\n", EMPEG_IRQ_IR);
		return;
	}

#if USE_TIMING_QUEUE_FIQS
	/* It's a FIQ not an IRQ */
	ICLR|=EMPEG_IRINPUT;

	/* Enable FIQs: there should be a neater way of doing
	   this... */
	{
		unsigned long flags;
		save_flags(flags);
		flags&=~F_BIT;
		restore_flags(flags);
	}
#endif
  
       	/* We want interrupts on rising and falling */
	GRER|=EMPEG_IRINPUT;
	GFER|=EMPEG_IRINPUT;
	
#ifdef CONFIG_PROC_FS
	proc_register(&proc_root, &input_proc_entry);
#endif

#if USE_TIMING_QUEUE_FIQS
	printk("empeg remote control/panel button support initialised (Using FIQs).\n");
#else
	printk("empeg remote control/panel button initialised.\n");
#endif
}

static inline void empeg_input_cleanup(void)
{
	int result;
	struct input_dev *dev = input_devices;

	free_irq(EMPEG_IRQ_IR, dev);

	/* No longer require interrupts */
	GRER&=~(EMPEG_IRINPUT);
	GFER&=~(EMPEG_IRINPUT);

	result = unregister_chrdev(EMPEG_IR_MAJOR, "empeg_ir");
	if (result < 0)
		printk(KERN_WARNING "empeg_input: Unable to unregister device.\n");
	printk("empeg IR cleanup complete.\n");
}

#ifdef MODULE
MODULE_AUTHOR("Mike Crowe");
MODULE_DESCRIPTION("A driver for the empeg infrared remote control and panel buttons");
MODULE_SUPPORTED_DEVICE("ir");

EXPORT_NO_SYMBOLS

int init_module(void)
{
	return empeg_input_init();
}

void cleanup_module(void)
{
	empeg_input_cleanup();
}

#endif /* MODULE */
