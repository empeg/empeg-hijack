/*
 * linux/include/asm-arm/arch-brutus/hardware.h
 *
 * Copyright (C) 1998 Nicolas Pitre <nico@cam.org>
 *
 * This file contains the hardware definitions for SA1100 architecture
 */

#ifndef __ASM_ARCH_HARDWARE_H
#define __ASM_ARCH_HARDWARE_H

/*
 * RAM definitions
 */
#define KERNTOPHYS(a)           ((unsigned long)(&a))

/* Flushing areas */
#define FLUSH_BASE_PHYS		0xe0000000	/* SA1100 zero bank */
#define FLUSH_BASE		0xdf000000
#define FLUSH_BASE_MINICACHE	0xdf800000

/*
 * SA1100 internal I/O mappings
 *
 * We have the following mapping:
 * 	phys		virt
 * 	80000000	f8000000
 * 	90000000	fa000000
 * 	a0000000	fc000000
 * 	b0000000	fe000000
 *
 * Nb: PCMCIA is mapped from 0xe0000000 to f7ffffff in mm-sa1100.c
 *
 */

/* virtual start of IO space */
#define VIO_BASE	0xf8000000

/* x = IO space shrink power i.e. size = space/(2^x) */
#define VIO_SHIFT	3

/* virtual end of IO space */
#define VIO_END		0xffffffff	
#define VIO_SIZE	(IO_END - IO_BASE)

/* physical start of IO space */
#define PIO_START	0x80000000

#define io_p2v( x ) 		\
   ( (((x)&0x00ffffff) | (((x)&0x30000000)>>VIO_SHIFT)) + VIO_BASE )

#define io_v2p( x ) 		\
   ( (((x)&0x00ffffff) | (((x)&(0x30000000>>VIO_SHIFT))<<VIO_SHIFT)) + PIO_START )


#ifdef CONFIG_SA1101

/* 
 * We have mapped the sa1101 depending on the value of SA1101_BASE.
 * It then appears from 0xdc000000.
 */

#define SA1101_p2v( x )		((x) - SA1101_BASE + 0xdc000000)

#define SA1101_v2p( x )		((x) - 0xdc000000 + SA1101_BASE)

#endif


#ifdef __ASSEMBLY__
#define LANGUAGE Assembly
#else
#define LANGUAGE C
#endif

/*
 * SA1100 internal IO definitions
 */

#include "SA-1100.h"

#ifdef CONFIG_SA1101
/* We also have a sa1101 to take care of */
#include "SA-1101.h"
#endif


/*
 * PCMCIA IO is mapped to 0xe0000000.  We are likely to use in*()/out*()
 * IO macros for what might appear there...
 * The SA1100 PCMCIA interface can be seen like a PC ISA bus for IO.
 */

#define PCIO_BASE	0xe0000000	/* PCMCIA0 IO space */
/*#define PCIO_BASE	0xe4000000*/	/* PCMCIA1 IO space */

/* Include specific empeg hardware definitions */
#ifdef CONFIG_SA1100_EMPEG
#include "empegcar.h"
#endif

#endif
