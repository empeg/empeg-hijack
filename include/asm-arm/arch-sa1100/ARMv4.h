/*
 *	FILE    	ARMv4.h
 *
 *	Version 	1.1
 *	Author  	Copyright (c) Marc A. Viredaz, 1998
 *	        	DEC Western Research Laboratory, Palo Alto, CA
 *	Date    	January 1998 (April 1997)
 *	System  	Advanced RISC Machine (ARM) version 4
 *	Language	C or ARM Assembly
 *	Purpose 	Definition of constants related to the Advanced RISC
 *	        	Machine (ARM) architecture version 4.
 *
 *	        	Language-specific definitions are selected by the
 *	        	macro "LANGUAGE", which should be defined as either
 *	        	"C" (default) or "Assembly".
 */


#ifndef ARMV4
#define ARMV4

#ifndef LANGUAGE
#define LANGUAGE	C
#endif /* !defined (LANGUAGE) */

#include <asm/arch/bitfield.h>

#define C       	0
#define Assembly	1


/*
 * General definitions
 */

#define ByteWdth	8       	/* Byte Width                      */
#define WordWdth	32      	/* Word Width                      */

#if LANGUAGE == C
typedef unsigned short	Word16 ;
typedef unsigned int	Word32 ;
typedef Word32  	Word ;
typedef Word    	Quad [4] ;
typedef void    	*Address ;
typedef void    	(*ExcpHndlr) (void) ;
#endif /* LANGUAGE == C */


/*
 * Program status registers and condition codes
 *
 * Registers
 *    CPSR      	Current Program Status Register (read/write).
 *    SPSR      	Saved Program Status Register (read/write).
 *    PC        	Program Counter (register 15, read/write).
 */

#define PSR_Mode	Fld (5, 0)	/* Mode                            */
#define PSR_32BitMode	        	/* 32-Bit Mode mask                */ \
                	(0x10 << FShft (PSR_Mode))
                	        	/* 26-bit mode:                    */
#define PSR_26_User	        	/*  User mode                      */ \
                	(0x00 << FShft (PSR_Mode))
#define PSR_26_FIQ	        	/*  Fast Interrupt reQuest mode    */ \
                	(0x01 << FShft (PSR_Mode))
#define PSR_26_IRQ	        	/*  Interrupt ReQuest mode         */ \
                	(0x02 << FShft (PSR_Mode))
#define PSR_26_SVC	        	/*  SuperVisor mode                */ \
                	(0x03 << FShft (PSR_Mode))
                	        	/* 32-bit mode:                    */
#define PSR_32_User	        	/*  User mode                      */ \
                	(0x10 << FShft (PSR_Mode))
#define PSR_32_FIQ	        	/*  Fast Interrupt reQuest mode    */ \
                	(0x11 << FShft (PSR_Mode))
#define PSR_32_IRQ	        	/*  Interrupt ReQuest mode         */ \
                	(0x12 << FShft (PSR_Mode))
#define PSR_32_SVC	        	/*  SuperVisor mode                */ \
                	(0x13 << FShft (PSR_Mode))
#define PSR_32_Abort	        	/*  data/instruction Abort mode    */ \
                	(0x17 << FShft (PSR_Mode))
#define PSR_32_Undef	        	/*  Undefined instruction mode     */ \
                	(0x1B << FShft (PSR_Mode))
#define PSR_32_System	        	/*  System mode                    */ \
                	(0x1F << FShft (PSR_Mode))
                	        	/* normal mode (32-bit mode):      */
#define PSR_User	PSR_32_User	/*  User mode                      */
#define PSR_FIQ 	PSR_32_FIQ	/*  Fast Interrupt reQuest mode    */
#define PSR_IRQ 	PSR_32_IRQ	/*  Interrupt ReQuest mode         */
#define PSR_SVC 	PSR_32_SVC	/*  SuperVisor mode                */
#define PSR_Abort	PSR_32_Abort	/*  data/instruction Abort mode    */
#define PSR_Undef	PSR_32_Undef	/*  Undefined instruction mode     */
#define PSR_System	PSR_32_System	/*  System mode                    */
#define PSR_T   	0x00000020	/* Thumb instruction set           */
#define PSR_F   	0x00000040	/* Fast interrupt disable          */
#define PSR_I   	0x00000080	/* Interrupt disable               */
#define PSR_V   	0x10000000	/* oVerflow flag                   */
#define PSR_C   	0x20000000	/* Carry flag                      */
#define PSR_Z   	0x40000000	/* Zero flag                       */
#define PSR_N   	0x80000000	/* Negative flag                   */
                	        	/* 26-bit mode:                    */
#define PC_26_Mode	Fld (2, 0)	/*  Mode                           */
#define PC_26_User	        	/*  User mode                      */ \
                	(0x00 << FShft (PC_26_Mode))
#define PC_26_FIQ	        	/*  Fast Interrupt reQuest mode    */ \
                	(0x01 << FShft (PC_26_Mode))
#define PC_26_IRQ	        	/*  Interrupt ReQuest mode         */ \
                	(0x02 << FShft (PC_26_Mode))
#define PC_26_SVC	        	/*  SuperVisor mode                */ \
                	(0x03 << FShft (PC_26_Mode))
#define PC_26_F 	0x04000000	/*  Fast interrupt disable         */
#define PC_26_I 	0x08000000	/*  Interrupt disable              */
#define PC_26_V 	0x10000000	/*  oVerflow flag                  */
#define PC_26_C 	0x20000000	/*  Carry flag                     */
#define PC_26_Z 	0x40000000	/*  Zero flag                      */
#define PC_26_N 	0x80000000	/*  Negative flag                  */


/*
 * Exception vectors
 */

                	        	/* exception Vector:               */
#define _Vec_Reset	0x00000000	/*  Reset                          */
#define _Vec_Undef	0x00000004	/*  Undefined instruction          */
#define _Vec_SWI	0x00000008	/*  SoftWare Interrupt             */
#define _Vec_InstrAbort	0x0000000C	/*  Instruction Abort              */
#define _Vec_DataAbort	0x00000010	/*  Data Abort                     */
#define _Vec_Addr	0x00000014	/*  Address error (26-bit mode)    */
#define _Vec_IRQ	0x00000018	/*  Interrupt ReQuest              */
#define _Vec_FIQ	0x0000001C	/*  Fast Interrupt reQuest         */

#if LANGUAGE == C
                	        	/* exception Vector:               */
#define Vec_Reset	        	/*  Reset                          */ \
                	(*((ExcpHndlr) _Vec_Reset))
#define Vec_Undef	        	/*  Undefined instruction          */ \
                	(*((ExcpHndlr) _Vec_Undef))
#define Vec_SWI 	        	/*  SoftWare Interrupt             */ \
                	(*((ExcpHndlr) _Vec_SWI))
#define Vec_InstrAbort	        	/*  Instruction Abort              */ \
                	(*((ExcpHndlr) _Vec_InstrAbort))
#define Vec_DataAbort	        	/*  Data Abort                     */ \
                	(*((ExcpHndlr) _Vec_DataAbort))
#define Vec_Addr	        	/*  Address error (26-bit mode)    */ \
                	(*((ExcpHndlr) _Vec_Addr))
#define Vec_IRQ 	        	/*  Interrupt ReQuest              */ \
                	(*((ExcpHndlr) _Vec_IRQ))
#define Vec_FIQ 	        	/*  Fast Interrupt reQuest         */ \
                	(*((ExcpHndlr) _Vec_FIQ))
#endif /* LANGUAGE == C */


#undef C
#undef Assembly

#endif /* !defined (ARMV4) */
