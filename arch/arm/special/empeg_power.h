/* Header for the power-pic device support in the empeg */

#ifndef EMPEG_POWER_H
#define EMPEG_POWER_H

/* There are multiple instances of this structure for the different
   channels provided */
struct power_dev
{
	time_t alarmtime;           /* Current alarm time */
	struct wait_queue *wq;      /* Wait queue for poll */
        struct tq_struct poller;    /* Checks the power state */
	int laststate;              /* Last power state */
	int newstate;               /* New power state */
	int displaystate;	    /* Is display on or off? */
};

/* Declarations */
static int power_ioctl(struct inode*,struct file*,unsigned int,unsigned long);
static int power_open(struct inode*,struct file*);
static int power_release(struct inode*,struct file*);
static unsigned int power_poll(struct file *flip, poll_table *wait);

/* External initialisation */
void empeg_power_init(void);

#endif

