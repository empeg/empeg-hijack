/*
 * sa1100 os timers
 *
 * Copyright (c) Lawrence S. Brakmo, 1998
 *
 * For keeping track of who is using which sa1100 os timers.
 *
 * Changelog:
 * 22-01-1998	lsb	Created 
 *
 */

#ifndef ostimers_h
#define ostimers_h

#include <asm/arch/hardware.h>

#define TIMER1_RATE (3686400)

/*
 * OS timer 0 used in time.h
 */

/*
 * OS timer 1 used in keyb_brutus.c to read keyboard, touchscreen and
 * to flash the cursor
 */

extern __inline__ int reset_timer1 (unsigned long delay)
{
  unsigned long next_os_timer_match;
  
  next_os_timer_match = OSCR + delay;
  OSMR1 = next_os_timer_match;
  OSSR = 0x2;

  return (1);
}

extern __inline__ void clear_timer1 (void)
{
  OSSR = 0x2;
}

#if 0
extern __inline__ void setup_timer1 (unsigned long delay)
{
  unsigned long next_os_timer_match;

  next_os_timer_match = OSCR + delay;
  OSMR1 = next_os_timer_match;
  OSSR = 0x2;
  OIER |= 0x2;
}
#else
void setup_timer1 (unsigned long delay)
{
  unsigned long next_os_timer_match;
  static int done = 0;

  if( done ) return;
  done = 1;

  next_os_timer_match = OSCR + delay;
  OSMR1 = next_os_timer_match;
  OSSR = 0x2;
  OIER |= 0x2;
}
#endif

extern __inline__ void cleanup_timer1 (void)
{
  OIER &= ~0x2;
  OSSR = 0x2;
}

#endif
