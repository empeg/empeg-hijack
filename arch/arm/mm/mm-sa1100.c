/*
 * arch/arm/mm/mm-sa1100.c
 *
 * Extra MM routines for the SA1100 architecture
 *
 * Copyright (C) 1998 Russell King
 * Copyright (C) 1999 Hugo Fiennes
 * 
 * 1999/09/12 Nicolas Pitre <nico@cam.org>
 *	Specific RAM implementation details are in 
 *	linux/include/asm/arch-sa1100/me mory.h now.
 *	Allows for better macro optimisations when possible.
 */

#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/init.h>

#include <asm/pgtable.h>
#include <asm/page.h>
#include <asm/io.h>
#include <asm/proc/mm-init.h>

#ifdef CONFIG_SA1100_EMPEG
/* default mapping is 4 banks of 4MB */
unsigned long empeg_virt_to_phys_mapping[16] = {
	0xc0000000,
	0xc8000000,
	0xd0000000,
	0xd8000000,
	0xc0000000,	// phantom
	0xc8000000,	// phantom
	0xd0000000,	// phantom
	0xd8000000,	// phantom
	0xc0000000,	// phantom
	0xc8000000,	// phantom
	0xd0000000,	// phantom
	0xd8000000,	// phantom
	0xc0000000,	// phantom
	0xc8000000,	// phantom
	0xd0000000,	// phantom
	0xd8000000	// phantom
};
unsigned long empeg_virt_to_phys_mapping_mk2a[16] = {
	0xc0000000,
	0xc0400000,
	0xc0800000,
	0xc0c00000,
	0xc8000000,
	0xc8400000,
	0xc8800000,
	0xc8c00000,
	0xd0000000,
	0xd0400000,
	0xd0800000,
	0xd0c00000,
	0xd8000000,
	0xd8400000,
	0xd8800000,
	0xd8c00000
};
unsigned long empeg_phys_to_virt_mapping[4] = {
	0xc0000000,
	0xc0400000,
	0xc0800000,
	0xc0c00000
};
#endif

#ifdef CONFIG_SA1100_VICTOR
#define FLASH_MAPPING \
	{ 0xd0000000, 0x00000000, 0x00200000, DOMAIN_IO, 1, 1 }, /* flash */
#elif defined(CONFIG_SA1100_EMPEG)
#include <linux/empeg.h>
#define FLASH_MAPPING \
        { EMPEG_FLASHBASE, 0x00000000, 0x00200000, DOMAIN_IO, 1, 1 }, /* flash */
#elif defined(CONFIG_SA1100_THINCLIENT)
#define FLASH_MAPPING \
        { 0xdc000000, 0x10000000, 0x00400000, DOMAIN_IO, 1, 1 }, /* CPLD */
#elif defined(CONFIG_SA1100_TIFON) /* We want continuous flash memory */
#define FLASH_MAPPING\
        { 0xd0000000, 0x00000000, 0x00800000, DOMAIN_IO, 1, 1 }, /* flash bank 1 */\
        { 0xd0800000, 0x08000000, 0x00800000, DOMAIN_IO, 1, 1 }, /* flash bank 2 */
#else
#define FLASH_MAPPING
#endif


#ifdef CONFIG_SA1101
#define SA1101_MAPPING \
        { 0xdc000000, SA1101_BASE, 0x00400000, DOMAIN_IO, 1, 1 },
#else
#define SA1101_MAPPING
#endif

#define MAPPING \
        { 0xf8000000, 0x80000000, 0x02000000, DOMAIN_IO, 0, 1 }, \
        { 0xfa000000, 0x90000000, 0x02000000, DOMAIN_IO, 0, 1 }, \
	{ 0xfc000000, 0xa0000000, 0x02000000, DOMAIN_IO, 0, 1 }, \
	{ 0xfe000000, 0xb0000000, 0x02000000, DOMAIN_IO, 0, 1 }, \
	FLASH_MAPPING	\
        SA1101_MAPPING	\
	{ 0xe0000000, 0x20000000, 0x04000000, DOMAIN_IO, 0, 1 }, /* PCMCIA0 IO */ \
	{ 0xe4000000, 0x30000000, 0x04000000, DOMAIN_IO, 0, 1 }, /* PCMCIA1 IO */ \
	{ 0xe8000000, 0x28000000, 0x04000000, DOMAIN_IO, 0, 1 }, /* PCMCIA0 attr*/ \
	{ 0xec000000, 0x38000000, 0x04000000, DOMAIN_IO, 0, 1 }, /* PCMCIA1 attr */ \
	{ 0xf0000000, 0x2c000000, 0x04000000, DOMAIN_IO, 0, 1 }, /* PCMCIA0 mem */ \
	{ 0xf4000000, 0x3c000000, 0x04000000, DOMAIN_IO, 0, 1 }, /* PCMCIA1 mem */

