/*
 * Minimalist Kernel Debugger 
 *
 * Copyright 1999, Silicon Graphics, Inc.
 *
 * Written March 1999 by Scott Lurndal at Silicon Graphics, Inc.
 *
 * Modifications from:
 *	Richard Bass			1999/07/20
 *		Many bug fixes and enhancements.
 *
 */

#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#if defined(__SMP__)
#include <linux/smp.h>
#endif
#include <linux/kdb.h>
#include <asm/system.h>
#include "kdbsupport.h"

int kdb_active = 0;
int kdb_flags = 0;
int kdb_nextline = 1;
int kdb_new_cpu = -1;

kdb_jmp_buf	kdbjmpbuf;

	/*
	 * Describe the command table.
	 */
typedef struct _kdbtab {
	char	*cmd_name;	/* Command name */
	kdb_func cmd_func;	/* Function to execute command */
	char	*cmd_usage;	/* Usage String for this command */
	char	*cmd_help;	/* Help message for this command */
	short	 cmd_flags;	/* Parsing flags */
	short	 cmd_minlen;	/* Minimum legal # command chars required */
} kdbtab_t;

	/*
	 * Provide space for KDB_MAX_COMMANDS commands.
 	 */
#define KDB_MAX_COMMANDS	100

static kdbtab_t kdb_commands[KDB_MAX_COMMANDS];

	/*
	 * Error messages.  XXX - use a macro for this...
	 */
static char kdb_notfound[]   = "Command Not Found";
static char kdb_argcount[]   = "Improper argument count, see usage.";
static char kdb_badwidth[]   = "Illegal value for BYTESPERWORD use 1, 2 or 4";
static char kdb_badradix[]   = "Illegal value for RADIX use 8, 10 or 16";
static char kdb_notenv[]     = "Cannot find environment variable";
static char kdb_noenvvalue[] = "Environment variable should have value";
static char kdb_notimp[]     = "Command not implemented";
static char kdb_envfull[]    = "Environment full";
static char kdb_envbuffull[] = "Environment buffer full";
static char kdb_toomanybpt[]  = "Too many breakpoints defined";
static char kdb_toomanydbregs[] = "More breakpoints than db registers defined";
static char kdb_dupbpt[]     = "Duplicate breakpoint address";
static char kdb_bptnotfound[] = "Breakpoint not found";
static char kdb_badmode[]    = "IDMODE should be x86 or 8086";
static char kdb_badint[]     = "Illegal numeric value";
static char kdb_invaddrfmt[] = "Invalid symbolic address format";
static char kdb_badreg[]     = "Invalid register name";
static char kdb_badcpunum[]  = "Invalid cpu number";
static char kdb_badlength[]  = "Invalid length field";
static char kdb_nobp[]       = "No Breakpoint exists";

static char *kdb_messages[] = {
	(char *)0,
	kdb_notfound, 
	(char *)0,
	kdb_argcount,
	kdb_badwidth,
	kdb_badradix,
	kdb_notenv,
	kdb_noenvvalue,
	kdb_notimp,
	kdb_envfull,
	kdb_envbuffull,
	kdb_toomanybpt,
	kdb_toomanydbregs,
	kdb_dupbpt,
	kdb_bptnotfound,
	kdb_badmode,
	kdb_badint,
	kdb_invaddrfmt,
	kdb_badreg,
	(char *)0,   /* KDB_CPUSWITCH */
	kdb_badcpunum,
	kdb_badlength,
	kdb_nobp,
	(char *)0
};
static const int __nkdb_err = sizeof(kdb_messages) / sizeof(char *);


/*
 * Initial environment.   This is all kept static and local to 
 * this file.   We don't want to rely on the memory allocation 
 * mechanisms in the kernel, so we use a very limited allocate-only
 * heap for new and altered environment variables.  The entire
 * environment is limited to a fixed number of entries (add more
 * to __env[] if required) and a fixed amount of heap (add more to
 * KDB_ENVBUFSIZE if required).
 */

#if defined(CONFIG_SMP)
static char prompt_str[] = "PROMPT=[%d]kdb> ";
static char more_str[] = "MOREPROMPT=[%d]more> ";
#else
static char prompt_str[] = "PROMPT=kdb> ";
static char more_str[] = "MOREPROMPT=more> ";
#endif
static char radix_str[] = "RADIX=16";
static char lines_str[] = "LINES=24";
static char columns_str[] = "COLUMNS=80";
static char mdcount_str[] = "MDCOUNT=8";	/* lines of md output */
static char idcount_str[] = "IDCOUNT=16";	/* lines of id output */
static char bperword_str[] = "BYTESPERWORD=4";	/* Word size for md output */
static char idmode_str[] = "IDMODE=x86";	/* 32-bit mode */
static char btargs_str[] = "BTARGS=5";		/* 5 possible args in bt */
static char sscount_str[] = "SSCOUNT=20";	/* lines of ssb output */

static char *__env[] = {
	bperword_str,
	columns_str,
	idcount_str,
	lines_str,
	mdcount_str,
	prompt_str, 
	radix_str, 
	idmode_str,
	btargs_str,
	sscount_str,
	more_str,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0,
	(char *)0
};
static const int __nenv = (sizeof(__env) / sizeof(char *));

