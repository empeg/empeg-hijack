/* Header file for Empeg USB-Ethernet emulation */

/* Version	Date		Comment				*/
/*   1		6Jan03		First Version			*/


#ifndef EMPEG_PEGASUS_H
#define EMPEG_PEGASUS_H

#ifdef BUILD_EMPEG_PEGASUS_C

/* Registers Used */
#define EMPEG_PEGASUS_ETH_CONTROL 	0x00
#define EMPEG_PEGASUS_ETH_CONTROL1	0x01
#define EMPEG_PEGASUS_ETH_CONTROL2	0x02
#define EMPEG_PEGASUS_MULTICAST_TBL	0x08
#define EMPEG_PEGASUS_ETH_ID		0x10
#define EMPEG_PEGASUS_PHY_ADDR		0x25
#define EMPEG_PEGASUS_PHY_DATA		0x26
#define EMPEG_PEGASUS_WAKEUP_STAT 	0x7A
#define EMPEG_PEGASUS_PHY_CTRL		0x7B
#define EMPEG_PEGASUS_GPIO_1_0		0x7E
#define EMPEG_PEGASUS_GPIO_3_2		0x7F

#define PEGASUS_SET_REGISTER		0x40
#define PEGASUS_GET_REGISTER		0xC0


typedef struct Empeg_USBEth
{
	unsigned char EthCtrl[3];
	unsigned char MulticastTbl[8];
	unsigned char EthId[6];
	unsigned char PhyReg[4];
	unsigned char WakeupStat[8];

}EMPEG_PEGASUS;

#endif

/* Fcn Declarations */
void init_pegasus(void);
unsigned char Pegasus_HandleCtrl(unsigned char * Data);

/* External Data */
extern unsigned char PegasusData[];
extern unsigned char PegasusLen;

#endif
