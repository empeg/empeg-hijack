/*
 * Kernel Debugger Breakpoint Handler
 *
 * Copyright 1999, Silicon Graphics, Inc.
 *
 * Written March 1999 by Scott Lurndal at Silicon Graphics, Inc.
 */
#include <stdarg.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/kdb.h>
#include <linux/stddef.h> 
#include <linux/vmalloc.h>
#include <asm/uaccess.h>

#include "kdbsupport.h"

k_machreg_t dbregs[KDB_DBREGS];

void
kdb_init(void)
{
	extern void kdb_inittab(void);

	kdb_inittab();
	kdb_initbptab();
	kdb_disinit();
	kdb_printf("kdb version %d.%d by Scott Lurndal. "\
		   "Copyright SGI, All Rights Reserved\n",
		   KDB_MAJOR_VERSION, KDB_MINOR_VERSION);
}

/*
 * kdbprintf
 * kdbgetword
 * kdb_getstr
 */

char *
kbd_getstr(char *buffer, size_t bufsize, char *prompt)
{
	extern char* kdb_getscancode(char *, size_t);

#if defined(CONFIG_SMP)
	kdb_printf(prompt, smp_processor_id());
#else
	kdb_printf("%s", prompt);
#endif

	return kdb_getscancode(buffer, bufsize);

}

int
kdb_printf(const char *fmt, ...)
{
	char buffer[256];
	va_list	ap;
	int diag;
	int linecount;

	diag = kdbgetintenv("LINES", &linecount);
	if (diag) 
		linecount = 22;

	va_start(ap, fmt);
	vsprintf(buffer, fmt, ap);
	va_end(ap);

	printk("%s", buffer);

	if (strchr(buffer, '\n') != NULL) {
		kdb_nextline++;
	}

	if (kdb_nextline == linecount) {
		char buf1[16];
		char buf2[32];
		extern char* kdb_getscancode(char *, size_t);
		char *moreprompt;

		/*
		 * Pause until cr.
		 */
		moreprompt = kdbgetenv("MOREPROMPT");
		if (moreprompt == NULL) {
			moreprompt = "more> ";
		}

#if defined(CONFIG_SMP)
		if (strchr(moreprompt, '%')) {
			sprintf(buf2, moreprompt, smp_processor_id());
			moreprompt = buf2;
		}
#endif

		printk(moreprompt);
		(void) kdb_getscancode(buf1, sizeof(buf1));

		kdb_nextline = 1;

		if ((buf1[0] == 'q') 
		 || (buf1[0] == 'Q')) {
			kdb_longjmp(&kdbjmpbuf, 1);
		}
	}
	
	return 0;
}

unsigned long
kdbgetword(unsigned long addr, int width)
{
	/*
	 * This function checks the address for validity.  Any address
	 * in the range PAGE_OFFSET to high_memory is legal, any address
	 * which maps to a vmalloc region is legal, and any address which
	 * is a user address, we use get_user() to verify validity.
	 */

	if (addr < PAGE_OFFSET) {
		/*
		 * Usermode address.
		 */
		unsigned long diag;
		unsigned long ulval;

		switch (width) {
		case 4:
		{	unsigned long *lp;

			lp = (unsigned long *) addr;
			diag = get_user(ulval, lp);
			break;
		}
		case 2:
		{	unsigned short *sp;

			sp = (unsigned short *) addr;
			diag = get_user(ulval, sp);
			break;
		}
		case 1:
		{	unsigned char *cp;

			cp = (unsigned char *) addr;
			diag = get_user(ulval, cp);
			break;
		}
		default:
			printk("kdbgetword: Bad width\n");
			return 0L;
		}
			
		if (diag) {
			if ((kdb_flags & KDB_FLAG_SUPRESS) == 0) {
				printk("kdb: Bad user address 0x%lx\n", addr);
				kdb_flags |= KDB_FLAG_SUPRESS;
			}
			return 0L;
		}
		kdb_flags &= ~KDB_FLAG_SUPRESS;
		return ulval;
	}

	if (addr > (unsigned long)high_memory) {
		extern int kdb_vmlist_check(unsigned long, unsigned long);

		if (!kdb_vmlist_check(addr, addr+width)) {
			/*
			 * Would appear to be an illegal kernel address; 
			 * Print a message once, and don't print again until
			 * a legal address is used.
			 */
			if ((kdb_flags & KDB_FLAG_SUPRESS) == 0) {
				printk("kdb: Bad kernel address 0x%lx\n", addr);
				kdb_flags |= KDB_FLAG_SUPRESS;
			}
			return 0L;
		}
	}

	/*
	 * A good address.  Reset error flag.
	 */
	kdb_flags &= ~KDB_FLAG_SUPRESS;

	switch (width) {
	case 4:
	{	unsigned long *lp;

		lp = (unsigned long *)(addr);
		return *lp;
	}
	case 2:
	{	unsigned short *sp;

		sp = (unsigned short *)(addr);
		return *sp;
	}
	case 1:
	{	unsigned char *cp;

		cp = (unsigned char *)(addr);
		return *cp;
	}
	}

	printk("kdbgetword: Bad width\n");
	return 0L;
}