/*
 * kdbgetenv
 *
 *	This function will return the character string value of
 *	an environment variable.
 *
 * Parameters:
 *	match	A character string representing an environment variable.
 * Outputs:
 *	None.
 * Returns:
 *	NULL	No environment variable matches 'match'
 *	char*	Pointer to string value of environment variable.
 * Locking:
 *	No locking considerations required.
 * Remarks:
 */
char *
kdbgetenv(const char *match)
{
	char **ep = __env;
	int    matchlen = strlen(match);
	int i;

	for(i=0; i<__nenv; i++) {
		char *e = *ep++;

		if (!e) continue;

		if ((strncmp(match, e, matchlen) == 0)
		 && ((e[matchlen] == '\0')
		   ||(e[matchlen] == '='))) {
			char *cp = strchr(e, '=');
			return (cp)?++cp:"";
		}
	}
	return (char *)0;
}

/*
 * kdballocenv
 *
 *	This function is used to allocate bytes for environment entries. 
 *
 * Parameters:
 *	match	A character string representing a numeric value
 * Outputs:
 *	*value  the unsigned long represntation of the env variable 'match'
 * Returns:
 *	Zero on success, a kdb diagnostic on failure.
 * Locking:
 *	No locking considerations required.  Must be called with all 
 *	processors halted.
 * Remarks:
 *	We use a static environment buffer (envbuffer) to hold the values
 *	of dynamically generated environment variables (see kdb_set).  Buffer
 *	space once allocated is never free'd, so over time, the amount of space
 *	(currently 512 bytes) will be exhausted if env variables are changed
 *	frequently.
 */
static char *
kdballocenv(size_t bytes)
{
#define	KDB_ENVBUFSIZE	512
	static char envbuffer[KDB_ENVBUFSIZE];
	static int  envbufsize = 0;
	char *ep = (char *)0;

	if ((KDB_ENVBUFSIZE - envbufsize) >= bytes) {
		ep = &envbuffer[envbufsize];
		envbufsize += bytes;
	}
	return ep;
}

/*
 * kdbgetulenv
 *
 *	This function will return the value of an unsigned long-valued
 *	environment variable.
 *
 * Parameters:
 *	match	A character string representing a numeric value
 * Outputs:
 *	*value  the unsigned long represntation of the env variable 'match'
 * Returns:
 *	Zero on success, a kdb diagnostic on failure.
 * Locking:
 *	No locking considerations required.
 * Remarks:
 */

int
kdbgetulenv(const char *match, unsigned long *value) 
{
	char *ep;

	ep = kdbgetenv(match);
	if (!ep) return KDB_NOTENV;
	if (strlen(ep) == 0) return KDB_NOENVVALUE;

	*value = simple_strtoul(ep, 0, 0);

	return 0;
}
	
/*
 * kdbgetintenv
 *
 *	This function will return the value of an integer-valued
 *	environment variable.
 *
 * Parameters:
 *	match	A character string representing an integer-valued env variable
 * Outputs:
 *	*value  the integer representation of the environment variable 'match'
 * Returns:
 *	Zero on success, a kdb diagnostic on failure.
 * Locking:
 *	No locking considerations required.
 * Remarks:
 */

int
kdbgetintenv(const char *match, int *value) {
	unsigned long val;
	int           diag;

	diag = kdbgetulenv(match, &val);
	if (!diag) {
		*value = (int) val;
	}
	return diag;
}

/*
 * kdbgetularg
 *
 *	This function will convert a numeric string
 *	into an unsigned long value.
 *
 * Parameters:
 *	arg	A character string representing a numeric value
 * Outputs:
 *	*value  the unsigned long represntation of arg.
 * Returns:
 *	Zero on success, a kdb diagnostic on failure.
 * Locking:
 *	No locking considerations required.
 * Remarks:
 */

int
kdbgetularg(const char *arg, unsigned long *value)
{
	char *endp;
	unsigned long val;

	val = simple_strtoul(arg, &endp, 0);

	if (endp == arg) {
		/*
		 * Try base 16, for us folks too lazy to type the
		 * leading 0x...
		 */
		val = simple_strtoul(arg, &endp, 16);
		if (endp == arg)
			return KDB_BADINT;
	}

	*value = val;

	return 0;
}

/*
 * kdbgetaddrarg
 *
 *	This function is responsible for parsing an
 *	address-expression and returning the value of
 *	the expression, symbol name, and offset to the caller. 
 *
 *	The argument may consist of a numeric value (decimal or
 *	hexidecimal), a symbol name, a register name (preceeded
 *	by the percent sign), an environment variable with a numeric
 *	value (preceeded by a dollar sign) or a simple arithmetic
 *	expression consisting of a symbol name, +/-, and a numeric
 *	constant value (offset).
 *
 * Parameters:
 *	argc	- count of arguments in argv
 *	argv	- argument vector
 *	*nextarg - index to next unparsed argument in argv[]
 *
 * Outputs:
 *	*value	- receives the value of the address-expression
 *	*offset - receives the offset specified, if any
 *	*name   - receives the symbol name, if any
 *	*nextarg - index to next unparsed argument in argv[]
 *
 * Returns:
 *	zero is returned on success, a kdb diagnostic code is 
 *      returned on error.
 *
 * Locking:
 *	No locking requirements.
 *
 * Remarks:
 *
 */

