/*
 *	FILE    	cmptblty.h
 *
 *	Version 	1.0
 *	Author  	Copyright (c) Marc A. Viredaz, 1998
 *	        	DEC Western Research Laboratory, Palo Alto, CA
 *	Date    	November 1997 (April 1997)
 *	System  	Advanced RISC Machine (ARM)
 *	Language	C or ARM Assembly
 *	Purpose 	Definition of macros to achieve compatibility between
 *	        	the ARM assembly language and C language syntax.
 *
 *	        	Language-specific definitions are selected by the
 *	        	macro "LANGUAGE", which should be defined as either
 *	        	"C" (default) or "Assembly".
 */


#ifndef CMPTBLTY
#define CMPTBLTY

#ifndef LANGUAGE
#define LANGUAGE	C
#endif /* !defined (LANGUAGE) */

#define C       	0
#define Assembly	1


/*
 * General definitions
 */

#undef FALSE
#undef TRUE
#if LANGUAGE == C
#define FALSE   	0
#define TRUE    	(!FALSE)
#elif LANGUAGE == Assembly
#define FALSE   	0
#define TRUE    	1
#endif /* LANGUAGE == C || LANGUAGE == Assembly */

#ifndef NULL
#if LANGUAGE == C && !defined(__cplusplus)
#define NULL    	((void *) 0)
#else
#define NULL    	0
#endif
#endif

/*
 * MACROS: BAnd, BOr, BXor, Modulo
 *
 * Purpose
 *    The ARM assembly language and C language use a different syntax for the
 *    bit-wise AND, OR, and XOR operations and the modulo operation. The
 *    macros "BAnd", "BOr", "BXor", and "Modulo" allow to write common
 *    definitions to be used both in assembly and C programs.
 *
 * Note
 *    Although the ARM assembly syntax for the bit-wise NOT operation and the
 *    left and right shift operations is also different from the
 *    corresponding C syntax, the assembler seems to recognize the latter.
 *    Therefore, no macros are provided for these operations. This holds also
 *    for the bit-wise OR operation. However, the macro "BOr" has been
 *    retained for consistency reasons.
 */

#if LANGUAGE == C

#ifndef BAnd
#define BAnd    	&
#endif /* !defined (BAnd) */

#ifndef BOr
#define BOr     	|
#endif /* !defined (BOr) */

#ifndef BXor
#define BXor    	^
#endif /* !defined (BXor) */

#ifndef Modulo
#define Modulo  	%
#endif /* !defined (Modulo) */

#elif LANGUAGE == Assembly

#ifndef BAnd
#define BAnd    	:AND:
#endif /* !defined (BAnd) */

#ifndef BOr
#define BOr     	:OR:
#endif /* !defined (BOr) */

#ifndef BXor
#define BXor    	:EOR:
#endif /* !defined (BXor) */

#ifndef Modulo
#define Modulo  	:MOD:
#endif /* !defined (Modulo) */

#endif /* LANGUAGE == C || LANGUAGE == Assembly */


#undef C
#undef Assembly

#endif /* !defined (CMPTBLTY) */