int
kdbinstalltrap(int type, handler_t newh, handler_t *oldh)
{
	/*
	 * Usurp INTn.  XXX - TBD.
	 */

	return 0;
}

int
kdbinstalldbreg(kdb_bp_t *bp)
{
	k_machreg_t	dr7;

	dr7 = kdb_getdr7();

	kdb_putdr(bp->bp_reg, bp->bp_addr);

	dr7 |= DR7_GE;

	switch (bp->bp_reg){
	case 0:
		DR7_RW0SET(dr7,bp->bp_mode);
		DR7_LEN0SET(dr7,bp->bp_length);
		DR7_G0SET(dr7);
		break;
	case 1:
		DR7_RW1SET(dr7,bp->bp_mode);
		DR7_LEN1SET(dr7,bp->bp_length);
		DR7_G1SET(dr7);
		break;
	case 2:
		DR7_RW2SET(dr7,bp->bp_mode);
		DR7_LEN2SET(dr7,bp->bp_length);
		DR7_G2SET(dr7);
		break;
	case 3:
		DR7_RW3SET(dr7,bp->bp_mode);
		DR7_LEN3SET(dr7,bp->bp_length);
		DR7_G3SET(dr7);
		break;
	default:
		kdb_printf("Bad debug register!! %d\n", bp->bp_reg);
		break;
	} 

	kdb_putdr7(dr7);
	return 0;
}

void
kdbremovedbreg(int regnum)
{
	k_machreg_t	dr7;

	dr7 = kdb_getdr7();
	
	kdb_putdr(regnum, 0);

	switch (regnum) {
	case 0:
		DR7_G0CLR(dr7);
		DR7_L0CLR(dr7);
		break;
	case 1:
		DR7_G1CLR(dr7);
		DR7_L1CLR(dr7);
		break;
	case 2:
		DR7_G2CLR(dr7);
		DR7_L2CLR(dr7);
		break;
	case 3:
		DR7_G3CLR(dr7);
		DR7_L3CLR(dr7);
		break;
	default:
		kdb_printf("Bad debug register!! %d\n", regnum);
		break;
	}

	kdb_putdr7(dr7);
}

k_machreg_t
kdb_getdr6(void)
{
	return kdb_getdr(6);
}

k_machreg_t
kdb_getdr7(void)
{
	return kdb_getdr(7);
}

k_machreg_t
kdb_getdr(int regnum)
{
	k_machreg_t contents = 0;
#if defined(__GNUC__)
	switch(regnum) {
	case 0:
		__asm__ ("movl %%db0,%0\n\t":"=r"(contents));
		break;
	case 1:
		__asm__ ("movl %%db1,%0\n\t":"=r"(contents));
		break;
	case 2:
		__asm__ ("movl %%db2,%0\n\t":"=r"(contents));
		break;
	case 3:
		__asm__ ("movl %%db3,%0\n\t":"=r"(contents));
		break;
	case 4:
	case 5:
		break;
	case 6:
		__asm__ ("movl %%db6,%0\n\t":"=r"(contents));
		break;
	case 7:
		__asm__ ("movl %%db7,%0\n\t":"=r"(contents));
		break;
	default:
		break;
	}
	  
#endif	/* __GNUC__ */
	return contents;
}