int
kdbgetaddrarg(int argc, const char **argv, int *nextarg, 
	      unsigned long *value,  long *offset, 
	      char **name, struct pt_regs *regs)
{
	unsigned long addr;
	long	      off = 0;
	int	      positive;
	int	      diag;
	char	     *symname;
	char	      symbol = '\0';
	char	     *cp;

	/*
	 * Process arguments which follow the following syntax:
	 *
	 *  symbol | numeric-address [+/- numeric-offset]
	 *  %register
	 *  $environment-variable
	 */

	if (*nextarg > argc) {
		return KDB_ARGCOUNT;
	}

	symname = (char *)argv[*nextarg];

	/*
	 * If there is no whitespace between the symbol
	 * or address and the '+' or '-' symbols, we
	 * remember the character and replace it with a
	 * null so the symbol/value can be properly parsed
	 */
	if ((cp = strpbrk(symname, "+-")) != NULL) {
		symbol = *cp;
		*cp++ = '\0';
	}

	if (symname[0] == '$') {
		diag = kdbgetulenv(&symname[1], &addr);
		if (diag)
			return diag;
	} else if (symname[0] == '%') {
		diag = kdbgetregcontents(&symname[1], regs, &addr);
		if (diag) 
			return diag;
	} else {
		addr = kdbgetsymval(symname);
		if (addr == 0) {
			diag = kdbgetularg(argv[*nextarg], &addr);
			if (diag) 
				return diag;
		}
	}

	symname = kdbnearsym(addr);

	(*nextarg)++;

	if (name)
		*name = symname; 
	if (value) 
		*value = addr;
	if (offset && name && *name)
		*offset = addr - kdbgetsymval(*name);
	
	if ((*nextarg > argc) 
	 && (symbol == '\0'))
		return 0;

	/*
	 * check for +/- and offset
	 */

	if (symbol == '\0') {
		if ((argv[*nextarg][0] != '+')
	  	 && (argv[*nextarg][0] != '-')) {
			/*
			 * Not our argument.  Return.
			 */
			return 0;
		} else {
			positive = (argv[*nextarg][0] == '+');
			(*nextarg)++;
		}
	} else
		positive = (symbol == '+');

	/*
	 * Now there must be an offset!
	 */
	if ((*nextarg > argc) 
	 && (symbol == '\0')) {
		return KDB_INVADDRFMT;
	}

	if (!symbol) {
		cp = (char *)argv[*nextarg];
		(*nextarg)++;
	}

	diag = kdbgetularg(cp, &off);
	if (diag)
		return diag;

	if (!positive) 
		off = -off;

	if (offset) 
		*offset += off;

	if (value) 
		*value += off;

	return 0;
}

static void
kdb_cmderror(int diag)
{
	if (diag >= 0) {
		kdb_printf("no error detected\n");
		return;
	}
	diag = -diag;
	if (diag >= __nkdb_err) {
		kdb_printf("Illegal diag %d\n", -diag);
		return;
	}
	kdb_printf("diag: %d: %s\n", diag, kdb_messages[diag]);
}

/*
 * kdb_parse
 *
 *	Parse the command line, search the command table for a 
 *	matching command and invoke the command function.
 *	
 * Parameters:
 *      cmdstr	The input command line to be parsed.
 *	regs	The registers at the time kdb was entered.
 * Outputs:
 *	None.
 * Returns:
 *	Zero for success, a kdb diagnostic if failure.
 * Locking:
 * 	None.
 * Remarks:
 *	Limited to 20 tokens.
 *
 *	Real rudimentary tokenization. Basically only whitespace
 *	is considered a token delimeter (but special consideration
 *	is taken of the '=' sign as used by the 'set' command).
 *
 *	The algorithm used to tokenize the input string relies on
 *	there being at least one whitespace (or otherwise useless)
 *	character between tokens as the character immediately following
 *	the token is altered in-place to a null-byte to terminate the
 *	token string.
 */

#define MAXARGC	20

