/* Register & bit definitions for the Philips PDIUSB12 USB device node
   controller */

/* Commands & bitsets */
#define CMD_SETADDRESS		0xd0
 #define SETADDRESS_ADDRESS     0x7f
 #define SETADDRESS_ENABLE      0x80

#define CMD_ENDPOINTENABLE	0xd8
 #define EPENABLE_GENERICISOEN  0x01

#define CMD_SETMODE		0xf3
 #define SETMODE1_NOLAZYCLOCK	0x02
 #define SETMODE1_CLOCKRUNNING	0x04
 #define SETMODE1_IRQMODE	0x08
 #define SETMODE1_SOFTCONNECT	0x10
 #define SETMODE1_NONISO        0x00
 #define SETMODE1_ISOOUT        0x40
 #define SETMODE1_ISOIN         0x80
 #define SETMODE1_ISOIO         0xc0
 #define SETMODE2_CLOCKDIV      0x0f
 #define SETMODE2_SETTOONE      0x40
 #define SETMODE2_SOFONLYIRQ    0x80

#define CMD_SETDMA		0xfb
 #define SETDMA_BURST1		0x00
 #define SETDMA_BURST4		0x01
 #define SETDMA_BURST8		0x02
 #define SETDMA_BURST16		0x03
 #define SETDMA_ENABLE		0x04
 #define SETDMA_DIRECTION       0x08
 #define SETDMA_AUTORELOAD      0x10
 #define SETDMA_IRQPINMODE      0x20
 #define SETDMA_EP4IRQENABLE    0x40
 #define SETDMA_EP5IRQENABLE    0x80

#define CMD_READINTERRUPT       0xf4
 #define IRQ1_CONTROLOUT	0x01
 #define IRQ1_CONTROLIN         0x02
 #define IRQ1_EP1OUT            0x04
 #define IRQ1_EP1IN             0x08
 #define IRQ1_MAINOUT           0x10
 #define IRQ1_MAININ            0x20
 #define IRQ1_BUSRESET          0x40
 #define IRQ1_SUSPENDCHANGE     0x80
 #define IRQ2_DMAEOT            0x01

#define CMD_SELECTEP0           0x00
#define CMD_SELECTEP1		0x01
#define CMD_SELECTEP2		0x02
#define CMD_SELECTEP3		0x03
#define CMD_SELECTEP4		0x04
#define CMD_SELECTEP5		0x05
 #define SELECTEP_FULL		0x01
 #define SELECTEP_STALL         0x02

#define CMD_LASTTRANSACTION0    0x40
#define CMD_LASTTRANSACTION1    0x41
#define CMD_LASTTRANSACTION2    0x42
#define CMD_LASTTRANSACTION3    0x43
#define CMD_LASTTRANSACTION4    0x44
#define CMD_LASTTRANSACTION5    0x45
 #define LASTTRANS_SUCCESS      0x01
 #define LASTTRANS_ERRCODE      0x1e
 #define LASTTRANS_SETUP        0x20
 #define LASTTRANS_DATA1        0x40
 #define LASTTRANS_STATNOTREAD  0x80

#define CMD_SETEPSTATUS0	0x40
#define CMD_SETEPSTATUS1	0x41
#define CMD_SETEPSTATUS2	0x42
#define CMD_SETEPSTATUS3	0x43
#define CMD_SETEPSTATUS4	0x44
#define CMD_SETEPSTATUS5	0x45
 #define SETEPSTATUS_STALLED    0x01

#define CMD_READBUFFER          0xf0
#define CMD_WRITEBUFFER 	0xf0
#define CMD_CLEARBUFFER		0xf2
#define CMD_VALIDATEBUFFER	0xfa
#define CMD_ACKSETUP            0xf1

#define CMD_SENDRESUME		0xf6
#define CMD_READFRAMENUMBER     0xf5
#define CMD_READCHIPID          0xfd

