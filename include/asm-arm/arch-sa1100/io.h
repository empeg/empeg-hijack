/*
 * linux/include/asm-arm/arch-brutus/io.h
 *
 * Copyright (C) 1997 Russell King
 *
 * Adapted for SA1100 (C) 1998 Nicolas Pitre <nico@cam.org>
 */
#ifndef __ASM_ARM_ARCH_IO_H
#define __ASM_ARM_ARCH_IO_H


/*
 * This architecture does not require any delayed IO.
 */
#undef	ARCH_IO_DELAY


/* These are I/O macros intended to be used over the PCMCIA I/O bus
 * of the SA-1100.  This interface acts more or less like an ISA bus
 * and addresses should not be leftshifted like on other ARM architectures. 
 * ldrh/strh must be used for 16 bits transfers.
 */

#if 1
/* These are the safe version of the macros */

#define __outb(value,port)						\
({									\
	__asm__ __volatile__(						\
		"str%?b	%0, [%1, %2]"					\
		: : "r" (value), "r" (PCIO_BASE), "r" (port));		\
})

#define __inb(port)							\
({									\
	unsigned char result;						\
	__asm__ __volatile__(						\
		"ldr%?b	%0, [%1, %2]"					\
		: "=r" (result) : "r" (PCIO_BASE), "r" (port));		\
	result;								\
})

#define __outw(value,port)						\
({									\
	__asm__ __volatile__(						\
		"str%?h	%0, [%1, %2]"					\
		: : "r" (value), "r" (PCIO_BASE), "r" (port));		\
})

#define __inw(port)							\
({									\
	unsigned short result;						\
	__asm__ __volatile__(						\
		"ldr%?h	%0, [%1, %2]"					\
		: "=r" (result) : "r" (PCIO_BASE), "r" (port));		\
	result;								\
})

#define __outl(value,port)						\
({									\
	__asm__ __volatile__(						\
		"str%?	%0, [%1, %2]"					\
		: : "r" (value), "r" (PCIO_BASE), "r" (port));		\
})

#define __inl(port)							\
({									\
	unsigned long result;						\
	__asm__ __volatile__(						\
		"ldr%?	%0, [%1, %2]"					\
		: "=r" (result) : "r" (PCIO_BASE), "r" (port));		\
	result;								\
})

#else	/* if 0 */
/* These ones let the compiler optimize a little more than the above.
 * However the -mcpu=strongarm110 compiler switch must be enabled in order
 * for this to work properly.
 */

#define __inb(port) (*((volatile unsigned char*)(PCIO_BASE+(port))))
#define __inw(port) (*((volatile unsigned short*)(PCIO_BASE+(port))))
#define __inl(port) (*((volatile unsigned long*)(PCIO_BASE+(port))))
#define __outb(value,port) *((volatile unsigned char*)(PCIO_BASE+(port))) = (value)
#define __outw(value,port) *((volatile unsigned short*)(PCIO_BASE+(port))) = (value)
#define __outl(value,port) *((volatile unsigned long*)(PCIO_BASE+(port))) = (value)

#endif


#define __ioaddrc(port)								\
({										\
	unsigned long addr;							\
	addr = PCIO_BASE + (port);					\
	addr;									\
})


/*
 * Translated address IO functions
 *
 * IO address has already been translated to a virtual address
 */
#define outb_t(v,p)								\
	(*(volatile unsigned char *)(p) = (v))

#define inb_t(p)								\
	(*(volatile unsigned char *)(p))

#define outl_t(v,p)								\
	(*(volatile unsigned long *)(p) = (v))

#define inl_t(p)								\
	(*(volatile unsigned long *)(p))

#endif