k_machreg_t
kdb_getcr(int regnum)
{
	k_machreg_t contents = 0;
#if defined(__GNUC__)
	switch(regnum) {
	case 0:
		__asm__ ("movl %%cr0,%0\n\t":"=r"(contents));
		break;
	case 1:
		break;
	case 2:
		__asm__ ("movl %%cr2,%0\n\t":"=r"(contents));
		break;
	case 3:
		__asm__ ("movl %%cr3,%0\n\t":"=r"(contents));
		break;
	case 4:
		__asm__ ("movl %%cr4,%0\n\t":"=r"(contents));
		break;
	default:
		break;
	}
	  
#endif	/* __GNUC__ */
	return contents;
}

void
kdb_putdr6(k_machreg_t contents)
{
	kdb_putdr(6, contents);
}

void
kdb_putdr7(k_machreg_t contents)
{
	kdb_putdr(7, contents);
}

void
kdb_putdr(int regnum, k_machreg_t contents)
{
#if defined(__GNUC__)
	switch(regnum) {
	case 0:
		__asm__ ("movl %0,%%db0\n\t"::"r"(contents));
		break;
	case 1:
		__asm__ ("movl %0,%%db1\n\t"::"r"(contents));
		break;
	case 2:
		__asm__ ("movl %0,%%db2\n\t"::"r"(contents));
		break;
	case 3:
		__asm__ ("movl %0,%%db3\n\t"::"r"(contents));
		break;
	case 4:
	case 5:
		break;
	case 6:
		__asm__ ("movl %0,%%db6\n\t"::"r"(contents));
		break;
	case 7:
		__asm__ ("movl %0,%%db7\n\t"::"r"(contents));
		break;
	default:
		break;
	}
#endif
}

/*
 * Symbol table functions.
 */

/*
 * kdbgetsym
 *
 *	Return the symbol table entry for the given symbol
 *
 * Parameters:
 * 	symname	Character string containing symbol name
 * Outputs:
 * Returns:
 *	NULL	Symbol doesn't exist
 *	ksp	Pointer to symbol table entry
 * Locking:
 *	None.
 * Remarks:
 */

__ksymtab_t *
kdbgetsym(const char *symname)
{
	__ksymtab_t *ksp = __kdbsymtab;
	int i;

	if (symname == NULL)
		return NULL;

	for (i=0; i<__kdbsymtabsize; i++, ksp++) {
		if (ksp->name && (strcmp(ksp->name, symname)==0)) {
			return ksp;
		}
	}

	return NULL;
}

/*
 * kdbgetsymval
 *
 *	Return the address of the given symbol.
 *
 * Parameters:
 * 	symname	Character string containing symbol name
 * Outputs:
 * Returns:
 *	0	Symbol name is NULL
 *	addr	Address corresponding to symname
 * Locking:
 *	None.
 * Remarks:
 */

unsigned long
kdbgetsymval(const char *symname)
{
	__ksymtab_t *ksp = kdbgetsym(symname);

	return (ksp?ksp->value:0);
}

/*
 * kdbaddmodsym
 *
 *	Add a symbol to the kernel debugger symbol table.  Called when
 *	a new module is loaded into the kernel.
 *
 * Parameters:
 * 	symname	Character string containing symbol name
 *	value	Value of symbol
 * Outputs:
 * Returns:
 *	0	Successfully added to table.
 *	1	Duplicate symbol
 *	2	Symbol table full
 * Locking:
 *	None.
 * Remarks:
 */

int
kdbaddmodsym(char *symname, unsigned long value) 
{

	/*
	 * Check for duplicate symbols.
	 */
	if (kdbgetsym(symname)) {
		printk("kdb: Attempt to register duplicate symbol '%s' @ 0x%lx\n",
			symname, value);
		return 1;
	}

	if (__kdbsymtabsize < __kdbmaxsymtabsize) {
		__ksymtab_t *ksp = &__kdbsymtab[__kdbsymtabsize++];
		
		ksp->name = symname;
		ksp->value = value;
		return 0;
	}

	/*
	 * No room left in kernel symbol table.
	 */
	{
		static int __kdbwarn = 0;

		if (__kdbwarn == 0) {
			__kdbwarn++;
			printk("kdb: Exceeded symbol table size.  Increase KDBMAXSYMTABSIZE in scripts/genkdbsym.awk\n");
		}
	}

	return 2;
}