static int
kdb_parse(char *cmdstr, struct pt_regs *regs)
{
	char *argv[MAXARGC];
	int  argc=0;
	char *cp;
	kdbtab_t *tp;
	int i;

	/*
	 * First tokenize the command string.   
	 */
	cp = cmdstr;

	/*
	 * If a null statement is provided, do nothing.
	 */
	if ((*cp == '\n') || (*cp == '\0'))
		return 0;

	while (*cp) {
		/* skip whitespace */
		while (isspace(*cp)) cp++;
		if ((*cp == '\0') || (*cp == '\n'))
			break;
		argv[argc++] = cp;
		/* Skip to next whitespace */
		for(; *cp && (!isspace(*cp) && (*cp != '=')); cp++);
		*cp++ = '\0';		/* Squash a ws or '=' character */
	}

	for(tp=kdb_commands, i=0; i < KDB_MAX_COMMANDS; i++,tp++) {
		if (tp->cmd_name) {
			/*
			 * If this command is allowed to be abbreviated, 
			 * check to see if this is it.
			 */

			if (tp->cmd_minlen
			 && (strlen(argv[0]) <= tp->cmd_minlen)) {
				if (strncmp(argv[0], 
					    tp->cmd_name, 
				    	    tp->cmd_minlen) == 0) {
					break;
				}
			}

			if (strcmp(argv[0], tp->cmd_name)==0) {
				break;
			}
		}
	}

	if (i < KDB_MAX_COMMANDS) {
		return (*tp->cmd_func)(argc-1, 
				       (const char**)argv, 
				       (const char**)__env, 
				       regs);
	}

	/*
	 * If the input with which we were presented does not
	 * map to an existing command, attempt to parse it as an 
	 * address argument and display the result.   Useful for
	 * obtaining the address of a variable, or the nearest symbol
	 * to an address contained in a register.
	 */
	{
		unsigned long value;
		char *name = NULL;
		long offset;
		int nextarg = 0;

		if (kdbgetaddrarg(0, (const char **)argv, &nextarg, 
				  &value, &offset, &name, regs)) {
			return KDB_NOTFOUND;
		}

		kdb_printf("%s = 0x%8.8x  ", argv[0], value);
		if (name) {
			kdb_printf("(%s+0x%lx)", name, offset);
		}
		kdb_printf("\n");
		return 0;
	}
}

/*
 * kdb
 *
 * 	This function is the entry point for the kernel debugger.  It
 *	provides a command parser and associated support functions to 
 *	allow examination and control of an active kernel.
 *
 * 	This function may be invoked directly from any
 *	point in the kernel by calling with reason == KDB_REASON_ENTER
 *	(XXX - note that the regs aren't set up this way - could
 *	       use a software interrupt to enter kdb to get regs...)
 *
 *	The breakpoint trap code should invoke this function with
 *	one of KDB_REASON_BREAK (int 03) or KDB_REASON_DEBUG (debug register)
 *
 *	the panic function should invoke this function with 
 *	KDB_REASON_PANIC.
 *
 *	The kernel fault handler should invoke this function with
 *	reason == KDB_REASON_FAULT and error == trap vector #.
 *
 * Inputs:
 *	reason		The reason KDB was invoked
 *	error		The hardware-defined error code
 *	regs		The registers at time of fault/breakpoint
 * Outputs:
 *	none
 * Locking:
 *	none
 * Remarks:
 *	No assumptions of system state.  This function may be invoked
 *	with arbitrary locks held.  It will stop all other processors
 *	in an SMP environment, disable all interrupts and does not use
 *	the operating systems keyboard driver.
 */

int
kdb(int reason, int error, struct pt_regs *regs)
{
	char	cmdbuf[255];
	char	*cmd;
	int	diag;
	unsigned long flags;
	struct  pt_regs func_regs;

	kdb_new_cpu = -1;

	/*
	 * Remove the breakpoints to prevent double-faults
	 * if kdb happens to use a function where a breakpoint
	 * has been enabled.
	 */

	kdb_bp_remove();

	if (reason != KDB_REASON_DEBUG) {
		kdb_printf("Entering kdb ");
#if defined(__SMP__)
		kdb_printf("on processor %d ", smp_processor_id());
#endif
	}

	switch (reason) {
	case KDB_REASON_DEBUG:
		/*
		 * If re-entering kdb after a single step
		 * command, don't print the message.
		 */
		diag = kdb_db_trap(regs);
		if (diag == 0) {
			kdb_printf("Entering kdb ");
#if defined(__SMP__)
			kdb_printf("on processor %d ", smp_processor_id());
#endif
		} else if (diag == 2) {
			/*
			 * in middle of ssb command.  Just return.
			 */
			return 0;
		}
		break;
	case KDB_REASON_FAULT:
		break;
	case KDB_REASON_INT:
		kdb_printf("due to KDB_ENTER() call\n");
		break;
	case KDB_REASON_KEYBOARD:
		kdb_printf("due to Keyboard Entry\n");
		break;
	case KDB_REASON_SWITCH:
		kdb_printf("due to cpu switch\n");
		break;
	case KDB_REASON_ENTER:
		kdb_printf("due to function call\n");
		regs = &func_regs;
		regs->xcs = 0;
#if defined(CONFIG_KDB_FRAMEPTR)
		asm volatile("movl %%ebp,%0":"=m" (*(int *)&regs->ebp));
#endif
		asm volatile("movl %%esp,%0":"=m" (*(int *)&regs->esp));
		regs->eip = (long) &kdb;	/* for traceback. */
		break;
	case KDB_REASON_PANIC:
		kdb_printf("due to panic @ 0x%8.8x\n", regs->eip);
		kdbdumpregs(regs, NULL, NULL);
		break;
	case KDB_REASON_BREAK:
		kdb_printf("due to Breakpoint @ 0x%8.8x\n", regs->eip);
		break;
	default:
		break;
	}

#if defined(__SMP__)
	/*
	 * If SMP, stop other processors
	 */
	if (smp_num_cpus > 1) {
		/*
		 * Stop all other processors
		 */
		smp_kdb_stop(1);
	}
#endif	/* __SMP__ */

	/*
	 * Disable interrupts during kdb command processing 
	 */
	__save_flags(flags);
	__cli();

	while (1) {
		/*
		 * Initialize pager context.
		 */
		kdb_nextline = 1;

		/*
		 * Use kdb_setjmp/kdb_longjmp to break out of 
		 * the pager early.
		 */
		if (kdb_setjmp(&kdbjmpbuf)) {
			/*
			 * Command aborted (usually in pager)
			 */

			/*
		 	 * XXX - need to abort a SSB ?
			 */
			continue;
		}

		/*
		 * Fetch command from keyboard
		 */
		cmd = kbd_getstr(cmdbuf, sizeof(cmdbuf), kdbgetenv("PROMPT"));

		diag = kdb_parse(cmd, regs);
		if (diag == KDB_NOTFOUND) {
			kdb_printf("Unknown kdb command: '%s'\n", cmd);
			diag = 0;
		}
		if ((diag == KDB_GO)
		 || (diag == KDB_CPUSWITCH))
			break;	/* Go or cpu switch command */

		if (diag)
			kdb_cmderror(diag);
	}

	/*
	 * Set up debug registers.
	 */
	kdb_bp_install();

#if defined(__SMP__)
	if ((diag == KDB_CPUSWITCH)
	 && (kdb_new_cpu != -1)) {
		/*
		 * Leaving the other CPU's at the barrier, except the
		 * one we are switching to, we'll send ourselves a 
		 * kdb IPI before allowing interrupts so it will get
		 * caught ASAP and get this CPU back waiting at the barrier.
		 */
		
		smp_kdb_stop(0);	/* Stop ourself */

		/*
	 	 * let the new cpu go.
		 */
		clear_bit(kdb_new_cpu, &smp_kdb_wait);
	} else {
		/*
		 * Let the other processors continue.
		 */
		smp_kdb_wait = 0;
	}
#endif

	kdb_flags &= ~(KDB_FLAG_SUPRESS|KDB_FLAG_FAULT);

	__restore_flags(flags);


	return 0;
}

