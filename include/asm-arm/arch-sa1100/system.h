/*
 * linux/include/asm-arm/arch-rpc/system.h
 *
 * Copyright (c) 1998 Nicolas Pitre <nico@cam.org>
 */

#ifndef __ASM_ARCH_SYSTEM_H
#define __ASM_ARCH_SYSTEM_H


#ifdef CONFIG_SA1100_VICTOR

#define arch_reset( x ) {						\
	/* switch off power supply */					\
	cli();								\
	mdelay( 2000 );						\
	GPCR = GPIO_GPIO23;	/* power off */			\
	while(1);							\
	}

#else

#define arch_reset( x ) {						\
	cli();								\
	__asm__ volatile (						\
"	mov	r0, #0x130 \n"						\
"	mcr	p15, 0, r0, c1, c0	@ MMU off \n"			\
"       mov     r0, #0\n"						\
"	mcr	p15, 0, r0, c7, c7	@ flush caches \n"		\
"	mov	pc, #0 \n");						\
	}

#endif


/* Enter SA1100 idle mode (see data sheet sec 9.5).
 * It seems that the wait-on-interrupt just hang the CPU forever if it's
 * on the end of a cache line.  Workaround: we force an explicit alignment
 * before it.
 */
#define arch_do_idle() \
	do { \
	__asm__ __volatile__( \
"	mcr	p15, 0, %0, c15, c2, 2	@ Disable clock switching \n" \
"	ldr	%0, [%0]		@ Must perform a non-cached access \n" \
"	b	1f			@ Seems we must align the next \n" \
"	.align 5			@ instruction on a cache line \n" \
"1:	mcr	p15, 0, %0, c15, c8, 2	@ Wait for interrupts \n" \
"	nop				@ to ensure SA1100 re-awakes \n" \
"	mcr	p15, 0, %0, c15, c1, 2	@ Reenable clock switching \n" \
	: : "r" (&ICIP) : "cc" ); \
	} while (0)


#endif