/*
 * kdbdelmodsym
 *
 *	Add a symbol to the kernel debugger symbol table.  Called when
 *	a new module is loaded into the kernel.
 *
 * Parameters:
 * 	symname	Character string containing symbol name
 *	value	Value of symbol
 * Outputs:
 * Returns:
 *	0	Successfully added to table.
 *	1	Symbol not found
 * Locking:
 *	None.
 * Remarks:
 */

int
kdbdelmodsym(const char *symname)
{
	__ksymtab_t *ksp, *endksp;

	if (symname == NULL)
		return 1;

	/*
	 * Search for the symbol.  If found, move 
	 * all successive symbols down one position
	 * in the symbol table to avoid leaving holes.
	 */
	endksp = &__kdbsymtab[__kdbsymtabsize];
	for (ksp = __kdbsymtab; ksp < endksp; ksp++) {
		if (ksp->name && (strcmp(ksp->name, symname) == 0)) {
			endksp--;
			for ( ; ksp < endksp; ksp++) {
				*ksp = *(ksp + 1);
			}
			__kdbsymtabsize--;
			return 0;
		}
	}

	return 1;
}

/*
 * kdbnearsym
 *
 *	Return the name of the symbol with the nearest address
 *	less than 'addr'.
 *
 * Parameters:
 * 	addr	Address to check for symbol near
 * Outputs:
 * Returns:
 *	NULL	No symbol with address less than 'addr'
 *	symbol	Returns the actual name of the symbol.
 * Locking:
 *	None.
 * Remarks:
 */

char *
kdbnearsym(unsigned long addr)
{
	__ksymtab_t *ksp = __kdbsymtab;
	__ksymtab_t *kpp = NULL;
	int i;

	for(i=0; i<__kdbsymtabsize; i++, ksp++) {
		if (!ksp->name) 
			continue;

		if (addr == ksp->value) {
			kpp = ksp;
			break;
		}
		if (addr > ksp->value) {
			if ((kpp == NULL) 
			 || (ksp->value > kpp->value)) {
				kpp = ksp;
			}
		}
	}

	/*
	 * If more than 128k away, don't bother.
	 */
	if ((kpp == NULL)
	 || ((addr - kpp->value) > 0x20000)) {
		return NULL;
	}

	return kpp->name;
}

/*
 * kdbgetregcontents
 *
 *	Return the contents of the register specified by the 
 *	input string argument.   Return an error if the string
 *	does not match a machine register.
 *
 *	The following pseudo register names are supported:
 *	   &regs	 - Prints address of exception frame
 *	   kesp		 - Prints kernel stack pointer at time of fault
 *	   %<regname>	 - Uses the value of the registers at the 
 *			   last time the user process entered kernel
 *			   mode, instead of the registers at the time
 *			   kdb was entered.
 *
 * Parameters:
 *	regname		Pointer to string naming register
 *	regs		Pointer to structure containing registers.
 * Outputs:
 *	*contents	Pointer to unsigned long to recieve register contents
 * Returns:
 *	0		Success
 *	KDB_BADREG	Invalid register name
 * Locking:
 * 	None.
 * Remarks:
 *
 * 	Note that this function is really machine independent.   The kdb
 *	register list is not, however.
 */

static struct kdbregs {
	char   *reg_name;
	size_t	reg_offset;
} kdbreglist[] = {
	{ "eax",	offsetof(struct pt_regs, eax) },
	{ "ebx",	offsetof(struct pt_regs, ebx) },
	{ "ecx",	offsetof(struct pt_regs, ecx) },
	{ "edx",	offsetof(struct pt_regs, edx) },

	{ "esi",	offsetof(struct pt_regs, esi) },
	{ "edi",	offsetof(struct pt_regs, edi) },
	{ "esp",	offsetof(struct pt_regs, esp) },
	{ "eip",	offsetof(struct pt_regs, eip) },

	{ "ebp",	offsetof(struct pt_regs, ebp) },
	{ " ss", 	offsetof(struct pt_regs, xss) },
	{ " cs",	offsetof(struct pt_regs, xcs) },
	{ "eflags", 	offsetof(struct pt_regs, eflags) },

	{ " ds", 	offsetof(struct pt_regs, xds) },
	{ " es", 	offsetof(struct pt_regs, xes) },
	{ "origeax",	offsetof(struct pt_regs, orig_eax) },

};