/*
 * kdb_md
 *
 *	This function implements the 'md' and 'mds' commands.
 *
 *	md|mds  [<addr arg> [<line count> [<radix>]]]
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
 */

int
kdb_md(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	char fmtchar;
	char fmtstr[64];
	int radix, count, width;
	unsigned long addr;
	unsigned long word;
	long	offset = 0;
	int	diag;
	int	nextarg;
	static unsigned long lastaddr = 0;
	static unsigned long lastcount = 0;
	static unsigned long lastradix = 0;
	char	lastbuf[50];
	int	symbolic = 0;

	/*
	 * Defaults in case the relevent environment variables are unset
	 */
	radix = 16;
	count = 8;
	width = 4;

	if (argc == 0) {
		if (lastaddr == 0)
			return KDB_ARGCOUNT;
		sprintf(lastbuf, "0x%lx", lastaddr);
		argv[1] = lastbuf;
		argc = 1;
		count = lastcount;
		radix = lastradix;
	} else {
		unsigned long val;

		if (argc >= 2) { 

			diag = kdbgetularg(argv[2], &val);
			if (!diag) 
				count = (int) val;
		} else {
			diag = kdbgetintenv("MDCOUNT", &count);
		}

		if (argc >= 3) {
			diag = kdbgetularg(argv[3], &val);
			if (!diag) 
				radix = (int) val;
		} else {
			diag = kdbgetintenv("RADIX",&radix);
		}
	}

	switch (radix) {
	case 10:
		fmtchar = 'd';
		break;
	case 16:
		fmtchar = 'x';
		break;
	case 8:
		fmtchar = 'o';
		break;
	default:
		return KDB_BADRADIX;
	}

	diag = kdbgetintenv("BYTESPERWORD", &width);

	if (strcmp(argv[0], "mds") == 0) {
		symbolic = 1;
		width = 4;
	}

	switch (width) {
	case 4:
		sprintf(fmtstr, "%%8.8%c ", fmtchar);
		break;
	case 2:
		sprintf(fmtstr, "%%4.4%c ", fmtchar);
		break;
	case 1:
		sprintf(fmtstr, "%%2.2%c ", fmtchar);
		break;
	default:
		return KDB_BADWIDTH;
	}

	
	nextarg = 1;
	diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL, regs);
	if (diag)
		return diag;

	/* Round address down modulo BYTESPERWORD */
	
	addr &= ~(width-1);

	/*
	 * Remember count and radix for next 'md'
	 */
	lastcount = count;
	lastradix = radix;

	while (count--) {
		int	num = (symbolic?1 :(16 / width));
		char	cbuf[32];
		char	*c = cbuf;
		char	t;
		int     i;
	
		for(i=0; i<sizeof(cbuf); i++) {
			cbuf[i] = '\0';
		}

		kdb_printf("%8.8x: ", addr);
	
		for(i=0; i<num; i++) {
			char *name = NULL;

			word = kdbgetword(addr, width);
			if (kdb_flags & KDB_FLAG_SUPRESS) {
				kdb_flags &= ~KDB_FLAG_SUPRESS;
				return 0;  /* Error message already printed */
			}

			kdb_printf(fmtstr, word);
			if (symbolic) {
				name = kdbnearsym(word);
			}
			if (name) {
				unsigned long offset;
	
				offset = word - kdbgetsymval(name);
				kdb_printf("%s+0x%x", name, offset);
				addr += 4;
			} else {
				switch (width) {
				case 4:
					*c++ = isprint(t=kdbgetword(addr++, 1))
							?t:'.'; 
					*c++ = isprint(t=kdbgetword(addr++, 1))
							?t:'.'; 
				case 2:
					*c++ = isprint(t=kdbgetword(addr++, 1))
							?t:'.'; 
				case 1:
					*c++ = isprint(t=kdbgetword(addr++, 1))
							?t:'.'; 
					break;
				}
			}
		}
		kdb_printf(" %s\n", cbuf);
	}

	lastaddr = addr;

	return 0;
}

