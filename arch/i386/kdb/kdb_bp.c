/*
 * Kernel Debugger Breakpoint Handler
 *
 * Copyright 1999, Silicon Graphics, Inc.
 *
 * Written March 1999 by Scott Lurndal at Silicon Graphics, Inc.
 */

#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/kdb.h>
#if defined(__SMP__)
#include <linux/smp.h>
#include <linux/sched.h>
#endif
#include <asm/system.h>
#include "kdbsupport.h"

/*
 * Table of breakpoints
 */
#define KDB_MAXBPT	16
static 	kdb_bp_t    breakpoints[KDB_MAXBPT];

static char *rwtypes[] = {"Instruction", "Data Write", "I/O", "Data Access"};

/*
 * kdb_bp_install
 *
 * 	Install breakpoints prior to returning from the kernel debugger. 
 * 	This allows the breakpoints to be set upon functions that are
 * 	used internally by kdb, such as printk().
 *
 * Parameters:
 *	None.
 * Outputs:
 *	None.
 * Returns:
 *	None.
 * Locking:
 *	None.
 * Remarks:
 */

void
kdb_bp_install(void)
{
	int i;

	for(i=0; i<KDB_MAXBPT; i++) {
		if (breakpoints[i].bp_enabled){
			kdbinstalldbreg(&breakpoints[i]);
		}
	}
}

#if defined(__SMP__)
/*
 * kdb_global
 *
 * 	Called from each processor when leaving the kernel debugger
 *	barrier IPI to establish the debug register contents for 
 *	global breakpoints.
 *
 * Parameters:
 *	cpuid		Calling logical cpu number
 * Outputs:
 *	None.
 * Returns:
 *	None.
 * Locking:
 *	None.
 * Remarks:
 */

/* ARGSUSED */
void
kdb_global(int cpuid)
{
	int i;

	for(i=0; i<KDB_MAXBPT; i++) {
		if (breakpoints[i].bp_enabled && breakpoints[i].bp_global){
			kdbinstalldbreg(&breakpoints[i]);
		}
	}

}
#endif	/* __SMP__ */

/*
 * kdb_bp_remove
 *
 * 	Remove breakpoints upon entry to the kernel debugger. 
 *
 * Parameters:
 *	None.
 * Outputs:
 *	None.
 * Returns:
 *	None.
 * Locking:
 *	None.
 * Remarks:
 */

void
kdb_bp_remove(void)
{
	int i;

	for(i=0; i<KDB_MAXBPT; i++) {
		if (breakpoints[i].bp_enabled) {
			kdbremovedbreg(breakpoints[i].bp_reg);
		}
	}
}

/*
 * Handle a debug register trap.   
 */

/*
 * kdb_db_trap
 *
 * 	Perform breakpoint processing upon entry to the
 *	processor debugger fault.   Determine and print
 *	the active breakpoint.
 *
 * Parameters:
 *	ef	Exception frame containing machine register state
 * Outputs:
 *	None.
 * Returns:
 *	0	Standard instruction or data breakpoint encountered
 *	1	Single Step fault ('ss' command)
 *	2	Single Step fault, caller should continue ('ssb' command)
 * Locking:
 *	None.
 * Remarks:
 *	Yup, there be goto's here.
 */