static const int nkdbreglist = sizeof(kdbreglist) / sizeof(struct kdbregs);

int
kdbgetregcontents(const char *regname, 
		  struct pt_regs *regs,
		  unsigned long *contents)
{
	int i;

	if (strcmp(regname, "&regs") == 0) {
		*contents = (unsigned long)regs;
		return 0;
	}

	if (strcmp(regname, "kesp") == 0) {
		*contents = (unsigned long)regs + sizeof(struct pt_regs);
		return 0;
	}

	if (regname[0] == '%') {
		/* User registers:  %%e[a-c]x, etc */
		regname++;
		regs = (struct pt_regs *)
			(current->tss.esp0 - sizeof(struct pt_regs));
	}

	for (i=0; i<nkdbreglist; i++) {
		if (strnicmp(kdbreglist[i].reg_name, 
			     regname, 
			     strlen(regname)) == 0)
			break;
	}

	if ((i == nkdbreglist) 
	 || (strlen(kdbreglist[i].reg_name) != strlen(regname))) {
		return KDB_BADREG;
	}

	*contents = *(unsigned long *)((unsigned long)regs +
			kdbreglist[i].reg_offset);

	return 0;
}

/*
 * kdbsetregcontents
 *
 *	Set the contents of the register specified by the 
 *	input string argument.   Return an error if the string
 *	does not match a machine register.
 *
 *	Supports modification of user-mode registers via
 *	%<register-name>
 *
 * Parameters:
 *	regname		Pointer to string naming register
 *	regs		Pointer to structure containing registers.
 *	contents	Unsigned long containing new register contents
 * Outputs:
 * Returns:
 *	0		Success
 *	KDB_BADREG	Invalid register name
 * Locking:
 * 	None.
 * Remarks:
 */

int
kdbsetregcontents(const char *regname, 
		  struct pt_regs *regs,
		  unsigned long contents)
{
	int i;

	if (regname[0] == '%') {
		regname++;
		regs = (struct pt_regs *)
			(current->tss.esp0 - sizeof(struct pt_regs));
	}

	for (i=0; i<nkdbreglist; i++) {
		if (strnicmp(kdbreglist[i].reg_name, 
			     regname, 
			     strlen(regname)) == 0)
			break;
	}

	if ((i == nkdbreglist) 
	 || (strlen(kdbreglist[i].reg_name) != strlen(regname))) {
		return KDB_BADREG;
	}

	*(unsigned long *)((unsigned long)regs + kdbreglist[i].reg_offset) = 
		contents;

	return 0;
}

/*
 * kdbdumpregs
 *
 *	Dump the specified register set to the display.
 *
 * Parameters:
 *	regs		Pointer to structure containing registers.
 *	type		Character string identifying register set to dump
 *	extra		string further identifying register (optional)
 * Outputs:
 * Returns:
 *	0		Success
 * Locking:
 * 	None.
 * Remarks:
 *	This function will dump the general register set if the type
 *	argument is NULL (struct pt_regs).   The alternate register 
 *	set types supported by this function:
 *
 *	d 		Debug registers
 *	c		Control registers
 *	u		User registers at most recent entry to kernel
 * Following not yet implemented:
 *	m		Model Specific Registers (extra defines register #)
 *	r		Memory Type Range Registers (extra defines register)
 */

