/*
   drivers/char/serial_285.c

   Driver for the serial port on the 21285 StrongArm-110 core logic chip.

   Based on drivers/char/serial.c
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
#include <linux/serial_reg.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/console.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/uaccess.h>
#include <asm/bitops.h>
#include <asm/dec21285.h>
#include <asm/hardware.h>

#define BAUD_BASE 781250
#define SERIAL_285_MINOR_START 64

static struct tty_driver rs285_driver;
static int rs285_refcount;
static struct tty_struct *rs285_table[1];

static struct termios *rs285_termios[1];
static struct termios *rs285_termios_locked[1];

static char wbuf[1000], *putp = wbuf, *getp = wbuf, x_char = 0;
static struct tty_struct *rs285_tty = NULL;
static int rs285_use_count = 0;

static int rs285_write_room(struct tty_struct *tty)
{
	return putp >= getp ? (sizeof(wbuf) - (long) putp + (long) getp) : ((long) getp - (long) putp - 1);
}

static void rs285_rx_int(int irq, void *dev_id, struct pt_regs *regs)
{
	if (!rs285_tty) {
		disable_irq(IRQ_CONRX);
		return;
	}
	while (!(*CSR_UARTFLG & 0x10)) {
		int ch, flag;
		ch = *CSR_UARTDR;
		flag = *CSR_RXSTAT;
		if (flag & 4)
			tty_insert_flip_char(rs285_tty, 0, TTY_OVERRUN);
		if (flag & 2)
			flag = TTY_PARITY;
		else if (flag & 1)
			flag = TTY_FRAME;
		tty_insert_flip_char(rs285_tty, ch, flag);
	}
	tty_flip_buffer_push(rs285_tty);
}

static void rs285_send_xchar(struct tty_struct *tty, char ch)
{
	x_char = ch;
	enable_irq(IRQ_CONTX);
}

static void rs285_throttle(struct tty_struct *tty)
{
	if (I_IXOFF(tty))
		rs285_send_xchar(tty, STOP_CHAR(tty));
}

static void rs285_unthrottle(struct tty_struct *tty)
{
	if (I_IXOFF(tty)) {
		if (x_char)
			x_char = 0;
		else
			rs285_send_xchar(tty, START_CHAR(tty));
	}
}

static void rs285_tx_int(int irq, void *dev_id, struct pt_regs *regs)
{
	while (!(*CSR_UARTFLG & 0x20)) {
		if (x_char) {
			*CSR_UARTDR = x_char;
			x_char = 0;
			continue;
		}
		if (putp == getp) {
			disable_irq(IRQ_CONTX);
			break;
		}
		*CSR_UARTDR = *getp;
		if (++getp >= wbuf + sizeof(wbuf))
			getp = wbuf;
	}
	if (rs285_tty)
		wake_up_interruptible(&rs285_tty->write_wait);
}

static inline int rs285_xmit(int ch)
{
	if (putp + 1 == getp || (putp + 1 == wbuf + sizeof(wbuf) && getp == wbuf))
		return 0;
	*putp = ch;
	if (++putp >= wbuf + sizeof(wbuf))
		putp = wbuf;
	enable_irq(IRQ_CONTX);
	return 1;
}

static int rs285_write(struct tty_struct *tty, int from_user,
		       const u_char * buf, int count)
{
	int i;
	if (from_user) {
		if (verify_area(VERIFY_READ, buf, count))
			return -EINVAL;
	}
	for (i = 0; i < count; i++) {
		char ch;
		if (from_user)
			get_user(ch, buf + i);
		else
			ch = buf[i];
		if (!rs285_xmit(ch))
			break;
	}
	return i;
}

static void rs285_put_char(struct tty_struct *tty, u_char ch)
{
	rs285_xmit(ch);
}

static int rs285_chars_in_buffer(struct tty_struct *tty)
{
	return sizeof(wbuf) - rs285_write_room(tty);
}

static void rs285_flush_buffer(struct tty_struct *tty)
{
	disable_irq(IRQ_CONTX);
	putp = getp = wbuf;
	if (x_char)
		enable_irq(IRQ_CONTX);
}

static inline void rs285_set_cflag(int cflag)
{
	int h_lcr, baud, quot;

	switch (cflag & CSIZE) {
	case CS5:
		h_lcr = 0x10;
		break;
	case CS6:
		h_lcr = 0x30;
		break;
	case CS7:
		h_lcr = 0x50;
		break;
	default:
		h_lcr = 0x70;
		break;		// CS8

	}
	if (cflag & CSTOPB)
		h_lcr |= 0x08;
	if (cflag & PARENB)
		h_lcr |= 0x02;
	if (!(cflag & PARODD))
		h_lcr |= 0x04;

	switch (cflag & CBAUD) {
	case B19200:
		baud = 19200;
		break;
	case B38400:
		baud = 38400;
		break;
	default:
		baud = 9600;
	}
	quot = BAUD_BASE / baud - 1;

	*CSR_UARTCON = 0;
	*CSR_L_UBRLCR = quot & 0xff;
	*CSR_M_UBRLCR = (quot >> 8) & 0x0f;
	*CSR_H_UBRLCR = h_lcr;
	*CSR_UARTCON = 1;
}

static void rs285_set_termios(struct tty_struct *tty, struct termios *old)
{
	if (old && tty->termios->c_cflag == old->c_cflag)
		return;
	rs285_set_cflag(tty->termios->c_cflag);
}


static void rs285_stop(struct tty_struct *tty)
{
	disable_irq(IRQ_CONTX);
}

static void rs285_start(struct tty_struct *tty)
{
	enable_irq(IRQ_CONTX);
}

static void rs285_wait_until_sent(struct tty_struct *tty, int timeout)
{
	int orig_jiffies = jiffies;
	while (*CSR_UARTFLG & 8) {
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(1);
		if (signal_pending(current))
			break;
		if (timeout && ((orig_jiffies + timeout) < jiffies))
			break;
	}
	current->state = TASK_RUNNING;
}

static int rs285_open(struct tty_struct *tty, struct file *filp)
{
	int line;
	line = MINOR(tty->device) - tty->driver.minor_start;
	if (line)
		return -ENODEV;
	tty->driver_data = NULL;
	if (!rs285_tty)
		rs285_tty = tty;
	enable_irq(IRQ_CONRX);
	rs285_use_count++;
	return 0;
}

static void rs285_close(struct tty_struct *tty, struct file *filp)
{
	if (!--rs285_use_count) {
		rs285_wait_until_sent(tty, 0);
		disable_irq(IRQ_CONRX);
		disable_irq(IRQ_CONTX);
		rs285_tty = NULL;
	}
}

__initfunc(int rs285_init(void))
{
	memset(&rs285_driver, 0, sizeof(rs285_driver));
	rs285_driver.magic = TTY_DRIVER_MAGIC;
	rs285_driver.driver_name = "serial_285";
	rs285_driver.name = "ttyI";
	rs285_driver.major = TTY_MAJOR;
	rs285_driver.minor_start = SERIAL_285_MINOR_START;
	rs285_driver.num = 1;
	rs285_driver.type = TTY_DRIVER_TYPE_SERIAL;
	rs285_driver.subtype = SERIAL_TYPE_NORMAL;
	rs285_driver.init_termios = tty_std_termios;
	rs285_driver.init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	rs285_driver.flags = TTY_DRIVER_REAL_RAW;
	rs285_driver.refcount = &rs285_refcount;
	rs285_driver.table = rs285_table;
	rs285_driver.termios = rs285_termios;
	rs285_driver.termios_locked = rs285_termios_locked;

	rs285_driver.open = rs285_open;
	rs285_driver.close = rs285_close;
	rs285_driver.write = rs285_write;
	rs285_driver.put_char = rs285_put_char;
	rs285_driver.flush_chars = NULL;
	rs285_driver.write_room = rs285_write_room;
	rs285_driver.chars_in_buffer = rs285_chars_in_buffer;
	rs285_driver.flush_buffer = rs285_flush_buffer;
	rs285_driver.ioctl = NULL;
	rs285_driver.throttle = rs285_throttle;
	rs285_driver.unthrottle = rs285_unthrottle;
	rs285_driver.send_xchar = rs285_send_xchar;
	rs285_driver.set_termios = rs285_set_termios;
	rs285_driver.stop = rs285_stop;
	rs285_driver.start = rs285_start;
	rs285_driver.hangup = NULL;
	rs285_driver.break_ctl = NULL;
	rs285_driver.wait_until_sent = rs285_wait_until_sent;
	rs285_driver.read_proc = NULL;

	if (request_irq(IRQ_CONRX, rs285_rx_int, 0, "rs285", NULL))
		panic("Couldn't get rx irq for rs285");

	if (request_irq(IRQ_CONTX, rs285_tx_int, 0, "rs285", NULL))
		panic("Couldn't get tx irq for rs285");

	if (tty_register_driver(&rs285_driver))
		panic("Couldn't register 21285 serial driver\n");

	return 0;
}

#ifdef CONFIG_SERIAL_21285_CONSOLE
/************** console driver *****************/

