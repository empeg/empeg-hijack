/*
 * linux/include/asm-arm/arch-sa1100/processor.h
 *
 * Copyright (c) 1996 Russell King.
 *
 * Changelog:
 *  10-09-1996	RMK	Created
 *  05-01-1999  HBF     Mods for SA1100
 *  21-09-1999  NP	SWAPPER_PG_DIR readjusted for SA1100
 */

#ifndef __ASM_ARCH_PROCESSOR_H
#define __ASM_ARCH_PROCESSOR_H

/*
 * Bus types
 */
#define EISA_bus 0
#define EISA_bus__is_a_macro /* for versions in ksyms.c */
#define MCA_bus 0
#define MCA_bus__is_a_macro /* for versions in ksyms.c */

/*
 * User space: 3GB
 */
#define TASK_SIZE	(0xc0000000UL)

/* This decides where the kernel will search for a free chunk of vm
 * space during mmap's.
 */
#define TASK_UNMAPPED_BASE (TASK_SIZE / 3)

/*
 * SWAPPER_PG_DIR can't be defined using PAGE_OFFSET like it is done in
 * ../proc-armv/processor.h since virt and phys ram starts at the same addr.
 * If this file is included after <asm/proc/processor.h> in ../processor.h, 
 * then we can redefine SWAPPER_PG_DIR here.  Otherwise the include order
 * must be reversed.
 */
#ifdef SWAPPER_PG_DIR
#undef SWAPPER_PG_DIR
#define SWAPPER_PG_DIR  (((unsigned long)swapper_pg_dir) - 0)
#else
#error SWAPPER_PG_DIR will be wrongly redefined
#endif

#endif
