/*#define DEBUG_USB */

/* Source file for Empeg USB-Ethernet emulation */

/* Version	Date		Comment				*/
/*   1		6Jan03		First Version			*/


#include <linux/config.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/major.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/init.h>

#include <asm/byteorder.h>
#include <asm/irq.h>
#include <asm/fiq.h>
#include <asm/segment.h>
#include <asm/io.h>
#include <asm/hardware.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>

#define BUILD_EMPEG_PEGASUS_C
#include "empeg_pegasus.h"

/*#include "empeg_usb.h"
*/

#define MII_READ	0x00
#define MII_WRITE	0x01
#define MII_SWREAD	0x40


EMPEG_PEGASUS UsbEth;
unsigned char Default_EthCtrl[] = {0x09, 0x00, 0x00};
unsigned char Default_MulticastTbl[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
unsigned char Default_EthId[] = {0x00, 0x09, 0x5b, 0x03, 0x7b, 0xf0};
unsigned char Default_PhyReg[] = {0x00, 0x00, 0x00, 0x00};
unsigned char Default_WakeupStat[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};

unsigned char PegasusData[8];
unsigned char PegasusLen;

void init_pegasus(void)
{
	memcpy(UsbEth.EthCtrl, Default_EthCtrl, 3);
	memcpy(UsbEth.MulticastTbl, Default_MulticastTbl, 8);
	memcpy(UsbEth.EthId, Default_EthId, 6);
	memcpy(UsbEth.PhyReg, Default_PhyReg, 4);
	memcpy(UsbEth.WakeupStat, Default_WakeupStat, 8);

}

unsigned char Pegasus_MIIReg(unsigned char read_write, unsigned char * data, unsigned char start, unsigned char length)
{
	unsigned char ret_length = 0;
	unsigned char base_offset = 0x00;
	
	if(start == 0x25)
	{
		base_offset = 0x01;
	}

	if(read_write == MII_READ)
	{
		

	}
	else
	{
		/* Write, so work out what is happening and set the regs */
		if(data[base_offset+2] & MII_SWREAD)
		{
			if((data[base_offset+2] & 0x0F) == 1)
			{
#ifdef DEBUG_USB
				printk("Req read of MII reg1\n");
#endif
				UsbEth.PhyReg[1] = 0x4D;
				UsbEth.PhyReg[2] = 0x78;
				UsbEth.PhyReg[3] = 0xc1;

			}
		}

	}
	return ret_length;
} 

unsigned char Pegasus_HandleCtrl(unsigned char * Data)
{
	static int reg = 0;
	static int length = 0;

	unsigned char DataReq = 0;
	
	if(reg || length)
	{
		switch(reg)
		{
		case EMPEG_PEGASUS_ETH_CONTROL:
			memcpy(UsbEth.EthCtrl, Data, length);
			break;

		case EMPEG_PEGASUS_ETH_CONTROL1:
			memcpy(UsbEth.EthCtrl+1, Data, length);
			break;

		case EMPEG_PEGASUS_ETH_CONTROL2:
			memcpy(UsbEth.EthCtrl+2, Data, length);
			break;

		case EMPEG_PEGASUS_MULTICAST_TBL:
			memcpy(UsbEth.MulticastTbl, Data, length);
			break;

		case EMPEG_PEGASUS_ETH_ID:
			memcpy(UsbEth.EthId, Data, length);
			break;

		case EMPEG_PEGASUS_PHY_ADDR:
			Pegasus_MIIReg(MII_WRITE, Data, reg, length);
			UsbEth.PhyReg[3] |= 0x81;
			break;

		case EMPEG_PEGASUS_PHY_DATA:
			memcpy(UsbEth.PhyReg+1, Data, length);
			UsbEth.PhyReg[3] |= 0x81;
			break;

		case EMPEG_PEGASUS_WAKEUP_STAT:
			memcpy(UsbEth.WakeupStat, Data, length);
			break;

		case EMPEG_PEGASUS_PHY_CTRL:
			memcpy(UsbEth.WakeupStat+1, Data, length);
			break;

		case EMPEG_PEGASUS_GPIO_1_0:
			memcpy(UsbEth.WakeupStat+4, Data, length);
			break;

		case EMPEG_PEGASUS_GPIO_3_2:
			memcpy(UsbEth.WakeupStat+5, Data, length);
			break;
		}

		/* Force some fields */
		

		DataReq = 0xFF;
		reg = 0;
		length = 0;
	}
	else if(Data[0] == PEGASUS_GET_REGISTER)
	{
		reg = Data[4] + (Data[5] *256);
		length = Data[6] + (Data[7] *256);
		
		switch(reg)
		{
		case EMPEG_PEGASUS_ETH_CONTROL:
			memcpy(PegasusData, UsbEth.EthCtrl, length);
			PegasusLen = length;
			break;

		case EMPEG_PEGASUS_ETH_CONTROL1:
			memcpy(PegasusData, UsbEth.EthCtrl+1, length);
			PegasusLen = length;
			break;

		case EMPEG_PEGASUS_ETH_CONTROL2:
			memcpy(PegasusData, UsbEth.EthCtrl+2, length);
			PegasusLen = length;
			break;

		case EMPEG_PEGASUS_MULTICAST_TBL:
			memcpy(PegasusData, UsbEth.MulticastTbl, length);
			PegasusLen = length;
			break;

		case EMPEG_PEGASUS_ETH_ID:
			memcpy(PegasusData, UsbEth.EthId, length);
			PegasusLen = length;
			break;

		case EMPEG_PEGASUS_PHY_ADDR:
			memcpy(PegasusData, UsbEth.PhyReg, length);
			PegasusLen = length;
			break;

		case EMPEG_PEGASUS_PHY_DATA:
			memcpy(PegasusData, UsbEth.PhyReg+1, length);
			PegasusLen = length;
			break;

		case EMPEG_PEGASUS_WAKEUP_STAT:
			memcpy(PegasusData, UsbEth.WakeupStat, length);
			PegasusLen = length;
			break;

		case EMPEG_PEGASUS_PHY_CTRL:
			memcpy(PegasusData, UsbEth.WakeupStat+1, length);
			PegasusLen = length;
			break;

		case EMPEG_PEGASUS_GPIO_1_0:
			memcpy(PegasusData, UsbEth.WakeupStat+4, length);
			PegasusLen = length;
			break;

		case EMPEG_PEGASUS_GPIO_3_2:
			memcpy(PegasusData, UsbEth.WakeupStat+5, length);
			PegasusLen = length;
			break;

		default:
			PegasusLen = 0;
		}

		reg = 0;
		length = 0;
	}
	else if(Data[0] == PEGASUS_SET_REGISTER)
	{
		reg = Data[4] + (Data[5] *256);
		length = Data[6] + (Data[7] *256);
		DataReq = 0xFF;
	}

	return DataReq;
}

unsigned char * PegasusAddMAC(unsigned char * data)
{
	memcpy(&data[6], UsbEth.EthId, 6);

	return data;
}