int
kdbdumpregs(struct pt_regs *regs,
	    const char *type,
	    const char *extra)
{
	int i;
	int count = 0;

	if (type 
	 && (type[0] == 'u')) {
		type = NULL;
		regs = (struct pt_regs *)
			(current->tss.esp0 - sizeof(struct pt_regs));
	}

	if (type == NULL) {
		for (i=0; i<nkdbreglist; i++) {
			kdb_printf("%s = 0x%8.8x  ", 
				   kdbreglist[i].reg_name, 
				   *(unsigned long *)((unsigned long)regs + 
						  kdbreglist[i].reg_offset));

			if ((++count % 4) == 0)
				kdb_printf("\n");
		}

		kdb_printf("&regs = 0x%8.8x\n", regs);

		return 0;
	}

	switch (type[0]) {
	case 'd':
	{
		unsigned long dr[8];

		for(i=0; i<8; i++) {
			if ((i == 4) || (i == 5)) continue;
			dr[i] = kdb_getdr(i);
		}
		kdb_printf("dr0 = 0x%8.8x  dr1 = 0x%8.8x  dr2 = 0x%8.8x  dr3 = 0x%8.8x\n",
			   dr[0], dr[1], dr[2], dr[3]);
		kdb_printf("dr6 = 0x%8.8x  dr7 = 0x%8.8x\n", 
			   dr[6], dr[7]);
		return 0;
	}
	case 'c':
	{
		unsigned long cr[5];

		for (i=0; i<5; i++) {
			cr[i] = kdb_getcr(i);
		}
		kdb_printf("cr0 = 0x%8.8x  cr1 = 0x%8.8x  cr2 = 0x%8.8x  cr3 = 0x%8.8x\ncr4 = 0x%8.8x\n",
			   cr[0], cr[1], cr[2], cr[3], cr[4]);
		return 0;
	}
	case 'm':
		break;
	case 'r':
		break;
	default:
		return KDB_BADREG;
	}

	/* NOTREACHED */
	return 0;
}

int
kdb_setjmp(kdb_jmp_buf *jb)
{
#if defined(CONFIG_KDB_FRAMEPTR)
	__asm__ ("movl 8(%esp), %eax\n\t"
		 "movl %ebx, 0(%eax)\n\t"
		 "movl %esi, 4(%eax)\n\t"
		 "movl %edi, 8(%eax)\n\t"
		 "movl (%esp), %ecx\n\t"
		 "movl %ecx, 12(%eax)\n\t"
		 "leal 8(%esp), %ecx\n\t"
		 "movl %ecx, 16(%eax)\n\t"
		 "movl 4(%esp), %ecx\n\t"
		 "movl %ecx, 20(%eax)\n\t");
#else	 /* CONFIG_KDB_FRAMEPTR */
	__asm__ ("movl 4(%esp), %eax\n\t"
		 "movl %ebx, 0(%eax)\n\t"
		 "movl %esi, 4(%eax)\n\t"
		 "movl %edi, 8(%eax)\n\t"
		 "movl %ebp, 12(%eax)\n\t"
		 "leal 4(%esp), %ecx\n\t"
		 "movl %ecx, 16(%eax)\n\t"
		 "movl 0(%esp), %ecx\n\t"
		 "movl %ecx, 20(%eax)\n\t");
#endif   /* CONFIG_KDB_FRAMEPTR */
	return 0;
}

void
kdb_longjmp(kdb_jmp_buf *jb, int reason)
{
#if defined(CONFIG_KDB_FRAMEPTR)
	__asm__("movl 8(%esp), %ecx\n\t"
		"movl 12(%esp), %eax\n\t"
		"movl 20(%ecx), %edx\n\t"
		"movl 0(%ecx), %ebx\n\t"
		"movl 4(%ecx), %esi\n\t"
		"movl 8(%ecx), %edi\n\t"
		"movl 12(%ecx), %ebp\n\t"
		"movl 16(%ecx), %esp\n\t"
		"jmp *%edx\n");
#else    /* CONFIG_KDB_FRAMEPTR */
	__asm__("movl 4(%esp), %ecx\n\t"
		"movl 8(%esp), %eax\n\t"
		"movl 20(%ecx), %edx\n\t"
		"movl 0(%ecx), %ebx\n\t"
		"movl 4(%ecx), %esi\n\t"
		"movl 8(%ecx), %edi\n\t"
		"movl 12(%ecx), %ebp\n\t"
		"movl 16(%ecx), %esp\n\t"
		"jmp *%edx\n");
#endif	 /* CONFIG_KDB_FRAMEPTR */
}

