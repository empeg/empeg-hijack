/*
 * Kernel Debugger
 *
 * Copyright 1999, Silicon Graphics, Inc.
 *
 * Written March 1999 by Scott Lurndal at Silicon Graphics, Inc.
 */

#if !defined(__KDB_H)
#define __KDB_H

#include "bfd.h"

#define KDB_MAJOR_VERSION	0
#define KDB_MINOR_VERSION	5

/*
 * Kernel Debugger Error codes 
 */

#define KDB_NOTFOUND	-1
#define KDB_GO		-2
#define KDB_ARGCOUNT	-3
#define KDB_BADWIDTH	-4
#define KDB_BADRADIX	-5
#define KDB_NOTENV	-6
#define KDB_NOENVVALUE	-7
#define KDB_NOTIMP	-8
#define KDB_ENVFULL	-9
#define KDB_ENVBUFFULL	-10
#define KDB_TOOMANYBPT	-11
#define KDB_TOOMANYDBREGS -12
#define KDB_DUPBPT	-13
#define KDB_BPTNOTFOUND	-14
#define KDB_BADMODE	-15
#define KDB_BADINT	-16
#define KDB_INVADDRFMT  -17
#define KDB_BADREG      -18
#define KDB_CPUSWITCH	-19
#define KDB_BADCPUNUM   -20
#define KDB_BADLENGTH	-21
#define KDB_NOBP	-22

	/*
	 * XXX - machine dependent.
	 */
#define KDB_ENTER()	asm("\tint $129\n")

	/*
	 * kdb_active is initialized to zero, and is set 
	 * to KDB_REASON_xxx whenever the kernel debugger is entered.
	 */
extern int kdb_active;	

	/*
	 * KDB_FLAG_EARLYKDB is set when the 'kdb' option is specified
	 * as a boot parameter (e.g. via lilo).   It indicates that the
	 * kernel debugger should be entered as soon as practical.
	 */
#define KDB_FLAG_EARLYKDB	0x00000001
	/*
	 * KDB_FLAG_SSB is set when the 'ssb' command is in progress.  It
	 * indicates to the debug fault trap code that a trace fault
	 * (single step) should be continued rather than the debugger
	 * be entered.
	 */
#define KDB_FLAG_SSB		0x00000002
	/*
	 * KDB_FLAG_SUPRESS is set when an error message is printed
	 * by a helper function such as kdbgetword.  No further error messages
	 * will be printed until this flag is reset.  Used to prevent 
	 * an illegal address from causing voluminous error messages.
	 */
#define KDB_FLAG_SUPRESS	0x00000004
	/*
	 * KDB_FLAG_FAULT is set when handling a debug register instruction
	 * fault.  The backtrace code needs to know.
	 */
#define KDB_FLAG_FAULT		0x00000008

extern int kdb_flags;

extern int kdb_nextline;

	/*
	 * External entry point for the kernel debugger.  The pt_regs
	 * at the time of entry are supplied along with the reason for
	 * entry to the kernel debugger.
	 */

struct pt_regs;
extern int   kdb(int reason, int error_code, struct pt_regs *);
#define KDB_REASON_ENTER    1		/* call debugger from source */
#define KDB_REASON_FAULT    2		/* called from fault, eframe valid */
#define KDB_REASON_BREAK    3		/* called from int 3, eframe valid */
#define KDB_REASON_DEBUG    4		/* Called from int #DB, eframe valid */
#define KDB_REASON_PANIC    5		/* Called from panic(), eframe valid */
#define KDB_REASON_SWITCH   6		/* Called via CPU switch */
#define KDB_REASON_INT      7		/* Called via int 129 */
#define KDB_REASON_KEYBOARD 8           /* Called via keyboard interrupt */

	/*
	 * The entire contents of this file from this point forward are
	 * private to the kernel debugger.     They are subject to change
	 * without notice.
	 */

	/*
	 * Breakpoint state 
	 */

typedef struct _kdb_bp {
	bfd_vma 	bp_addr;	/* Address breakpoint is present at */
	unsigned char	bp_prevbyte;	/* Byte which the int3 replaced */

	unsigned int 	bp_regtype:1;	/* Uses a debug register (0-4) */
	unsigned int	bp_enabled:1;	/* Breakpoint is active in register */
	unsigned int	bp_global:1;	/* Global to all processors */
	unsigned int	bp_free:1;	/* This entry is available */
	unsigned int	bp_data:1;	/* Data breakpoint */
	unsigned int    bp_write:1;	/* Write data breakpoint */
	unsigned int	bp_mode:2;	/* 0=inst, 1=write, 2=io, 3=read */
	unsigned int	bp_length:2;	/* 0=1 byte, 1=2 bytes, 2=BAD, 3=4 */

	int		bp_reg;		/* Register that this breakpoint uses */
	int		bp_cpu;		/* Cpu #  (if bp_global == 0) */
} kdb_bp_t;

extern void  kdb_global(int);		/* Set global actions */

	/*
	 * External Function Declarations
	 */
extern char *kdbgetenv(const char *);
extern int   kdbgetintenv(const char *, int *);
extern int   kdbgetularg(const char *, unsigned long *);
extern int   kdbgetaddrarg(int, const char**, int*, unsigned long *, long *, char **, struct pt_regs *);
extern unsigned long kdbgetword(unsigned long, int);
extern void  kdb_id1(unsigned long);
extern void  kdb_disinit(void);


	/*
	 * External command function declarations
	 */

extern int kdb_id	(int argc, const char **argv, const char **envp, struct pt_regs *);
extern int kdb_bp	(int argc, const char **argv, const char **envp, struct pt_regs *);
extern int kdb_bc	(int argc, const char **argv, const char **envp, struct pt_regs *);
extern int kdb_bt	(int argc, const char **argv, const char **envp, struct pt_regs *);
extern int kdb_ss	(int argc, const char **argv, const char **envp, struct pt_regs *);

	/*
	 * Symbol table format
	 */

typedef struct __symtab {
		char *name;
		unsigned long value;
		} __ksymtab_t;

extern __ksymtab_t __kdbsymtab[];
extern int __kdbsymtabsize;
extern int __kdbmaxsymtabsize;

extern unsigned long kdbgetsymval(const char *);
extern char *        kdbnearsym(unsigned long);
extern int           kdbaddmodsym(char *, unsigned long);
extern int           kdbdelmodsym(const char *);

typedef int (*kdb_func)(int, const char **, const char **, struct pt_regs*);

extern int           kdb_register(char *, kdb_func, char *, char *, short);
extern int           kdb_unregister(char *);

extern int	     kdb_printf(const char *,...);
#endif	/* __KDB_H */
