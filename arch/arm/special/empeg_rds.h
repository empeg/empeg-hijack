/* Header for the RDS device support in the empeg */

#ifndef EMPEG_RDS_H
#define EMPEG_RDS_H

/* There are multiple instances of this structure for the different
   channels provided */
struct rds_dev
{
	/* Receive buffer for this endpoint */
	char *rx_buffer;
	int rx_head;
	int rx_tail;
	int rx_used;
	int rx_free;
	int rx_count;
	
	/* Blocking queue */
	struct wait_queue *rx_wq;

	/* RDS decoder state information */
	int interface;
	int state;
	unsigned int rdsstream;
        int bitsinfifo;
	unsigned char buffer[8];
        int badcrccount;
        int discardpacket;

	int good_packets;
	int recovered_packets;
	int bad_packets;
	int sync_lost_packets;
	int in_use;
};

/* Buffer size */
#define RDS_RX_BUFFER_SIZE          1024

/* Declarations */
static ssize_t rds_read(struct file*,char*,size_t,loff_t*);
static ssize_t rds_write(struct file*,const char*,size_t,loff_t*);
static int rds_ioctl(struct inode*,struct file*,unsigned int,unsigned long);
static int rds_open(struct inode*,struct file*);
static int rds_release(struct inode*,struct file*);
static unsigned int rds_poll(struct file *filp, poll_table *table);

/* External initialisation */
void empeg_rds_init(void);

/* RDS checkword offsets */
#define RDS_OFFSETA	0x0fc
#define RDS_OFFSETB	0x198
#define RDS_OFFSETC	0x168
#define RDS_OFFSETCP	0x350
#define RDS_OFFSETD	0x1b4

#endif