/*
 * kdb_mm
 *
 *	This function implements the 'mm' command.
 *
 *	mm address-expression new-value
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
 *	Restricted to working on 4-byte words at this time.
 */

int
kdb_mm(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	int diag;
	unsigned long addr;
	long 	      offset = 0;
	unsigned long contents;
	unsigned long word;
	int nextarg;

	if (argc != 2) {
		return KDB_ARGCOUNT;
	}

	nextarg = 1;
	diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL, regs);
	if (diag)
		return diag;

	if (nextarg > argc) 
		return KDB_ARGCOUNT;

	diag = kdbgetaddrarg(argc, argv, &nextarg, &contents, NULL, NULL, regs);
	if (diag)
		return diag;

	if (nextarg != argc + 1)
		return KDB_ARGCOUNT;

	/*
	 * To prevent modification of invalid addresses, check first.
	 */
	word = kdbgetword(addr, sizeof(word));
	if (kdb_flags & KDB_FLAG_SUPRESS) {
		kdb_flags &= ~KDB_FLAG_SUPRESS;
		return 0;
	}

	*(unsigned long *)(addr) = contents;

	kdb_printf("0x%x = 0x%x\n", addr, contents);

	return 0;
}

/*
 * kdb_go
 *
 *	This function implements the 'go' command.
 *
 *	go [address-expression]
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
 */

int
kdb_go(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	unsigned long addr;
	int diag;
	int nextarg;
	long offset;

	if (argc == 1) {
		nextarg = 1;
		diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL, regs);
		if (diag)
			return diag;

		regs->eip = addr;
	} else if (argc) 
		return KDB_ARGCOUNT;

	return KDB_GO;
}

/*
 * kdb_rd
 *
 *	This function implements the 'rd' command.
 *
 *	rd		display all general registers.
 *	rd  c		display all control registers.
 *	rd  d		display all debug registers.
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
 */

int
kdb_rd(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	/*
	 */
	
	if (argc == 0) {
		return kdbdumpregs(regs, NULL, NULL);
	} 

	if (argc > 2) {
		return KDB_ARGCOUNT;
	}

	return kdbdumpregs(regs, argv[1], argv[2]);
}

/*
 * kdb_rm
 *
 *	This function implements the 'rm' (register modify)  command.
 *
 *	rm register-name new-contents
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
 *	Currently doesn't allow modification of control or
 *	debug registers, nor does it allow modification 
 *	of model-specific registers (MSR).
 */

int
kdb_rm(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	int diag;
	int ind = 0;
	unsigned long contents;

	if (argc != 2) {
		return KDB_ARGCOUNT;
	}

	/*
	 * Allow presence or absence of leading '%' symbol.
	 */

	if (argv[1][0] == '%')
		ind = 1;

	diag = kdbgetularg(argv[2], &contents);
	if (diag)
		return diag;

	diag = kdbsetregcontents(&argv[1][ind], regs, contents);
	if (diag)
		return diag;

	return 0;
}

/*
 * kdb_ef
 *
 *	This function implements the 'ef' (display exception frame) 
 *	command.  This command takes an address and expects to find
 *	an exception frame at that address, formats and prints it.
 *
 *	ef address-expression
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
 *	Not done yet.
 */