int
kdb_db_trap(struct pt_regs *ef)
{
	k_machreg_t  dr6;
	k_machreg_t  dr7;
	int rw, reg;
	int i;
	int rv = 0;

	dr6 = kdb_getdr6();
	dr7 = kdb_getdr7();

	if (dr6 & DR6_BS) {
		/* single step */
		rv = 1;		/* Indicate single step */
		if (kdb_flags & KDB_FLAG_SSB) {
			unsigned char op1, op2;

			op1 = *(unsigned char *)(ef->eip);
			if (op1 == 0x0f) 
				op2 = *(unsigned char *)(ef->eip+1);
			if (((op1&0xf0) == 0xe0)  /* short disp jumps */
			 || ((op1&0xf0) == 0x70)  /* Misc. jumps */
			 ||  (op1       == 0xc2)  /* ret */
			 ||  (op1       == 0x9a)  /* call */
			 || ((op1&0xf8) == 0xc8)  /* enter, leave, iret, int, */
			 || ((op1       == 0x0f)
			  && ((op2&0xf0)== 0x80))) {
				/*
				 * End the ssb command here.
				 */
				kdb_flags &= ~KDB_FLAG_SSB;
			} else {
				kdb_id1(ef->eip);
				rv = 2;	/* Indicate ssb - dismiss immediately */
			}
		} else {
			/*
			 * Print current insn
			 */
			kdb_printf("SS trap at 0x%x\n", ef->eip);
			kdb_id1(ef->eip);
		}

		if (rv != 2)
			ef->eflags &= ~EF_TF;
	}

	if (dr6 & DR6_B0) {
		rw = DR7_RW0(dr7);
		reg = 0;
		goto handle;
	}

	if (dr6 & DR6_B1) {
		rw = DR7_RW1(dr7);
		reg = 1;
		goto handle;
	}

	if (dr6 & DR6_B2) {
		rw = DR7_RW2(dr7);
		reg = 2;
		goto handle;
	}

	if (dr6 & DR6_B3) {
		rw = DR7_RW3(dr7);
		reg = 3;
		goto handle;
	}

	if (rv > 0) 
		goto handled;

	goto unknown;	/* dismiss */

handle:
	/*
	 * Set Resume Flag
	 */
	ef->eflags |= EF_RF;

	/*
	 * Determine which breakpoint was encountered.
	 */
	for(i=0; i<KDB_MAXBPT; i++) {
		if ((breakpoints[i].bp_enabled)
		 && (breakpoints[i].bp_reg == reg)) {
			/*
			 * Hit this breakpoint.  Remove it while we are
			 * handling hit to avoid recursion. XXX ??
			 */
			kdb_printf("%s breakpoint #%d at 0x%x\n", 
				  rwtypes[rw],
				  i, breakpoints[i].bp_addr);

			/*
			 * For an instruction breakpoint, disassemble
			 * the current instruction.
			 */
			if (rw == 0) {
				kdb_id1(ef->eip);
			}

			goto handled;
		}
	}

unknown: 
	kdb_printf("Unknown breakpoint.  Should forward. \n");

handled:

	/*
	 * Clear the pending exceptions.
	 */
	kdb_putdr6(0);
	
	return rv;
}

/*
 * kdb_printbp
 *
 * 	Internal function to format and print a breakpoint entry.
 *
 * Parameters:
 *	None.
 * Outputs:
 *	None.
 * Returns:
 *	None.
 * Locking:
 *	None.
 * Remarks:
 */

static void
kdb_printbp(kdb_bp_t *bp, int i)
{
	char *symname;
	long  offset;

	kdb_printf("%s ", rwtypes[bp->bp_mode]);

	kdb_printf("BP #%d at 0x%x ",
		   i, bp->bp_addr);
	symname = kdbnearsym(bp->bp_addr);
	if (symname){
		kdb_printf("(%s", symname);

		offset = bp->bp_addr - kdbgetsymval(symname);
		if (offset) {
			if (offset > 0)
				kdb_printf("+0x%x", offset);
			else
				kdb_printf("-0x%x", offset);
		}
		kdb_printf(") ");
	} 

	if (bp->bp_enabled) {
		kdb_printf("\n    is enabled in dr%d", bp->bp_reg);
		if (bp->bp_global)
			kdb_printf(" globally");
		else 
			kdb_printf(" on cpu %d", bp->bp_cpu);

		if (bp->bp_mode != 0) {
			kdb_printf(" for %d bytes", bp->bp_length + 1);
		}
	} else {
		kdb_printf("\n    is disabled");
	}

	kdb_printf("\n");
}

/*
 * kdb_bp
 *
 * 	Handle the bp, and bpa commands.
 *
 *	[bp|bpa] <addr-expression> [DATAR|DATAW|IO [length]]
 *
 * Parameters:
 *	argc	Count of arguments in argv
 *	argv	Space delimited command line arguments
 *	envp	Environment value
 *	regs	Exception frame at entry to kernel debugger
 * Outputs:
 *	None.
 * Returns:
 *	Zero for success, a kdb diagnostic if failure.
 * Locking:
 *	None.
 * Remarks:
 */

