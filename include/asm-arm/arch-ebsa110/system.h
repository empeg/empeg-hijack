/*
 * linux/include/asm-arm/arch-ebsa110/system.h
 *
 * Copyright (C) 1996-1999 Russell King.
 */
#ifndef __ASM_ARCH_SYSTEM_H
#define __ASM_ARCH_SYSTEM_H

#define arch_do_idle()		processor._do_idle()

extern __inline__ void arch_reset(char mode)
{
	/*
	 * loop endlessly
	 */
	cli();
	while(1);
}

#endif
