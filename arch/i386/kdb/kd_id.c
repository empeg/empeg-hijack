/*
 * Kernel Debugger.
 *
 * 	This module parses the instruction disassembly (id) command
 *	and invokes the disassembler. 
 *
 * Copyright 1999, Silicon Graphics, Inc.
 *
 * Written March 1999 by Scott Lurndal at Silicon Graphics, Inc.
 */

#if defined(STANDALONE)
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>
#include "kdb.h"
#include "sakdbsupport.h"
#include "dis-asm.h"
#else
#include <stdarg.h>
#include <linux/kernel.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/kdb.h>
#include "dis-asm.h"
#include "kdbsupport.h"
#endif

static disassemble_info	kdb_di;

/* ARGSUSED */
static int
kdbgetsym(bfd_vma addr, struct disassemble_info *dip)
{

	return 0;
}

/* ARGSUSED */
static void
kdbprintintaddr(bfd_vma addr, struct disassemble_info *dip, int flag)
{
	char	*sym = kdbnearsym(addr);
	int	offset = 0;

	if (sym) {
		offset = addr - kdbgetsymval(sym);
	}

	/*
	 * Print a symbol name or address as necessary.
	 */
	if (sym) {
		if (offset)
			dip->fprintf_func(dip->stream, "%s+0x%x", sym, offset);
		else
			dip->fprintf_func(dip->stream, "%s", sym);
	} else {
		dip->fprintf_func(dip->stream, "0x%x", addr);
	}

	if (flag) 
		dip->fprintf_func(dip->stream, ":   ");
}

static void
kdbprintaddr(bfd_vma addr, struct disassemble_info *dip)
{
	kdbprintintaddr(addr, dip, 0);
}

/* ARGSUSED */
static int
kdbgetidmem(bfd_vma addr, bfd_byte *buf, int length, struct disassemble_info *dip)
{
	bfd_byte	*bp = buf;
	int		i;

	/*
	 * Fill the provided buffer with bytes from 
	 * memory, starting at address 'addr' for 'length bytes.
	 *
	 */

	for(i=0; i<length; i++ ){
		*bp++ = (bfd_byte)kdbgetword(addr++, sizeof(bfd_byte));
	}

	return 0;
}

static int
kdbfprintf(FILE* file, const char *fmt, ...)
{
	char buffer[256];
	va_list ap;

	va_start(ap, fmt);
	vsprintf(buffer, fmt, ap);
	va_end(ap);

	printk("%s", buffer);

	return 0;
}

/*
 * kdb_bt
 *
 * 	Handle the id (instruction display) command.
 *
 *	id  <addr>
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
kdb_id(int argc, const char **argv, const char **envp, struct pt_regs* regs)
{
	unsigned long		pc;
	int			icount;
	int			diag;
	int			i;
	char *			mode;
	int			nextarg;
	long			offset = 0;
	static unsigned long	lastpc=0;

	if (argc != 1) 
		if (lastpc == 0)
			return KDB_ARGCOUNT;
		else {
			char lastbuf[50];
			sprintf(lastbuf, "0x%lx", lastpc);
			argv[1] = lastbuf;
			argc = 1;
		}
	

	/*
	 * Fetch PC.  First, check to see if it is a symbol, if not,
	 * try address.
	 */
	nextarg = 1;
	diag = kdbgetaddrarg(argc, argv, &nextarg, &pc, &offset, NULL, regs);
	if (diag)
		return diag;

	/*
	 * Number of lines to display
	 */
	diag = kdbgetintenv("IDCOUNT", &icount);
	if (diag) 
		return diag;

	mode = kdbgetenv("IDMODE");
	if (mode) {
		if (strcmp(mode, "x86") == 0) {
			kdb_di.mach = bfd_mach_i386_i386;
		} else if (strcmp(mode, "8086") == 0) {
			kdb_di.mach = bfd_mach_i386_i8086;
		} else {
			return KDB_BADMODE;
		}
	}

	for(i=0; i<icount; i++) {
		kdbprintintaddr(pc, &kdb_di, 1);
		pc += print_insn_i386(pc, &kdb_di);
		kdb_printf("\n");
	}

	lastpc = pc;

	return 0;
}

/*
 * kdb_id1
 *
 * 	Disassemble a single instruction at 'pc'.
 *
 * Parameters:
 *	pc	Address of instruction to disassemble
 * Outputs:
 *	None.
 * Returns:
 *	Zero for success, a kdb diagnostic if failure.
 * Locking:
 *	None.
 * Remarks:
 */

void
kdb_id1(unsigned long pc)
{
	char *mode;

	/*
	 * Allow the user to specify that this instruction
	 * should be treated as a 16-bit insn.
	 */
	mode = kdbgetenv("IDMODE");
	if (mode) {
		kdb_di.mach = bfd_mach_i386_i386;
		if (strcmp(mode, "8086") == 0) {
			kdb_di.mach = bfd_mach_i386_i8086;
		}
	}

	kdbprintintaddr(pc, &kdb_di, 1);
	(void) print_insn_i386(pc, &kdb_di);
	kdb_printf("\n");
}

/*
 * kdb_disinit
 *
 * 	Initialize the disassembly information structure
 *	for the GNU disassembler.
 *
 * Parameters:
 *	None.
 * Outputs:
 *	None.
 * Returns:
 *	Zero for success, a kdb diagnostic if failure.
 * Locking:
 *	None.
 * Remarks:
 */

void
kdb_disinit(void)
{
	kdb_di.fprintf_func     = kdbfprintf;
	kdb_di.stream	    = NULL;
	kdb_di.application_data = NULL;
	kdb_di.flavour          = bfd_target_elf_flavour;
	kdb_di.arch		    = bfd_arch_i386;
	kdb_di.mach		    = bfd_mach_i386_i386;
	kdb_di.endian	    = BFD_ENDIAN_LITTLE;
	kdb_di.symbol	    = NULL;
	kdb_di.flags	    = 0;
	kdb_di.private_data	    = NULL;
	kdb_di.read_memory_func = kdbgetidmem;
	kdb_di.print_address_func = kdbprintaddr;
	kdb_di.symbol_at_address_func = kdbgetsym;
	kdb_di.buffer	    = NULL;
	kdb_di.buffer_vma       = 0;
	kdb_di.buffer_length    = 0;
	kdb_di.bytes_per_line   = 0;
	kdb_di.bytes_per_chunk  = 0;
	kdb_di.display_endian   = BFD_ENDIAN_LITTLE;
	kdb_di.insn_info_valid  = 0;
	kdb_di.branch_delay_insns = 0;
	kdb_di.data_size	    = 0;
	kdb_di.insn_type	    = 0;
	kdb_di.target           = 0;
	kdb_di.target2          = 0;
}