int
kdb_bp(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	int     i;
	kdb_bp_t *bp;
	int     diag;
	int     free, same;
	k_machreg_t addr;
	char   *symname = NULL;
	long    offset = 0ul;
	int	nextarg;
	unsigned long 	mode, length;
#if defined(__SMP__)
	int	global;
#endif

	if (argc == 0) {
		/*
		 * Display breakpoint table
		 */
		for(i=0,bp=breakpoints; i<KDB_MAXBPT; i++, bp++) {
			if (bp->bp_free) continue;

			kdb_printbp(bp, i);
		}

		return 0;
	}
	
#if defined(__SMP__)
	global = (strcmp(argv[0], "bpa") == 0);
#endif

	nextarg = 1;
	diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, &symname, regs);
	if (diag)
		return diag;

	mode = 0;		/* Default to instruction breakpoint */
	length = 0;		/* Length must be zero for insn bp */
	if ((argc + 1) != nextarg) {
		if (strnicmp(argv[nextarg], "datar", sizeof("datar")) == 0) {
			mode = 3;
		} else if (strnicmp(argv[nextarg], "dataw", sizeof("dataw")) == 0) {
			mode = 1;
		} else if (strnicmp(argv[nextarg], "io", sizeof("io")) == 0) {
			mode = 2;
		} else {
			return KDB_ARGCOUNT;
		}

		length = 3;	/* Default to 4 byte */

		nextarg++;

		if ((argc + 1) != nextarg) {
			diag = kdbgetularg((char *)argv[nextarg], &length);
			if (diag)
				return diag;

			if ((length > 4) || (length == 3)) 
				return KDB_BADLENGTH;

			length--;	/* Normalize for debug register */
			nextarg++;
		}

		if ((argc + 1) != nextarg) 
			return KDB_ARGCOUNT;
	}
	
	/*
	 * Allocate a new bp structure 
	 */
	free = same = KDB_MAXBPT;
	for(i=0; i<KDB_MAXBPT; i++) {
		if (breakpoints[i].bp_free) {
			if (free == KDB_MAXBPT) 
				free = i;
		} else if (breakpoints[i].bp_addr == addr) {
			same = i;
		}
	}
	if (same != KDB_MAXBPT) 
		return KDB_DUPBPT;

	if (free == KDB_MAXBPT)
		return KDB_TOOMANYBPT;

	bp = &breakpoints[free];

	bp->bp_addr = addr;
	bp->bp_mode = mode;
	bp->bp_length = length;
	
	/*
	 * Find a free register 
	 */
	for(i=0; i<KDB_DBREGS; i++) {
		if (dbregs[i] == 0xffffffff) {
			/*
			 * Set this to zero to indicate that it has been
			 * allocated.  We'll actually store it when leaving
			 * kdb when we reinstall the debug registers.
			 *
			 * This allows breakpoints to be set on kernel 
			 * functions that are also called by kdb without 
			 * interfering with kdb operations.
			 */
			dbregs[i] = 0;
			bp->bp_reg = i;
			bp->bp_free = 0;
			bp->bp_enabled = 1;
#if defined(__SMP__)
			if (global) 
				bp->bp_global = 1;
			else
				bp->bp_cpu = smp_processor_id();
#endif

			kdb_printbp(bp, free);

			break;
		}
	}
	/*
	 * XXX if not enough regs, fail.  Eventually replace
	 *     first byte of instruction with int03.
	 */
	if (i==KDB_DBREGS)
		return KDB_TOOMANYDBREGS;

	return 0;
}

/*
 * kdb_bc
 *
 * 	Handles the 'bc', 'be', and 'bd' commands
 *
 *	[bd|bc|be] <breakpoint-number>
 *
 * Parameters:
 *	argc	Count of arguments in argv
 *	argv	Space delimited command line arguments
 *	envp	Environment value
 *	regs	Exception frame at entry to kernel debugger
 * Outputs:
 *	None.
 * Returns:
 *	Zero for success, a kdb diagnostic for failure
 * Locking:
 *	None.
 * Remarks:
 */
#define KDBCMD_BC	0
#define KDBCMD_BE	1
#define KDBCMD_BD	2