int
kdb_ef(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{

	return KDB_NOTIMP;
}

/*
 * kdb_reboot
 *
 *	This function implements the 'reboot' command.  Reboot the system
 *	immediately.
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
 *	Shouldn't return from this function.
 */

int 
kdb_reboot(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	machine_restart(0);
	/* NOTREACHED */
	return 0;
}

/*
 * kdb_env
 *
 *	This function implements the 'env' command.  Display the current
 *	environment variables.
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
 */

int
kdb_env(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	int i;

	for(i=0; i<__nenv; i++) {
		if (__env[i]) {
			kdb_printf("%s\n", __env[i]);
		}
	}

	return 0;
}

/*
 * kdb_set
 *
 *	This function implements the 'set' command.  Alter an existing
 *	environment variable or create a new one.
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
 */

int
kdb_set(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	int i;
	char *ep;
	size_t varlen, vallen;

	/*
	 * we can be invoked two ways:
	 *   set var=value    argv[1]="var", argv[2]="value"
	 *   set var = value  argv[1]="var", argv[2]="=", argv[3]="value"
	 * - if the latter, shift 'em down.
	 */
	if (argc == 3) {
		argv[2] = argv[3];
		argc--;
	}

	if (argc != 2) 
		return KDB_ARGCOUNT;

	/*
	 * Tokenizer squashed the '=' sign.  argv[1] is variable
	 * name, argv[2] = value.
	 */
	varlen = strlen(argv[1]);
	vallen = strlen(argv[2]);
	ep = kdballocenv(varlen + vallen + 2);
	if (ep == (char *)0) 
		return KDB_ENVBUFFULL;

	sprintf(ep, "%s=%s", argv[1], argv[2]);

	ep[varlen+vallen+1]='\0';

	for(i=0; i<__nenv; i++) {
		if (__env[i]
		 && ((strncmp(__env[i], argv[1], varlen)==0)
		   && ((__env[i][varlen] == '\0')
		    || (__env[i][varlen] == '=')))) {
			__env[i] = ep;
			return 0;
		}
	}
	
	/*
	 * Wasn't existing variable.  Fit into slot.
	 */
	for(i=0; i<__nenv-1; i++) {
		if (__env[i] == (char *)0) {
			__env[i] = ep;
			return 0;
		}
	}

	return KDB_ENVFULL;
}

#if defined(__SMP__)
/*
 * kdb_cpu
 *
 *	This function implements the 'cpu' command.
 *
 *	cpu	[<cpunum>]
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
 */

int
kdb_cpu(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	unsigned long cpunum;
	int diag;

	if (argc == 0) {
		int i;

		kdb_printf("Currently on cpu %d\n", smp_processor_id());
		kdb_printf("Available cpus: ");
		for (i=0; i<NR_CPUS; i++) {
			if (test_bit(i, &cpu_online_map)) {
				if (i) kdb_printf(", ");
				kdb_printf("%d", i);
			}
		}
		kdb_printf("\n");
		return 0;
	}

	if (argc != 1) 
		return KDB_ARGCOUNT;

	diag = kdbgetularg(argv[1], &cpunum);
	if (diag)
		return diag;

	/*
	 * Validate cpunum
	 */
	if ((cpunum > NR_CPUS)
	 || !test_bit(cpunum, &cpu_online_map))
		return KDB_BADCPUNUM;

	kdb_new_cpu = cpunum;

	/*
	 * Switch to other cpu
	 */
	return KDB_CPUSWITCH;
}
#endif	/* __SMP__ */

/*
 * kdb_ps
 *
 *	This function implements the 'ps' command which shows
 *	a list of the active processes.
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
 */

int
kdb_ps(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	struct task_struct	*p;

	kdb_printf("Task Addr     Pid       Parent   cpu  lcpu    Tss     Command\n");
	for_each_task(p) {
		kdb_printf("0x%8.8x %10.10d %10.10d %4.4d %4.4d 0x%8.8x %s\n",
			   p, p->pid, p->p_pptr->pid, p->processor, 
			   p->last_processor,
			   &p->tss,
			   p->comm);
	}

	return 0;
}

/*
 * kdb_ll
 *
 *	This function implements the 'll' command which follows a linked
 *	list and executes an arbitrary command for each element.
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
 */

int
kdb_ll(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	int diag;
	unsigned long addr, firstaddr;
	long 	      offset = 0;
	unsigned long va;
	unsigned long linkoffset;
	int nextarg;

	if (argc != 3) {
		return KDB_ARGCOUNT;
	}

	nextarg = 1;
	diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL, regs);
	if (diag)
		return diag;

	diag = kdbgetularg(argv[2], &linkoffset);
	if (diag)
		return diag;

	/*
	 * Using the starting address as the first element in the list,
	 * and assuming that the list ends with a null pointer or with
	 * the first element again (do the right thing for both
	 * null-terminated and circular lists).
	 */

	va = firstaddr = addr;

	while (va) {
		char buf[80];

		sprintf(buf, "%s 0x%lx\n", argv[3], va);
		diag = kdb_parse(buf, regs);
		if (diag)
			return diag;

		addr = va + linkoffset;
		va = kdbgetword(addr, sizeof(va));
		if (kdb_flags & KDB_FLAG_SUPRESS) {
			kdb_flags &= ~KDB_FLAG_SUPRESS;
			return 0;
		}
		if (va == firstaddr)
			break;
	}

	return 0;
}

/*
 * kdb_help
 *
 *	This function implements the 'help' and '?' commands.
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
 */

int
kdb_help(int argc, const char **argv, const char **envp, struct pt_regs *regs)
{
	kdbtab_t *kt;

	kdb_printf("%-15.15s %-20.20s %s\n", "Command", "Usage", "Description");
	kdb_printf("----------------------------------------------------------\n");
	for(kt=kdb_commands; kt->cmd_name; kt++) {
		kdb_printf("%-15.15s %-20.20s %s\n", kt->cmd_name, 
			kt->cmd_usage, kt->cmd_help);
	}
	return 0;
}

/*
 * kdb_register
 *
 *	This function is used to register a kernel debugger command.
 *
 * Inputs:
 *	cmd	Command name
 *	func	Function to execute the command
 *	usage	A simple usage string showing arguments
 *	help	A simple help string describing command
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, one if a duplicate command.
 * Locking:
 *	none.
 * Remarks:
 *
 */

