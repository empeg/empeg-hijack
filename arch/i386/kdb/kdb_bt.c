	/*
	 * Minimalist Kernel Debugger 
	 *
	 * 	Machine dependent stack traceback code.
	 *
	 * Copyright 1999, Silicon Graphics, Inc.
	 *
	 * Written by Scott Lurndal at Silicon Graphics, March 1999.
	 */

#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kdb.h>
#include <asm/system.h>
#include "kdbsupport.h"


#if defined(CONFIG_KDB_FRAMEPTR)
/*
 * kdb_prologue
 *
 *	This function analyzes a gcc-generated function prototype
 *	when frame pointers are enabled to determine the amount of
 *	automatic storage and register save storage is used on the
 *	stack of the target function.
 * Parameters:
 *	eip	Address of function to analyze
 * Outputs:
 *	*nauto	# bytes of automatic storage
 *	*nsave  # of saved registers
 * Returns:
 *	None.
 * Locking:
 *	None.
 * Remarks:
 *
 * 	A prologue generally looks like:
 *
 *		pushl  %ebp		(All functions)
 *		movl   %esp, %ebp	(All functions)
 *		subl   $auto, %esp	[some functions]
 *		pushl   reg		[some functions]
 *		pushl   reg		[some functions]
 */

void
kdb_prologue(unsigned long eip, unsigned long *nauto, unsigned long *nsave)
{
	size_t insn_size = sizeof(unsigned char);

	*nauto = 0;
	*nsave = 0;

	if (eip == 0) 
		return;

	if ((kdbgetword(eip, insn_size) != 0x55)	/* pushl %ebp */
	 || (kdbgetword(eip+1, insn_size) != 0x89)){/* movl esp, ebp */
		/*
		 * Unknown prologue type.
		 */
		return;
	}

	if (kdbgetword(eip+3, insn_size) == 0x83) {
		*nauto = kdbgetword(eip+5, sizeof(unsigned char));
		eip += 6;
	} else if (kdbgetword(eip+3, insn_size) == 0x81) {
		*nauto = kdbgetword(eip+5, sizeof(unsigned long));
		eip += 9;
	} else 
		eip += 3;


	while ((kdbgetword(eip, insn_size)&0xf8) == 0x50) 
		(*nsave)++, eip++;
}
#endif

/*
 * kdb_bt
 *
 *	This function implements the 'bt' command.  Print a stack 
 *	traceback. 
 *
 *	bt [<address-expression>]   (addr-exp is for alternate stacks)
 *	btp <pid>		     (Kernel stack for <pid>)
 *
 * 	address expression refers to a return address on the stack.  It
 *	is expected to be preceeded by a frame pointer.
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 *	envp	environment vector
 *	regs	registers at time kdb was entered.
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 *	Doing this without a frame pointer is _hard_.  some simple
 *	things are done here, but we usually don't find more than the
 *	first couple of frames yet.   More coming in this area.
 *
 *	mds comes in handy when examining the stack to do a manual
 *	traceback.
 */

int
kdb_bt(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	int done = 0;
#if !defined(CONFIG_KDB_FRAMEPTR)
	unsigned long sp;
#endif
	unsigned long base, limit, esp, ebp, eip;
	unsigned long start = 0;	/* Start address of current function */
	unsigned long addr;
	long offset = 0;
	int nextarg;
	int argcount=5;
	char	*name;
	int	diag;
	struct pt_regs	   taskregs;
	struct frame {
		unsigned long ebp;
		unsigned long eip;
	} old;
	unsigned long stackbase = (unsigned long)current;

	/*
	 * Determine how many possible arguments to print.
	 */
	diag = kdbgetintenv("BTARGS", &argcount);

	if (strcmp(argv[0], "btp") == 0){
		struct task_struct *p;
		int		   pid;
		
		diag = kdbgetularg((char *)argv[1], (unsigned long*)&pid);
		if (diag)
			return diag;

		taskregs.eax = 1;
		for_each_task(p) {
			if (p->pid == pid) {
				taskregs.eip = p->tss.eip;
				taskregs.esp = p->tss.esp;
				taskregs.ebp = p->tss.ebp;
				/*
				 * Since we don't really use the TSS
				 * to store register between task switches,
				 * attempt to locate real ebp (should be
				 * top of stack if task is in schedule)
				 */
				if (taskregs.ebp == 0) {
					taskregs.ebp = 
					 *(unsigned long *)(taskregs.esp);
				}

				taskregs.eax = 0;
				stackbase = (unsigned long)p;
				break;
			}
		}

		if (taskregs.eax == 1) {
			kdb_printf("No process with pid == %d found\n",
				   pid);
			return 0;
		}
		regs = &taskregs;
	} else {
		if (argc) {
			nextarg = 1;
			diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL, regs);
			if (diag)
				return diag;

			/*
			 * We assume the supplied address is points to an
			 * EIP value on the stack, and the word pushed above
			 * it is an EBP value.   Start as if we were in the
			 * middle of that frame.
			 */
			taskregs.eip = kdbgetword(addr, sizeof(unsigned long));
			taskregs.ebp = kdbgetword(addr-4, sizeof(unsigned long));
			taskregs.ebp = kdbgetword(taskregs.ebp, sizeof(unsigned long));
			regs = &taskregs;
		}
	}

	name = kdbnearsym(regs->eip);
	if (name) {
		start = kdbgetsymval(name);
	} else {
		kdb_printf("Cannot determine function for eip = 0x%x\n", 
			   regs->eip);
		return 0;
	}