#include "mm-armv.c"


/* 
 * Here are the page address translation functions, in case they aren't
 * provided as macros.
 */
#if !defined(__virt_to_phys__is_a_macro) || !defined(__phys_to_virt__is_a_macro)

#define SIZE_W_BANK_0	(RAM_IN_BANK_0                )
#define SIZE_W_BANK_1	(RAM_IN_BANK_1 + SIZE_W_BANK_0)
#define SIZE_W_BANK_2	(RAM_IN_BANK_2 + SIZE_W_BANK_1)
#define SIZE_W_BANK_3	(RAM_IN_BANK_3 + SIZE_W_BANK_2)

#define PHYS_OFF_BANK_0	(_DRAMBnk0                )
#define PHYS_OFF_BANK_1	(_DRAMBnk1 - SIZE_W_BANK_0)
#define PHYS_OFF_BANK_2	(_DRAMBnk2 - SIZE_W_BANK_1)
#define PHYS_OFF_BANK_3	(_DRAMBnk3 - SIZE_W_BANK_2)

#ifndef __virt_to_phys__is_a_macro
unsigned long __virt_to_phys(unsigned long vpage)
{
#ifndef CONFIG_SA1100_LART
	vpage -= PAGE_OFFSET;
	return	(vpage < SIZE_W_BANK_0) ? (vpage + PHYS_OFF_BANK_0) :
		(vpage < SIZE_W_BANK_1) ? (vpage + PHYS_OFF_BANK_1) :
		(vpage < SIZE_W_BANK_2) ? (vpage + PHYS_OFF_BANK_2) :
		(vpage < SIZE_W_BANK_3) ? (vpage + PHYS_OFF_BANK_3) :
		_ZeroMem;
#else
	/* twiddle for the LART memory trickery */
	unsigned long ppage;
	vpage -= PAGE_OFFSET;
	if (vpage < SIZE_W_BANK_0) {
		ppage = vpage + PHYS_OFF_BANK_0;
		if( ppage & LINE_A23 ) ppage += LINE_A23;
	}else if (vpage < SIZE_W_BANK_1) {
		ppage = vpage + PHYS_OFF_BANK_1;
		if( ppage & LINE_A23 ) ppage += LINE_A23;
	}else ppage = 
		(vpage < SIZE_W_BANK_2) ? (vpage + PHYS_OFF_BANK_2) :
		(vpage < SIZE_W_BANK_3) ? (vpage + PHYS_OFF_BANK_3) :
		_ZeroMem;
	return ppage;
#endif
}
#endif

#ifndef __phys_to_virt__is_a_macro
unsigned long __phys_to_virt(unsigned long ppage)
{
#ifndef CONFIG_SA1100_LART
	switch( (ppage - PHYS_OFF_BANK_0) >> 27 ) {
	    case 0:  return ppage - PHYS_OFF_BANK_0 + PAGE_OFFSET;
	    case 1:  return ppage - PHYS_OFF_BANK_1 + PAGE_OFFSET;
	    case 2:  return ppage - PHYS_OFF_BANK_2 + PAGE_OFFSET;
	    case 3:  return ppage - PHYS_OFF_BANK_3 + PAGE_OFFSET;
	    default: return 0;
	}
#else
	/* twiddle for the LART memory trickery */
	switch( (ppage - PHYS_OFF_BANK_0) >> 27 ) {
	    case 0:  return ppage - PHYS_OFF_BANK_0 + PAGE_OFFSET -
				 ((ppage & LINE_A24) ? LINE_A23 : 0);
	    case 1:  return ppage - PHYS_OFF_BANK_1 + PAGE_OFFSET -
				 ((ppage & LINE_A24) ? LINE_A23 : 0);
	    case 2:  return ppage - PHYS_OFF_BANK_2 + PAGE_OFFSET;
	    case 3:  return ppage - PHYS_OFF_BANK_3 + PAGE_OFFSET;
	    default: return 0;
	}
#endif
}
#endif

#endif  /* macros not defined */

#ifdef CONFIG_SA1100_EMPEG
void empeg_setup_bank_mapping(int hw_rev)
{
	if (hw_rev >= 9) {
	//	empeg_phys_to_virt_mapping[0] = 0xc0000000;
		empeg_phys_to_virt_mapping[1] = 0xc1000000;
		empeg_phys_to_virt_mapping[2] = 0xc2000000;
		empeg_phys_to_virt_mapping[3] = 0xc3000000;
		memcpy(empeg_virt_to_phys_mapping,empeg_virt_to_phys_mapping_mk2a,sizeof(empeg_virt_to_phys_mapping));
	}
}
#endif
