/* USB header for the USB device support in the empeg */
#ifndef EMPEG_USB_H
#define EMPEG_USB_H

/* USB device: one in, one out, and various stats */
struct usb_dev
{
	/* Transmit buffer for this endpoint */
	char *tx_buffer;
	int tx_head;
	int tx_tail;
	int tx_used;
	int tx_free;
	int tx_count;
	
	/* Receive buffer for this endpoint */
	char *rx_buffer;
	int rx_head;
	int rx_tail;
	int rx_used;
	int rx_free;
	int rx_count;
	
	/* Blocking queue */
	struct wait_queue *rx_wq;
	struct wait_queue *tx_wq;
	
	/* Stats */
	int stats_ok[4],stats_err[4],stats_nak[4],stats_warn[4],stats_overrun[4];
};

/* Buffer sizes */
#define USB_TX_BUFFER_SIZE          16384
#define USB_RX_BUFFER_SIZE          65536

#define USB_DEFAULT                 0    /* No descriptor read */
#define USB_DESCRIPTOR_READ         1    /* Descriptor read,
					    Addr not assigned */
#define USB_ADDRESS_ASSIGNED        2    /* Descriptor Read,
					    Addr assigned*/
#define USB_CONFIGURED              3

/* GLOBAL STATUS VALUES */
#define STD_COMMAND                 0x00
#define SETUP_COMMAND_PHASE         0x40
#define FUNCTION_ERROR              0x7F  /* Used when we are stalling the
					     function EP0 */
#define HUB_ERROR                   0xFF  /* Used when we are stalling the
					     HUB EP0 */

/*
 * bRequest Values
 */
#define GET_STATUS                  0
#define CLEAR_FEATURE               1
#define SET_FEATURE                 3
#define SET_ADDRESS                 5
#define GET_DESCRIPTOR              6
#define SET_DESCRIPTOR              7
#define GET_CONFIGURATION           8
#define SET_CONFIGURATION           9
#define GET_INTERFACE               10
#define SET_INTERFACE               11
#define SYNCH_FRAME                 12
#define REQ_DONE                    0xFF    /*private code: request done*/

/*
 * Descriptor Types
 */

#define DEVICE                      1
#define CONFIGURATION               2
#define XSTRING                     3
#define INTERFACE                   4
#define ENDPOINT                    5

/*
 * Recipient Selectors/Masks
 */

#define RECIPIENT_MASK              0x1F
#define DEVICE_RECIPIENT            0
#define INTERFACE_RECIPIENT         1
#define ENDPOINT_RECIPIENT          2
#define OTHER_RECIPIENT             3

/*
 * Feature Selectors
 */

#define         DEVICE_REMOTE_WAKEUP   0x01
#define         ENDPOINT_STALL         0x00

/* Declarations */
static ssize_t usb_read(struct file*,char*,size_t,loff_t*);
static ssize_t usb_write(struct file*,const char*,size_t,loff_t*);
static int usb_ioctl(struct inode*,struct file*,unsigned int,unsigned long);
static int usb_open(struct inode*,struct file*);
static int usb_release(struct inode*,struct file*);
static unsigned int usb_poll(struct file *filp, poll_table *table);

/* External initialisation */
void empeg_usb_init(void);

#define MAXIMUM_USB_STRING_LENGTH 255

// values for the bits returned by the USB GET_STATUS command
#define USB_GETSTATUS_SELF_POWERED                0x01
#define USB_GETSTATUS_REMOTE_WAKEUP_ENABLED       0x02


#define USB_DEVICE_DESCRIPTOR_TYPE                0x01
#define USB_CONFIGURATION_DESCRIPTOR_TYPE         0x02
#define USB_STRING_DESCRIPTOR_TYPE                0x03
#define USB_INTERFACE_DESCRIPTOR_TYPE             0x04
#define USB_ENDPOINT_DESCRIPTOR_TYPE              0x05
#define USB_POWER_DESCRIPTOR_TYPE                 0x06

#define USB_DESCRIPTOR_MAKE_TYPE_AND_INDEX(d, i) ((USHORT)((USHORT)d<<8 | i))