int
kdb_register(char *cmd, 
	     kdb_func func, 
	     char *usage,
	     char *help, 
	     short minlen)
{
	int i;
	kdbtab_t *kp;

	/*
	 *  Brute force method to determine duplicates 
	 */
	for (i=0, kp=kdb_commands; i<KDB_MAX_COMMANDS; i++, kp++) {
		if (kp->cmd_name && (strcmp(kp->cmd_name, cmd)==0)) {
			kdb_printf("Duplicate kdb command registered: '%s'\n",
				   cmd);
			return 1;
		}
	}

	/*
	 * Insert command into first available location in table
	 */
	for (i=0, kp=kdb_commands; i<KDB_MAX_COMMANDS; i++, kp++) {
		if (kp->cmd_name == NULL) {
			kp->cmd_name   = cmd;
			kp->cmd_func   = func;
			kp->cmd_usage  = usage;
			kp->cmd_help   = help;
			kp->cmd_flags  = 0;
			kp->cmd_minlen = minlen;
			break;
		}
	}
	return 0;
}

/*
 * kdb_unregister
 *
 *	This function is used to unregister a kernel debugger command.
 *	It is generally called when a module which implements kdb
 *	commands is unloaded.
 *
 * Inputs:
 *	cmd	Command name
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, one command not registered.
 * Locking:
 *	none.
 * Remarks:
 *
 */

int
kdb_unregister(char *cmd) 
{
	int i;
	kdbtab_t *kp;

	/*
	 *  find the command.
	 */
	for (i=0, kp=kdb_commands; i<KDB_MAX_COMMANDS; i++, kp++) {
		if (kp->cmd_name && (strcmp(kp->cmd_name, cmd)==0)) {
			kp->cmd_name = NULL;
			return 0;
		}
	}

	/*
	 * Couldn't find it.
	 */
	return 1;
}

/*
 * kdb_inittab
 *
 *	This function is called by the kdb_init function to initialize
 *	the kdb command table.   It must be called prior to any other
 *	call to kdb_register.
 *
 * Inputs:
 *	None.
 * Outputs:
 *	None.
 * Returns:
 *	None.
 * Locking:
 *	None.
 * Remarks:
 *
 */

void
kdb_inittab(void)
{
	int i;
	kdbtab_t *kp;

	for(i=0, kp=kdb_commands; i < KDB_MAX_COMMANDS; i++,kp++) {
		kp->cmd_name = NULL;
	}

	kdb_register("md", kdb_md, "<vaddr>",   "Display Memory Contents", 1);
	kdb_register("mds", kdb_md, "<vaddr>", 	"Display Memory Symbolically", 0);
	kdb_register("mm", kdb_mm, "<vaddr> <contents>",   "Modify Memory Contents", 0);
	kdb_register("id", kdb_id, "<vaddr>",   "Display Instructions", 1);
	kdb_register("go", kdb_go, "[<vaddr>]", "Continue Execution", 1);
	kdb_register("rd", kdb_rd, "",		"Display Registers", 1);
	kdb_register("rm", kdb_rm, "<reg> <contents>", "Modify Registers", 0);
	kdb_register("ef", kdb_ef, "<vaddr>",   "Display exception frame", 0);
	kdb_register("bt", kdb_bt, "[<vaddr>]", "Stack traceback", 1);
	kdb_register("btp", kdb_bt, "<pid>", 	"Display stack for process <pid>", 0);
	kdb_register("bp", kdb_bp, "[<vaddr>]", "Set/Display breakpoints", 0);
	kdb_register("bl", kdb_bp, "[<vaddr>]", "Display breakpoints", 0);
	kdb_register("bpa", kdb_bp, "[<vaddr>]", "Set/Display global breakpoints", 0);
	kdb_register("bc", kdb_bc, "<bpnum>",   "Clear Breakpoint", 0);
	kdb_register("be", kdb_bc, "<bpnum>",   "Enable Breakpoint", 0);
	kdb_register("bd", kdb_bc, "<bpnum>",   "Disable Breakpoint", 0);
	kdb_register("ss", kdb_ss, "[<#steps>]", "Single Step", 1);
	kdb_register("ssb", kdb_ss, "", 	"Single step to branch/call", 0);
	kdb_register("ll", kdb_ll, "<first-element> <linkoffset> <cmd>", "Execute cmd for each element in linked list", 0);
	kdb_register("env", kdb_env, "", 	"Show environment variables", 0);
	kdb_register("set", kdb_set, "", 	"Set environment variables", 0);
	kdb_register("help", kdb_help, "", 	"Display Help Message", 1);
	kdb_register("?", kdb_help, "",         "Display Help Message", 0);
#if defined(__SMP__)
	kdb_register("cpu", kdb_cpu, "<cpunum>","Switch to new cpu", 0);
#endif	/* __SMP__ */
	kdb_register("ps", kdb_ps, "", 		"Display active task list", 0);
	kdb_register("reboot", kdb_reboot, "",  "Reboot the machine immediately", 0);
}