#if !defined(CONFIG_KDB_FRAMEPTR)
	/*
	 * starting with %esp value, track the stack back to 
	 * the end.
	 */

	sp = (unsigned long) (1+regs);	/* Point past exception frame */
	sp -= 8;			/* Adjust to encompass return addr */
	if (regs->xcs & 3) {
		/* User space? */
		sp = regs->esp;
	}
	sp -= 8;			/* Adjust to encompass return addr */
	base = sp & ~0x1fff;		/* XXX - use stack size constant */


	kdb_printf("    ESP       EIP         Function(args)\n");
	kdb_printf("0x%x 0x%x  %s ()\n", 
		   sp, regs->eip, name);

	while (!done) {
		extern char _text, _etext;
		extern char __init_begin, __init_end;
		unsigned long word;

		word = *(unsigned long *)(sp);
		if (  ((word >= (unsigned long)&_text) 
		    && (word <= (unsigned long)&_etext))
		 ||   ((word >= (unsigned long)&__init_begin)
		    && (word <= (unsigned long)&__init_end))) {
			/* text address */

#if defined(NOTNOW)
			kdb_printf("word %x sp %x char %x off %x\n",
				   word, sp, *(unsigned char *)(word - 5),
				   *(signed long *)(word - 4));
#endif
			/*
			 * If the instruction 5 bytes before the
			 * return instruction is a call, treat this
			 * entry as a probable activation record
			 */
			if (*(unsigned char *)(word - 5) == 0xe8) {
				signed long val = *(signed long *)(word - 4);
				int         i;
	
				/*
				 * Is this a real activation record?
				 */
				/*
				 * Of course, this misses all the 
				 * indirect calls, etc. XXX XXX
				 */
				if ((val + word) == start) {
					kdb_printf("0x%x 0x%x  ", 
						   sp,  word);
					name = kdbnearsym(word);
					if (name) {
						kdb_printf("%s (", name);
					}
					for(i=0; i<argcount; i++) {
						kdb_printf("0x%x, ", 
						   *(unsigned long *)(sp+(4*i)));
					}
					kdb_printf(")\n");
				}
	
			}
		}
		sp += sizeof(unsigned long);
		if (sp >= base+0x2000) 	/* XXX - use stack size constant */
			done = 1;
	}
#else
	kdb_printf("    EBP       EIP         Function(args)\n");

	base = stackbase;

	esp = regs->esp;
	ebp = regs->ebp;
	eip = regs->eip;

	limit = base + 0x2000;			/* stack limit */

#if 0
	printk("stackbase = 0x%lx base = 0x%lx limit = 0x%lx ebp = 0x%lx esp = 0x%lx eip = 0x%lx\n",
		stackbase, base, limit, ebp, esp, eip);
#endif

	if ((eip == start)		/* Beginning of function */
	 || (eip == start+1)) {		/* Right after pushl %ebp */
		/*
		 * If at start of function, create a dummy frame
		 */
		old.ebp = ebp;
		if (kdb_flags & KDB_FLAG_FAULT) {
			if ((regs->xcs&0xffff) == 0x10) {
				/*
				 * For instruction breakpoint on a target of a 
				 * call instruction, we must fetch the caller's
				 * eip shall we say, differently.
				 */
				old.eip = regs->esp;	/* Don't Ask */
			} else {
				old.eip = *(unsigned long *)(regs+1);
			}
		} else {
			old.eip = (eip == start)
				 ?kdbgetword(esp, sizeof(unsigned long))
				 :kdbgetword(esp+4, sizeof(unsigned long));
		}
		ebp = (unsigned long)&old;
	}

	/*
	 * While the ebp remains within the stack, continue to 
	 * print stack entries.
	 *
	 * XXX - this code depends on the fact that kernel
	 *       stacks are aligned on address boundaries 
	 *       congruent to 0 modulo 8192 and are 8192 bytes
	 *       in length and are part of the 8k bytes based
	 *	 at the process structure (e.g. current).
	 *
	 */

	while (!done) {
		unsigned long nebp, neip;
		char *newname;

		kdb_printf("0x%x 0x%x  ", ebp, eip);
		kdb_printf("%s", name);

		if (ebp < PAGE_OFFSET) {
			done++;
			kdb_printf("\n");
			continue;
		}

		nebp = kdbgetword(ebp, sizeof(unsigned long));
		neip = kdbgetword(ebp+4, sizeof(unsigned long));

		if (eip != start) {
			kdb_printf("+0x%x", eip - start);
		}

		/*
		 * Get frame for caller to determine the frame size
		 * and argument count.
		 */
		newname = kdbnearsym(neip);
		if (newname) {
			int i;
			unsigned long nauto, nsave, nargs;

			start = kdbgetsymval(newname);
			name = newname;

			kdb_printf("( ");

			kdb_prologue(kdbgetsymval(newname), &nauto, &nsave);

			nargs = (nebp - ebp - 8 - nauto - (nsave * 4))/4;
			if (nargs > argcount)
				nargs = argcount;
			for(i=0; i<nargs; i++){
				unsigned long argp = ebp + 8 + (4*i);

				if (i) 
					kdb_printf(", ");
				kdb_printf("0x%x", 
				     kdbgetword(argp, sizeof(unsigned long)));
			}
		}
		kdb_printf(")\n");


		ebp = nebp;
		eip = neip;
	}
#endif	/* CONFIG_KDB_FRAMEPTR */
	return 0;
}