//
// Values for bmAttributes field of an
// endpoint descriptor
//

#define USB_ENDPOINT_TYPE_MASK                    0x03

#define USB_ENDPOINT_TYPE_CONTROL                 0x00
#define USB_ENDPOINT_TYPE_ISOCHRONOUS             0x01
#define USB_ENDPOINT_TYPE_BULK                    0x02
#define USB_ENDPOINT_TYPE_INTERRUPT               0x03


//
// definitions for bits in the bmAttributes field of a 
// configuration descriptor.
//
#define USB_CONFIG_POWERED_MASK                   0xc0

#define USB_CONFIG_BUS_POWERED                    0x80
#define USB_CONFIG_SELF_POWERED                   0x40
#define USB_CONFIG_REMOTE_WAKEUP                  0x20

//
// Endpoint direction bit, stored in address
//

#define USB_ENDPOINT_DIRECTION_MASK               0x80

// test direction bit in the bEndpointAddress field of
// an endpoint descriptor.
#define USB_ENDPOINT_DIRECTION_OUT(addr)          (!((addr) & USB_ENDPOINT_DIRECTION_MASK))
#define USB_ENDPOINT_DIRECTION_IN(addr)           ((addr) & USB_ENDPOINT_DIRECTION_MASK)

//
// USB defined request codes
// see chapter 9 of the USB 1.0 specifcation for
// more information.
//

// These are the correct values based on the USB 1.0
// specification

#define USB_REQUEST_GET_STATUS                    0x00
#define USB_REQUEST_CLEAR_FEATURE                 0x01

#define USB_REQUEST_SET_FEATURE                   0x03

#define USB_REQUEST_SET_ADDRESS                   0x05
#define USB_REQUEST_GET_DESCRIPTOR                0x06
#define USB_REQUEST_SET_DESCRIPTOR                0x07
#define USB_REQUEST_GET_CONFIGURATION             0x08
#define USB_REQUEST_SET_CONFIGURATION             0x09
#define USB_REQUEST_GET_INTERFACE                 0x0A
#define USB_REQUEST_SET_INTERFACE                 0x0B
#define USB_REQUEST_SYNC_FRAME                    0x0C


//
// defined USB device classes
//


#define USB_DEVICE_CLASS_RESERVED           0x00
#define USB_DEVICE_CLASS_AUDIO              0x01
#define USB_DEVICE_CLASS_COMMUNICATIONS     0x02
#define USB_DEVICE_CLASS_HUMAN_INTERFACE    0x03
#define USB_DEVICE_CLASS_MONITOR            0x04
#define USB_DEVICE_CLASS_PHYSICAL_INTERFACE 0x05
#define USB_DEVICE_CLASS_POWER              0x06
#define USB_DEVICE_CLASS_PRINTER            0x07
#define USB_DEVICE_CLASS_STORAGE            0x08
#define USB_DEVICE_CLASS_HUB                0x09
#define USB_DEVICE_CLASS_VENDOR_SPECIFIC    0xFF

//
// USB defined Feature selectors
//

#define USB_FEATURE_ENDPOINT_STALL          0x0000
#define USB_FEATURE_REMOTE_WAKEUP           0x0001
#define USB_FEATURE_POWER_D0                0x0002
#define USB_FEATURE_POWER_D1                0x0003
#define USB_FEATURE_POWER_D2                0x0004
#define USB_FEATURE_POWER_D3                0x0005

#define BUS_POWERED                           0x80
#define SELF_POWERED                          0x40
#define REMOTE_WAKEUP                         0x20

//
// USB power descriptor added to core specification
//

#define USB_SUPPORT_D0_COMMAND      0x01
#define USB_SUPPORT_D1_COMMAND      0x02
#define USB_SUPPORT_D2_COMMAND      0x04
#define USB_SUPPORT_D3_COMMAND      0x08

#define USB_SUPPORT_D1_WAKEUP       0x10
#define USB_SUPPORT_D2_WAKEUP       0x20

#endif