static void rs285_console_write(struct console *co, const char *s, u_int count)
{
	int i;

	disable_irq(IRQ_CONTX);
	for (i = 0; i < count; i++) {
		while (*CSR_UARTFLG & 0x20);
		*CSR_UARTDR = s[i];
		if (s[i] == '\n') {
			while (*CSR_UARTFLG & 0x20);
			*CSR_UARTDR = '\r';
		}
	}
	enable_irq(IRQ_CONTX);
}

static int rs285_console_wait_key(struct console *co)
{
	int c;

	disable_irq(IRQ_CONRX);
	while (*CSR_UARTFLG & 0x10);
	c = *CSR_UARTDR;
	enable_irq(IRQ_CONRX);
	return c;
}

static kdev_t rs285_console_device(struct console *c)
{
	return MKDEV(TTY_MAJOR, SERIAL_285_MINOR_START);
}

__initfunc(static int rs285_console_setup(struct console *co, char *options))
{
	int baud = 9600;
	int bits = 8;
	int parity = 'n';
	int cflag = CREAD | HUPCL | CLOCAL;

	if (0 && options) {
		char *s = options;
		baud = simple_strtoul(options, NULL, 10);
		while (*s >= '0' && *s <= '9')
			s++;
		if (*s)
			parity = *s++;
		if (*s)
			bits = *s - '0';
	}
	/*
	 *    Now construct a cflag setting.
	 */
	switch (baud) {
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
	default:
		cflag |= B9600;
		break;
	}
	switch (bits) {
	case 7:
		cflag |= CS7;
		break;
	default:
		cflag |= CS8;
		break;
	}
	switch (parity) {
	case 'o':
	case 'O':
		cflag |= PARODD;
		break;
	case 'e':
	case 'E':
		cflag |= PARENB;
		break;
	}
	co->cflag = cflag;
	rs285_set_cflag(cflag);
	rs285_console_write(NULL, "\e[2J\e[Hboot ", 12);
	if (options)
		rs285_console_write(NULL, options, strlen(options));
	else
		rs285_console_write(NULL, "no options", 10);
	rs285_console_write(NULL, "\n", 1);

	return 0;
}

static struct console rs285_cons =
{
	"ttyI",
	rs285_console_write,
	NULL,
	rs285_console_device,
	rs285_console_wait_key,
	NULL,
	rs285_console_setup,
	CON_PRINTBUFFER,
	-1,
	0,
	NULL
};

__initfunc(long rs285_console_init(long kmem_start, long kmem_end))
{
	register_console(&rs285_cons);
	return kmem_start;
}

#endif				// CONFIG_SERIAL_21285_CONSOLE