int
kdb_bc(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	k_machreg_t 	addr;
	kdb_bp_t	*bp = 0;
	int lowbp = KDB_MAXBPT;
	int highbp = 0;
	int done = 0;
	int i;
	int diag;
	int cmd;			/* KDBCMD_B? */

	if (strcmp(argv[0], "be") == 0) {
		cmd = KDBCMD_BE;
	} else if (strcmp(argv[0], "bd") == 0) {
		cmd = KDBCMD_BD;
	} else 
		cmd = KDBCMD_BC;

	if (argc != 1) 
		return KDB_ARGCOUNT;

	if (strcmp(argv[1], "*") == 0) {
		lowbp = 0;
		highbp = KDB_MAXBPT;
	} else {
		diag = kdbgetularg(argv[1], &addr);
		if (diag) 
			return diag;

		/*
		 * For addresses less than the maximum breakpoint number, 
		 * assume that the breakpoint number is desired.
		 */
		if (addr < KDB_MAXBPT) {
			bp = &breakpoints[addr];
			lowbp = highbp = addr;
			highbp++;
		} else {
			for(i=0; i<KDB_MAXBPT; i++) {
				if (breakpoints[i].bp_addr == addr) {
					lowbp = highbp = i;
					highbp++;
					break;
				}
			}
		}
	}

	for(bp=&breakpoints[lowbp], i=lowbp; 
	    i < highbp;
	    i++, bp++) {
		if (bp->bp_free) 
			continue;
		
		done++;

		switch (cmd) {
		case KDBCMD_BC:
			kdbremovedbreg(bp->bp_reg);
			dbregs[bp->bp_reg] = 0xffffffff;
			kdb_printf("Breakpoint %d at 0x%x in dr%d cleared\n",
				i, bp->bp_addr, bp->bp_reg);
			bp->bp_free = 1;
			bp->bp_addr = 0;
			break;
		case KDBCMD_BE:
		{
			int r;

			for(r=0; r<KDB_DBREGS; r++) {
				if (dbregs[r] == 0xffffffff){
					bp->bp_reg = r;
					bp->bp_enabled = 1;
					kdb_printf("Breakpoint %d at 0x%x in dr%d enabled\n",
						i, bp->bp_addr, bp->bp_reg);
					break;
				}
			}
			if (r == KDB_DBREGS) 
				return KDB_TOOMANYDBREGS;

			break;
		}
		case KDBCMD_BD:
			kdbremovedbreg(bp->bp_reg);
			dbregs[bp->bp_reg] = 0xffffffff;
			bp->bp_enabled = 0;
			kdb_printf("Breakpoint %d at 0x%x in dr%d disabled\n",
				i, bp->bp_addr, bp->bp_reg);
			break;
		}
	}

	return (!done)?KDB_BPTNOTFOUND:0;
}

/*
 * kdb_initbptab
 *
 *	Initialize the breakpoint table.
 *
 * Parameters:
 *	None.
 * Outputs:
 *	None.
 * Returns:
 *	None.
 * Locking:
 *	None.
 * Remarks:
 */

void
kdb_initbptab(void)
{
	int i;

	/*
	 * First time initialization.  
	 */
	for (i=0; i<KDB_MAXBPT; i++) {
		breakpoints[i].bp_free = 1;
		breakpoints[i].bp_enabled = 0;
		breakpoints[i].bp_global = 0;
		breakpoints[i].bp_reg = 0;
		breakpoints[i].bp_mode = 0;
		breakpoints[i].bp_length = 0;
	}

	/*
	 * Don't allow setting breakpoint at address 0xffffffff
	 */
	for(i=0; i<KDB_DBREGS; i++) {
		dbregs[i] = 0xffffffff;
	}
}

/*
 * kdb_ss
 *
 *	Process the 'ss' (Single Step) and 'ssb' (Single Step to Branch)
 *	commands.
 *
 *	ss [<insn-count>]
 *	ssb
 *
 * Parameters:
 *	argc	Argument count
 *	argv	Argument vector
 *	envp	Environment vector
 *	regs	Registers at time of entry to kernel debugger
 * Outputs:
 *	None.
 * Returns:
 *	0 for success, a kdb error if failure.
 * Locking:
 *	None.
 * Remarks:
 *
 *	Set the trace flag in the EFLAGS register to trigger 
 *	a debug trap after the next instruction.   Print the 
 *	current instruction.
 *
 *	For 'ssb', set the trace flag in the debug trap handler 
 *	after printing the current insn and return directly without
 *	invoking the kdb command processor, until a branch instruction
 *	is encountered or SSCOUNT lines are printed.
 */

int
kdb_ss(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	int ssb = 0;

	ssb = (strcmp(argv[0], "ssb") == 0);
	if ((ssb && (argc != 0)) 
	 || (!ssb && (argc > 1))) {
		return KDB_ARGCOUNT;
	}

#if 0
	/*
	 * Fetch provided count
	 */
	diag = kdbgetularg(argv[1], &sscount);
	if (diag)
		return diag;
#endif

	/*
	 * Print current insn
	 */
	kdb_id1(regs->eip);	

	/*
  	 * Set trace flag and go.
	 */
	regs->eflags |= EF_TF;		

	if (ssb) 
		kdb_flags |= KDB_FLAG_SSB;

	return KDB_GO;
}
