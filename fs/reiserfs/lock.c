/*
 * Copyright 1998 Hans Reiser, see reiserfs/README for licensing and copyright details
 */
#ifdef __KERNEL__

#include <linux/reiserfs_fs.h>
#include <linux/fs.h>
#include <linux/malloc.h>
#include <linux/sched.h>

#else

#include "nokernel.h"

#endif

/* This is a modification of the lock_buffer code that takes an
   arbitrary lock ulong, bitnumber and wait queue pointer rather than
   being buffer_head specific.  (Undoubtedly one of the SMP guys is
   writting something that will replace this but it was not in the
   code that I have seen.)  Note that lock_buffer has buffer specific
   stuff that this does not do and so this cannot replace that code.
   If someone writes bitops for shorts I'll write this for shorts.
   -Hans */

int __wait_on_ulong_lock(unsigned long *, unsigned short, struct  wait_queue **);
extern inline int wait_on_ulong_lock( unsigned long *, unsigned short, struct  wait_queue **);
inline int ulong_lock_locked(unsigned long *, unsigned short);
void unlock_ulong_lock(unsigned long *, unsigned short, struct  wait_queue **);

inline int lock_ulong_lock(unsigned long * lock, unsigned short bitnumber, struct  wait_queue ** l_wait)
{
  int repeat = 0;

  if (test_and_set_bit(bitnumber, lock))
    repeat = __wait_on_ulong_lock(lock, bitnumber, l_wait);
  return repeat;
}

extern inline int wait_on_ulong_lock( unsigned long * lock, unsigned short bitnumber, struct  wait_queue ** l_wait)
{
  if (ulong_lock_locked(lock, bitnumber))
    {
      __wait_on_ulong_lock(lock, bitnumber, l_wait);
      return 1;
    }
  return 0;
}

/* return whether it was able to give you the lock.  Note that it does
   not wait on the lock, and it is used where schedule is a no-no and not
   getting the lock is okay. */
inline int try_ulong_lock(unsigned long * lock, unsigned short bitnumber)
{
  return (!test_and_set_bit(bitnumber, lock));
}

int __wait_on_ulong_lock(unsigned long * lock, unsigned short bitnumber, struct  wait_queue ** l_wait) 
{
  struct wait_queue wait = { current, NULL };
  int repeat = 0;

  add_wait_queue(l_wait, &wait);
loop:
  current->state = TASK_UNINTERRUPTIBLE;
  if (!try_ulong_lock(lock, bitnumber)) {
    current->policy |= SCHED_YIELD;
    schedule();
    repeat = 1;
    goto loop;
  }
  remove_wait_queue(l_wait, &wait);
  current->state = TASK_RUNNING;
  return repeat;
}

inline int ulong_lock_locked(unsigned long * lock, unsigned short bitnumber)
{
  return test_bit(bitnumber, lock);
}

void unlock_ulong_lock(unsigned long * lock, unsigned short bitnumber, struct  wait_queue ** l_wait)
{
  if (test_and_clear_bit(bitnumber, lock))      
    wake_up(l_wait);
}
