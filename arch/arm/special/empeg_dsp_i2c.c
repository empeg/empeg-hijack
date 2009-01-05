/*
 * SA1100/empeg DSP (Philips) interface via i2c
 *
 * (C) 2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Hugo Fiennes, <hugo@empeg.com>
 *   John Ripley, <john@empeg.com>
 *
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <asm/delay.h>
#include <asm/arch/hardware.h>
#include <asm/arch/hardware.h>

#include "empeg_dsp_i2c.h"

#ifdef CONFIG_EMPEG_I2C_FAN_CONTROL
	// semaphore to serialize access on the i2c bus:
	#include <asm/semaphore.h>
	static struct semaphore i2c_busy = MUTEX;
	#define GRAB_I2C_SEMA		down(&i2c_busy)
	#define RELEASE_I2C_SEMA	up(&i2c_busy)
#else
	#define GRAB_I2C_SEMA
	#define RELEASE_I2C_SEMA
#endif

/*
 * Delay stuff
 * These are the minimum delays that the i2c bus can take before
 * the lines fail to get driven to the correct level.
 */

static __inline__ void i2c_delay_long(void)
{
	udelay(15);	// any lower and the lines don't float
}

static __inline__ void i2c_delay_short(void)
{
	udelay(1);	// any lower and the lines don't float
}

static int i2c_stopped = 0;

/* Pulse out the start sequence */

static void i2c_startseq(void)
{
	/* avoid long initial delay if we were properly stopped last time */
	if (!i2c_stopped) {
		/* clock high, data high */
		GPSR = IIC_CLOCK;
		GPCR = IIC_DATAOUT;
		i2c_delay_long();
	}
	i2c_stopped = 0;

	/* Put data low */
	GPSR = IIC_DATAOUT;
	i2c_delay_long();
}

/* Pulse out the stop sequence */

static void i2c_stopseq(void)
{
	/* Data low, clock low */
	GPCR = IIC_CLOCK;
	GPSR = IIC_DATAOUT;
	i2c_delay_long();

	/* Clock high */
	GPSR = IIC_CLOCK;
	i2c_delay_short();
	
	/* Let data float high */
	GPCR = IIC_DATAOUT;
	i2c_delay_long();

	i2c_stopped = 1; /* permits a faster i2c_startseq() next time around */
}

/*
 * Read in a single bit, assumes the current state is clock low
 * and that the data direction is inbound.
 */

static int i2c_getdatabit(void)
{
	int result;

	/* Trigger the clock */
	GPSR = IIC_CLOCK;
	i2c_delay_long();
	
	/* Wait for the slave to give me the data */
	result = !(GPLR & IIC_DATAIN);

	/* Now take the clock low */
	GPCR = IIC_CLOCK;
	i2c_delay_long();
	return result;
}

/*
 * Pulse out a single bit, assumes the current state is clock low
 * and that the data direction is outbound.
 */

static void i2c_putdatabit(int bit)
{
	unsigned int old_bit;

	/* First set the data bit (clock low) */
	old_bit = !(GPLR & IIC_DATAOUT);
	if (bit != old_bit) {
		if (bit) {
			GPCR = IIC_DATAOUT;
			/* The 100K pull-up resistor will float
			 * the data line high here, but very slowly,
			 * so allow lots of time for it to do so.
			 */
			i2c_delay_long();
			i2c_delay_short();
		} else {
			/* The CPU drives the data line low here,
			 * very quickly, so only a short delay is needed.
			 */
			GPSR = IIC_DATAOUT;
		}
	}

	/* Now trigger the clock */
	i2c_delay_short();
	GPSR = IIC_CLOCK;
	i2c_delay_short();
	
	/* Drop the clock */
	GPCR = IIC_CLOCK;
	i2c_delay_short();
}

/*
 * Read an entire byte and send out acknowledge.
 * Returns byte read.
 */

static unsigned char i2c_getbyte(int nak)
{
	int i;
	unsigned char byte = 0;

	/* Let data line float */
	GPCR = IIC_DATAOUT;
	i2c_delay_long();

	/* Clock in the data */
	for (i = 7; i >= 0; --i) {
		if (i2c_getdatabit())
			byte |= (1 << i);
	}
	
	/* Well, I got it so respond with an ack, or nak */
	/* Send data low to indicate success */
	if (nak)
		GPCR = IIC_DATAOUT;
	else
		GPSR = IIC_DATAOUT;
	i2c_delay_long();

	/* Trigger clock since data is ready */
	GPSR = IIC_CLOCK;
	i2c_delay_long();

	/* Take clock low */
	GPCR = IIC_CLOCK;
	i2c_delay_long();

	/* Release data line */
	GPCR = IIC_DATAOUT;
	i2c_delay_long();

	return byte;
}
	
/*
 * Pulse out a complete byte and receive acknowledge.
 * Returns 0 on success, non-zero on failure.
 */
static int i2c_putbyte(unsigned char byte)
{
	int i, ack_failed;

	/* Clock/data low */
	GPCR = IIC_CLOCK;
	GPSR = IIC_DATAOUT;

	/* Clock out the data */
	for(i = 7; i >= 0; --i)
		i2c_putdatabit((byte >> i) & 1);
	
	/* data high (ie, no drive) */
	GPCR = IIC_DATAOUT;
	i2c_delay_short();
	GPCR = IIC_CLOCK;

	i2c_delay_long();
	
	/* Clock out */
	GPSR = IIC_CLOCK;
	
	/* Wait for ack to arrive */
	i2c_delay_long();

	ack_failed = !(GPLR & IIC_DATAIN);

	i2c_delay_long();
	/* Clock low */
	GPCR = IIC_CLOCK;
	
	i2c_delay_long();

	if (ack_failed) {
		i2c_stopseq();
		udelay(3000);
		printk(KERN_ERR "i2c: Failed to receive ACK for data!\n");
	}

	return ack_failed;
}	

