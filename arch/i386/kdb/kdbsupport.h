/*
 * Kernel Debugger Breakpoint Handler
 *
 * Copyright 1999, Silicon Graphics, Inc.
 *
 * Written March 1999 by Scott Lurndal at Silicon Graphics, Inc.
 */

#if !defined(_KDSUPPORT)
#define _KDSUPPORT

#include <asm/ptrace.h>

	/*
	 * This file provides definitions for functions that
	 * are dependent upon the product into which  kdb is
	 * linked. 
	 *
	 * This version is for linux.
	 */
typedef void (*handler_t)(struct pt_regs *);
typedef unsigned long k_machreg_t;

extern char*         kbd_getstr(char *, size_t, char *);
extern int           kdbinstalltrap(int, handler_t, handler_t*);
extern int           kdbinstalldbreg(kdb_bp_t*);
extern void          kdbremovedbreg(int);
extern void          kdb_initbptab(void);
extern int	     kdbgetregcontents(const char *, struct pt_regs *, unsigned long *);
extern int	     kdbsetregcontents(const char *, struct pt_regs *, unsigned long);
extern int	     kdbdumpregs(struct pt_regs *, const char *, const char *);


/*
 * kdb_db_trap is a processor dependent routine invoked
 * from kdb() via the #db trap handler.   It handles breakpoints involving
 * the processor debug registers and handles single step traps
 * using the processor trace flag.
 */

#define KDB_DB_BPT	0	/* Straight breakpoint */
#define KDB_DB_SS	1	/* Single Step trap */
#define KDB_DB_SSB	2	/* Single Step, caller should continue */

extern int 	     kdb_db_trap(struct pt_regs *);


	/*
	 *  Support for ia32 architecture debug registers.
	 */
#define KDB_DBREGS	4
extern k_machreg_t dbregs[];

#define DR6_BT  0x00008000
#define DR6_BS	0x00004000
#define DR6_BD  0x00002000

#define DR6_B3  0x00000008
#define DR6_B2	0x00000004
#define DR6_B1  0x00000002
#define DR6_B0	0x00000001

#define DR7_RW_VAL(dr, drnum) \
       (((dr) >> (16 + (4 * (drnum)))) & 0x3)

#define DR7_RW_SET(dr, drnum, rw)                              \
       do {                                                    \
               (dr) &= ~(0x3 << (16 + (4 * (drnum))));         \
               (dr) |= (((rw) & 0x3) << (16 + (4 * (drnum)))); \
       } while (0)

#define DR7_RW0(dr)       DR7_RW_VAL(dr, 0)
#define DR7_RW0SET(dr,rw)  DR7_RW_SET(dr, 0, rw)
#define DR7_RW1(dr)       DR7_RW_VAL(dr, 1)
#define DR7_RW1SET(dr,rw)  DR7_RW_SET(dr, 1, rw)
#define DR7_RW2(dr)       DR7_RW_VAL(dr, 2)
#define DR7_RW2SET(dr,rw)  DR7_RW_SET(dr, 2, rw)
#define DR7_RW3(dr)       DR7_RW_VAL(dr, 3)
#define DR7_RW3SET(dr,rw)  DR7_RW_SET(dr, 3, rw)


#define DR7_LEN_VAL(dr, drnum) \
       (((dr) >> (18 + (4 * (drnum)))) & 0x3)

#define DR7_LEN_SET(dr, drnum, rw)                             \
       do {                                                    \
               (dr) &= ~(0x3 << (18 + (4 * (drnum))));         \
               (dr) |= (((rw) & 0x3) << (18 + (4 * (drnum)))); \
       } while (0)

#define DR7_LEN0(dr)        DR7_LEN_VAL(dr, 0)
#define DR7_LEN0SET(dr,len)  DR7_LEN_SET(dr, 0, len)
#define DR7_LEN1(dr)        DR7_LEN_VAL(dr, 1)
#define DR7_LEN1SET(dr,len)  DR7_LEN_SET(dr, 1, len)
#define DR7_LEN2(dr)        DR7_LEN_VAL(dr, 2)
#define DR7_LEN2SET(dr,len)  DR7_LEN_SET(dr, 2, len)
#define DR7_LEN3(dr)        DR7_LEN_VAL(dr, 3)
#define DR7_LEN3SET(dr,len)  DR7_LEN_SET(dr, 3, len)

#define DR7_G0(dr)    (((dr)>>1)&0x1)
#define DR7_G0SET(dr) ((dr) |= 0x2)
#define DR7_G0CLR(dr) ((dr) &= ~0x2)
#define DR7_G1(dr)    (((dr)>>3)&0x1)
#define DR7_G1SET(dr) ((dr) |= 0x8)
#define DR7_G1CLR(dr) ((dr) &= ~0x8)
#define DR7_G2(dr)    (((dr)>>5)&0x1)
#define DR7_G2SET(dr) ((dr) |= 0x20)
#define DR7_G2CLR(dr) ((dr) &= ~0x20)
#define DR7_G3(dr)    (((dr)>>7)&0x1)
#define DR7_G3SET(dr) ((dr) |= 0x80)
#define DR7_G3CLR(dr) ((dr) &= ~0x80)

#define DR7_L0(dr)    (((dr))&0x1)
#define DR7_L0SET(dr) ((dr) |= 0x1)
#define DR7_L0CLR(dr) ((dr) &= ~0x1)
#define DR7_L1(dr)    (((dr)>>2)&0x1)
#define DR7_L1SET(dr) ((dr) |= 0x4)
#define DR7_L1CLR(dr) ((dr) &= ~0x4)
#define DR7_L2(dr)    (((dr)>>4)&0x1)
#define DR7_L2SET(dr) ((dr) |= 0x10)
#define DR7_L2CLR(dr) ((dr) &= ~0x10)
#define DR7_L3(dr)    (((dr)>>6)&0x1)
#define DR7_L3SET(dr) ((dr) |= 0x40)
#define DR7_L3CLR(dr) ((dr) &= ~0x40)

#define DR7_GD		0x00002000		/* General Detect Enable */
#define DR7_GE		0x00000200		/* Global exact */
#define DR7_LE		0x00000100		/* Local exact */

extern k_machreg_t kdb_getdr6(void);
extern void kdb_putdr6(k_machreg_t);

extern k_machreg_t kdb_getdr7(void);
extern void kdb_putdr7(k_machreg_t);

extern k_machreg_t kdb_getdr(int);
extern void kdb_putdr(int, k_machreg_t);

extern k_machreg_t kdb_getcr(int);

extern void kdb_bp_install(void);
extern void kdb_bp_remove(void);

/*
 * Support for setjmp/longjmp
 */
#define JB_BX   0
#define JB_SI   1
#define JB_DI   2
#define JB_BP   3
#define JB_SP   4
#define JB_PC   5

typedef struct __kdb_jmp_buf {
	unsigned long   regs[6];
} kdb_jmp_buf;

extern int kdb_setjmp(kdb_jmp_buf *);
extern void kdb_longjmp(kdb_jmp_buf *, int);

extern kdb_jmp_buf  kdbjmpbuf;

#endif	/* KDSUPPORT */
