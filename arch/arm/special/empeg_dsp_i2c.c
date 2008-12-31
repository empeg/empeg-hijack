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

static void i2c_startseq(void);
static void i2c_stopseq(void);
static int i2c_getdatabit(void);
static void i2c_putdatabit(int bit);
static int i2c_getbyte(unsigned char *byte, int nak);
static int i2c_putbyte(int byte);

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

/* Pulse out the start sequence */

static void i2c_startseq(void)
{
	/* Clock low, data high */
	GPCR = IIC_CLOCK | IIC_DATAOUT;
	i2c_delay_long();

	/* Put clock high */
	GPSR = IIC_CLOCK;
	i2c_delay_short();

	/* Put data low */
	GPSR = IIC_DATAOUT;

/* Ben Kamen (i2c fan controller dude) says this step is not needed: */
#if 0
	i2c_delay_short();

	/* Clock low again */
	GPCR = IIC_CLOCK;
#endif
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
	GPCR = IIC_CLOCK;
	old_bit = !(GPLR & IIC_DATAOUT);
	if (bit != old_bit) {
		if (bit)
			GPCR = IIC_DATAOUT;
		else
			GPSR = IIC_DATAOUT;
		i2c_delay_long();
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

static int i2c_getbyte(unsigned char *byte, int nak)
{
	int i;

	/* Let data line float */
	GPCR = IIC_DATAOUT;
	i2c_delay_long();

	*byte = 0;
	/* Clock in the data */
	for(i = 7; i >= 0; --i)
		if (i2c_getdatabit())
			(*byte) |= (1 << i);
	
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

	return 0; /* success */
}
	
/*
 * Pulse out a complete byte and receive acknowledge.
 * Returns 0 on success, non-zero on failure.
 */
static int i2c_putbyte(int byte)
{
	int i, ack;

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

	ack = !(GPLR & IIC_DATAIN);

	i2c_delay_long();
	/* Clock low */
	GPCR = IIC_CLOCK;
	
	i2c_delay_long();

	if (ack) {
		i2c_stopseq();
		udelay(3000);
		printk(KERN_ERR "i2c: Failed to receive ACK for data!\n");
	}

	return ack;
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

	/* Send start sequence */
	i2c_startseq();

	/* Set the device */
	if (i2c_putbyte(device & 0xFE))
		goto i2c_error;

	/* Set the address (higher then lower) */
	if (i2c_putbyte(address >> 8) || i2c_putbyte(address & 0xFF))
		goto i2c_error;

	/* Repeat the start sequence */
	i2c_startseq();
	
	/* Set the device but this time in read mode */
	if (i2c_putbyte(device | 0x01))
		goto i2c_error;

	/* Now read in the actual data */
	while(count--)
	{
		unsigned char b1, b2, b3;
		if(address < 0x200) {
			if (i2c_getbyte(&b1, 0) ||
			    i2c_getbyte(&b2, 0) ||
			    i2c_getbyte(&b3, 1))
				goto i2c_error;
			*data++ = (b1 << 16) | (b2 << 8) | b3;
		} else {
			/* Receive the 16 bit quantity */
			if (i2c_getbyte(&b1, 0) ||
			    i2c_getbyte(&b2, 1))
				goto i2c_error;
			*data++ = (b1 << 8) | b2;
		}
	}

	/* Now say we don't want any more: NAK (send bit 1) */
	i2c_putdatabit(1);

	i2c_stopseq();	
	rc = 0;		// success

 i2c_error:
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

	/* Pulse out the start sequence */
	i2c_startseq();

	/* Say who we're talking to */
	if (i2c_putbyte(device & 0xFE)) {
		printk("i2c_write: device select failed\n");
		goto i2c_error;
	}

	/* Set the address (higher then lower) */
	if (i2c_putbyte(address >> 8) || i2c_putbyte(address & 0xFF)) {
		printk("i2c_write: address select failed\n");
		goto i2c_error;
	}

	/* Now send the actual data */
	while(count--)
	{
		if (address < 0x200) {
			/* Send out the 24 bit quantity */
			
			/* Mask off the top 8 bits in certain situations! */
			if (i2c_putbyte((*data >> 16) & 0xff)) {
				printk("i2c_write: write first byte failed"
				       ", count:%d\n", count);
				goto i2c_error;
			}
			if (i2c_putbyte((*data >> 8) & 0xFF)) {
				printk("i2c_write: write second byte failed"
				       ", count:%d\n", count);
				goto i2c_error;
			}
			if (i2c_putbyte(*data & 0xFF)) {
				printk("i2c_write: write third byte failed"
				       ", count:%d\n", count);
				goto i2c_error;
			}
		}
		else {
			/* Send out 16 bit quantity */
			/* Mask off the top 8 bits in certain situations! */
			if (i2c_putbyte(*data >> 8)) {
				printk("i2c_write: write first byte failed"
				       ", count:%d\n", count);
				goto i2c_error;
			}
			if (i2c_putbyte(*data & 0xFF)) {
				printk("i2c_write: write second byte failed"
				       ", count:%d\n", count);
				goto i2c_error;
			}
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
	int	rc = -1;	// failure

	GRAB_I2C_SEMA;

	/* Send start sequence */
	i2c_startseq();

	/* Set the device */
	if (i2c_putbyte(device & 0xFE))
		goto done;

	/* Set the command */
	if (i2c_putbyte(command))
		goto done;

	/* Repeat the start sequence */
	i2c_startseq();
	
	/* Set the device but this time in read mode */
	if (i2c_putbyte(device | 0x01))
		goto done;

	/* Now read in the actual data */
	while (count--) {
		if (i2c_getbyte(data, 1))
			goto done;
		++data;
	}

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
	int	rc = -1;	// failure

	GRAB_I2C_SEMA;

	/* Pulse out the start sequence */
	i2c_startseq();

	/* Say who we're talking to */
	if (i2c_putbyte(device & 0xFE)) {
		printk("i2c_write8: device select failed\n");
		goto done;
	}

	/* Set the command */
	if (i2c_putbyte(command)) {
		printk("i2c_write8: command issue failed\n");
		goto done;
	}

	/* Now send the actual data */
	while (count--) {
		if (i2c_putbyte(*data)) {
			printk("i2c_write8: write byte failed, count:%d\n", count);
			goto done;
		}
		++data;
	}

	i2c_stopseq();
	rc = 0;
 done:
	RELEASE_I2C_SEMA;
	return rc;
}

#endif // CONFIG_EMPEG_I2C_FAN_CONTROL