/* Start a new i2c transaction */

static int i2c_start (unsigned char device, int cmd1, int cmd2, int reading)
{
	/* Send start sequence */
	i2c_startseq();

	/* Select the device in write mode */
	if (i2c_putbyte(device & 0xfe))
		goto abort;

	/* Send the command/address bytes */
	if (cmd1 != -1 && i2c_putbyte(cmd1))
		goto abort;
	if (cmd2 != -1 && i2c_putbyte(cmd2))
		goto abort;

	if (reading) {
		/* Repeat the start sequence */
		i2c_stopped = 1;
		i2c_startseq();
	
		/* Select the device but this time in read mode */
		if (i2c_putbyte(device | 0x01))
			goto abort;
	}
	return 0; /* success */
abort:
	printk("i2c_start(%u,%d,%d,%d) failed\n", device, cmd1, cmd2, reading);
	return 1; /* failure */
}

/*
 * This stuff gets called from empeg_audio2.c and friends
 */

/* Write to one or more I2C registers */

int i2c_read(unsigned char device, unsigned short address,
	     unsigned int *data, int count)
{
	int	rc = -1;	// failure

	GRAB_I2C_SEMA;

	/* Select the device */
	if (i2c_start(device, address >> 8, address, 1))
		goto done;

	/* Now read in the actual data */
	while(count--)
	{
		unsigned int msb = 0;
		if (address < 0x200)
			msb = i2c_getbyte(0) << 16;
		*data++ = msb | (i2c_getbyte(0) << 8) | i2c_getbyte(1);
	}

	/* Now say we don't want any more: NAK (send bit 1) */
	i2c_putdatabit(1);

	i2c_stopseq();	
	rc = 0;		// success
 done:
	RELEASE_I2C_SEMA;
	return rc;
}

int i2c_read1(unsigned char device, unsigned short address,
	      unsigned int *data)
{
	return i2c_read(device, address, data, 1);
}

int i2c_write(unsigned char device, unsigned short address,
	      unsigned int *data, unsigned short count)
{
	int	rc = -1;	// failure

	GRAB_I2C_SEMA;

	/* Select the device */
	if (i2c_start(device, address >> 8, address, 0))
		goto i2c_error;

	/* Now send the actual data */
	while(count--)
	{
		if (address < 0x200) {
			/* Send out top byte for a 24-bit quantity */
			if (i2c_putbyte(*data >> 16)) {
				printk("i2c_write: write top byte failed, count:%d\n", count);
				goto i2c_error;
			}
		}
		/* Send out lower 16-bits */
		if (i2c_putbyte(*data >> 8) || i2c_putbyte(*data)) {
			printk("i2c_write: write lower 16-bits failed, count:%d\n", count);
			goto i2c_error;
		}
		++data;
	}
	
	i2c_stopseq();
	rc = 0;		// success

 i2c_error:
	RELEASE_I2C_SEMA;
	return rc;
}

int i2c_write1(unsigned char device, unsigned short address,
	       unsigned int data)
{
	return i2c_write(device, address, &data, 1);
}


int dsp_write(unsigned short address, unsigned int data)
{
#if AUDIO_DEBUG
	printk("DSP_WRITE %x=%x\n",address,data);
#endif
	return(i2c_write1(IICD_DSP,address,data));
}  

int dsp_read_yram(unsigned short address, unsigned int *data)
{
	return i2c_read1(IICD_DSP, address, data);
}

int dsp_read_xram(unsigned short address, unsigned int *data)
{
	int status;
	status = i2c_read1(IICD_DSP, address, data);
	*data &= 0x3FFFF; /* Only eighteen bits of real data */
	return status;
}

int dsp_writemulti(dsp_setup *setup)
{
	int a;
	for(a = 0; setup[a].address != 0; a++) {
		if (dsp_write(setup[a].address, setup[a].data)) {
			printk(KERN_ERR "I2C write failed (%x, %x)\n",
			       setup[a].address, setup[a].data);
			return 1;
		}
	}
	
	return 0;
}

int dsp_patchmulti(dsp_setup *setup, int address, int new_data)
{
	int a;
	for(a = 0; setup[a].address != 0;a++) {
		if (setup[a].address == address) {
			setup[a].data = new_data;
			return 0;
		}
	}	
	return 1;
}

#ifdef CONFIG_EMPEG_I2C_FAN_CONTROL

int i2c_read8 (unsigned char device, unsigned char command, unsigned char *data, int count)
{
	int rc = -1;	// failure

	GRAB_I2C_SEMA;

	if (i2c_start(device, command, -1, 1))
		goto done;

	/* Now read in the actual data */
	while (count--)
		*data++ = i2c_getbyte(1);

	/* Now say we don't want any more: NAK (send bit 1) */
	i2c_putdatabit(1);
	i2c_stopseq();	
	rc = 0;		// success
 done:
	RELEASE_I2C_SEMA;
	return rc;
}

int i2c_write8 (unsigned char device, unsigned char command, unsigned char *data, int count)
{
	int rc = -1;	// failure

	GRAB_I2C_SEMA;

	if (i2c_start(device, command, -1, 0))
		goto done;

	/* Now send the actual data */
	while (count--) {
		if (i2c_putbyte(*data++)) {
			printk("i2c_write8: write byte failed, count:%d\n", count);
			goto done;
		}
	}

	i2c_stopseq();
	rc = 0;
 done:
	RELEASE_I2C_SEMA;
	return rc;
}

#endif // CONFIG_EMPEG_I2C_FAN_CONTROL

