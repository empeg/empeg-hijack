#ifndef _LINUX_BUFFER_H
#define _LINUX_BUFFER_H

/*
 * This file has definitions for buffer management structures shared between
 * the main buffer cache management and the journaling code.  
 */


#ifdef __KERNEL__

extern struct wait_queue * buffer_wait;

extern int sync_buffers(kdev_t dev, int wait);

extern void end_buffer_io_sync(struct buffer_head *, int);

void put_unused_buffer_head(struct buffer_head * bh);
struct buffer_head * get_unused_buffer_head(int async);

#endif /* __KERNEL__ */

#endif /*_LINUX_BUFFER_H */
