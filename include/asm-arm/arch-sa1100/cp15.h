/*
 *	FILE    	cp15.h
 *
 *	Version 	1.2
 *	Author  	Copyright (c) Marc A. Viredaz, 1998
 *	        	DEC Western Research Laboratory, Palo Alto, CA
 *	Date    	April 1998 (April 1997)
 *	System  	Advanced RISC Machine (ARM) version 4
 *	Language	C or ARM Assembly
 *	Purpose 	Definition of constants related to the Advanced RISC
 *	        	Machine (ARM) microprocessor's system control
 *	        	coprocessor (coprocessor 15). This file also contains
 *	        	information specific to the StrongARM SA-1100
 *	        	microprocessor (as indicted in comments), based on
 *	        	the StrongARM SA-1100 data sheet versions 2.1 and 2.2.
 *
 *	        	Language-specific definitions are selected by the
 *	        	macro "LANGUAGE", which should be defined as either
 *	        	"C" (default) or "Assembly".
 */


#ifndef CP15
#define CP15

#ifndef LANGUAGE
#define LANGUAGE	C
#endif /* !defined (LANGUAGE) */

#include <asm/arch/cmptblty.h>
#include <asm/arch/bitfield.h>

#define C       	0
#define Assembly	1

/*
 * General definitions
 *
 * Coprocessor
 *    SCC       	System Control Coprocessor (coprocessor 15).
 */

#if LANGUAGE == Assembly
#define SCC     	p15, 0  	/* System Control Coprocessor      */
#endif /* LANGUAGE == Assembly */


/*
 * Identification register
 *
 * Register
 *    SCCID     	System Control Coprocessor (SCC) IDentification
 *              	register (read).
 */

#if LANGUAGE == Assembly
#define SCCID   	c0, c0, 0	/* SCC IDentification reg.         */
#endif /* LANGUAGE == Assembly */

#define SCCID_Rev	Fld (4, 0)	/* Revision number                 */
#define SCCID_Part	Fld (12, 4)	/* Part number (BCD)               */
#define SCCID_SA_110	        	/* DEC StrongARM SA-110            */ \
                	FInsrt (0xA10, SCCID_Part)
#define SCCID_SA_1100	        	/* DEC StrongARM SA-1100           */ \
                	FInsrt (0xA11, SCCID_Part)
#define SCCID_Arch	Fld (8, 16)	/* Architecture version code       */
#define SCCID_ARMv3	        	/* ARM architecture version 3      */ \
                	FInsrt (0x00, SCCID_Arch)
#define SCCID_ARMv4	        	/* ARM architecture version 4      */ \
                	FInsrt (0x01, SCCID_Arch)
#define SCCID_Impl	Fld (8, 24)	/* Implementor initial (ASCII)     */
#define SCCID_ARM	        	/* ARM Ltd.                        */ \
                	FInsrt ('A', SCCID_Impl)
#define SCCID_DEC	        	/* Digital Equipment Corp. (DEC)   */ \
                	FInsrt ('D', SCCID_Impl)


/*
 * Control register
 *
 * Register
 *    SCCC      	System Control Coprocessor (SCC) Control register
 *              	(read/write).
 */

#if LANGUAGE == Assembly
#define SCCC    	c1, c0, 0	/* SCC Control reg.                */
#endif /* LANGUAGE == Assembly */

#define SCCC_M  	0x00000001	/* M bit (MMU)                     */
#define SCCC_MMUDis	(SCCC_M*0)	/*  MMU Disable                    */
#define SCCC_MMUEn	(SCCC_M*1)	/*  MMU Enable                     */
#define SCCC_A  	0x00000002	/* A bit (Alignment fault)         */
#define SCCC_AlnFltDis	(SCCC_A*0)	/*  Alignment Fault Disable        */
#define SCCC_AlnFltEn	(SCCC_A*1)	/*  Alignment Fault Enable         */
#define SCCC_C  	0x00000004	/* C bit (instr./data Cache)       */
                	        	/* [SA-1100: data cache]           */
#define SCCC_IDCchDis	(SCCC_C*0)	/*  Instr./Data Cache Disable      */
#define SCCC_IDCchEn	(SCCC_C*1)	/*  Instr./Data Cache Enable       */
#define SCCC_DCchDis	(SCCC_C*0)	/*  Data Cache Disable             */
#define SCCC_DCchEn	(SCCC_C*1)	/*  Data Cache Enable              */
#define SCCC_W  	0x00000008	/* W bit (Write buffer)            */
#define SCCC_WrBufDis	(SCCC_W*0)	/*  Write Buffer Disable           */
#define SCCC_WrBufEn	(SCCC_W*1)	/*  Write Buffer Enable            */
#define SCCC_P  	0x00000010	/* P bit (PROG32)                  */
                	        	/* [SA-1100: always 1 (32-bit)]    */
#define SCCC_26BitExcp	(SCCC_P*0)	/*  26-Bit Exception routines      */
#define SCCC_32BitExcp	(SCCC_P*1)	/*  32-Bit Exception routines      */
#define SCCC_D  	0x00000020	/* D bit (DATA32)                  */
                	        	/* [SA-1100: always 1 (32-bit)]    */
#define SCCC_26BitDAdd	(SCCC_D*0)	/*  26-Bit Data Address            */
#define SCCC_32BitDAdd	(SCCC_D*1)	/*  32-Bit Data Address            */
#define SCCC_L  	0x00000040	/* L bit                           */
                	        	/* [SA-1100: always 1 (unused)]    */
#define SCCC_B  	0x00000080	/* B bit (Big/little endian)       */
#define SCCC_LtlEnd	(SCCC_B*0)	/*  Little Endian                  */
#define SCCC_BigEnd	(SCCC_B*1)	/*  Big Endian                     */
#define SCCC_S  	0x00000100	/* S bit (System protection)       */
#define SCCC_R  	0x00000200	/* R bit (ROM protection)          */
#define SCCC_F  	0x00000400	/* F bit                           */
                	        	/* [SA-1100: unused]               */
#define SCCC_Z  	0x00000800	/* Z bit                           */
                	        	/* [SA-1100: unused]               */
#define SCCC_I  	0x00001000	/* I bit (Instr. cache)            */
#define SCCC_ICchDis	(SCCC_I*0)	/*  Instr. Cache Disable           */
#define SCCC_ICchEn	(SCCC_I*1)	/*  Instr. Cache Enable            */
#define SCCC_X  	0x00002000	/* X bit (eXception vector)        */
                	        	/* [SA-1100: specific]             */
#define SCCC_ExcpVec0	(SCCC_X*0)	/*  Exception Vector address       */
                	        	/*  base 0 (0x00000000)            */
#define SCCC_ExcpVec1	(SCCC_X*1)	/*  Exception Vector address       */
                	        	/*  base 1 (0xFFFF0000)            */


/*
 * Translation table base register
 *
 * Register
 *    SCCTTB    	System Control Coprocessor (SCC) Translation Table
 *              	Base register (read/write).
 */

#if LANGUAGE == Assembly
#define SCCTTB  	c2, c0, 0	/* SCC Translation Table Base reg. */
#endif /* LANGUAGE == Assembly */

#define SCCTTB_TrTblBs	Fld (18, 14)	/* Translation Table Base          */


/*
 * Domain access control register
 *
 * Register
 *    SCCDAC    	System Control Coprocessor (SCC) Domain Access
 *              	Control register (read/write).
 */

#if LANGUAGE == Assembly
#define SCCDAC  	c3, c0, 0	/* SCC Domain Access Control reg.  */
#endif /* LANGUAGE == Assembly */

#define SCCDAC_DA(Dmn)	        	/* Domain Access [0..15]           */ \
                	Fld (2, 2*(Dmn))
#define SCCDAC_DA0	SCCDAC_DA (0)	/* Domain Access  0                */
#define SCCDAC_DA1	SCCDAC_DA (1)	/* Domain Access  1                */
#define SCCDAC_DA2	SCCDAC_DA (2)	/* Domain Access  2                */
#define SCCDAC_DA3	SCCDAC_DA (3)	/* Domain Access  3                */
#define SCCDAC_DA4	SCCDAC_DA (4)	/* Domain Access  4                */
#define SCCDAC_DA5	SCCDAC_DA (5)	/* Domain Access  5                */
#define SCCDAC_DA6	SCCDAC_DA (6)	/* Domain Access  6                */
#define SCCDAC_DA7	SCCDAC_DA (7)	/* Domain Access  7                */
#define SCCDAC_DA8	SCCDAC_DA (8)	/* Domain Access  8                */
#define SCCDAC_DA9	SCCDAC_DA (9)	/* Domain Access  9                */
#define SCCDAC_DA10	SCCDAC_DA (10)	/* Domain Access 10                */
#define SCCDAC_DA11	SCCDAC_DA (11)	/* Domain Access 11                */
#define SCCDAC_DA12	SCCDAC_DA (12)	/* Domain Access 12                */
#define SCCDAC_DA13	SCCDAC_DA (13)	/* Domain Access 13                */
#define SCCDAC_DA14	SCCDAC_DA (14)	/* Domain Access 14                */
#define SCCDAC_DA15	SCCDAC_DA (15)	/* Domain Access 15                */
#define SCCDAC_NoAcc	0x0     	/*  No Access                      */
#define SCCDAC_Clnt	0x1     	/*  Client access                  */
#define SCCDAC_Mngr	0x3     	/*  Manager access                 */


/*
 * Fault status register
 *
 * Register
 *    SCCFS     	System Control Coprocessor (SCC) Fault Status
 *              	register (read/write).
 */

#if LANGUAGE == Assembly
#define SCCFS   	c5, c0, 0	/* SCC Fault Status reg.           */
#endif /* LANGUAGE == Assembly */

#define SCCFS_Stat	Fld (4, 0)	/* Status                          */
#define SCCFS_VecExcp	        	/*  Vector Exception               */ \
                	FInsrt (0x0, SCCFS_Stat)
#define SCCFS_Aln1	        	/*  Alignment fault (1st enc.)     */ \
                	FInsrt (0x1, SCCFS_Stat)
#define SCCFS_TermExcp	        	/*  Terminal Exception             */ \
                	FInsrt (0x2, SCCFS_Stat)
#define SCCFS_Aln2	        	/*  Alignment fault (2nd enc.)     */ \
                	FInsrt (0x3, SCCFS_Stat)
#define SCCFS_ExLFSct	        	/*  External abort on              */ \
                	        	/*  Line-Fetch (Section)           */ \
                	FInsrt (0x4, SCCFS_Stat)
#define SCCFS_TrSct	        	/*  Translation fault (Section)    */ \
                	FInsrt (0x5, SCCFS_Stat)
#define SCCFS_ExLFPg	        	/*  External abort on              */ \
                	        	/*  Line-Fetch (Page)              */ \
                	FInsrt (0x6, SCCFS_Stat)
#define SCCFS_TrPg	        	/*  Translation fault (Page)       */ \
                	FInsrt (0x7, SCCFS_Stat)
#define SCCFS_ExNLFSct	        	/*  External abort on Non          */ \
                	        	/*  Line-Fetch (Section)           */ \
                	FInsrt (0x8, SCCFS_Stat)
#define SCCFS_DmnSct	        	/*  Domain fault (Section)         */ \
                	FInsrt (0x9, SCCFS_Stat)
#define SCCFS_ExNLFPg	        	/*  External abort on Non          */ \
                	        	/*  Line-Fetch (Page)              */ \
                	FInsrt (0xA, SCCFS_Stat)
#define SCCFS_DmnPg	        	/*  Domain fault (Page)            */ \
                	FInsrt (0xB, SCCFS_Stat)
#define SCCFS_ExTr1st	        	/*  External abort on              */ \
                	        	/*  Translation (1st level)        */ \
                	FInsrt (0xC, SCCFS_Stat)
#define SCCFS_PermSct	        	/*  Permission fault (Section)     */ \
                	FInsrt (0xD, SCCFS_Stat)
#define SCCFS_ExTr2nd	        	/*  External abort on              */ \
                	        	/*  Translation (2nd level)        */ \
                	FInsrt (0xE, SCCFS_Stat)
#define SCCFS_PermPg	        	/*  Permission fault (Page)        */ \
                	FInsrt (0xF, SCCFS_Stat)
#define SCCFS_Dmn	Fld (4, 4)	/* Domain                          */
#define SCCFS_DBrPt	0x00000200	/* Data BreakPoint                 */
                	        	/* [SA-1100: specific]             */


/*
 * Fault address register
 *
 * Register
 *    SCCFA     	System Control Coprocessor (SCC) Fault Address
 *              	register (read/write).
 */

#if LANGUAGE == Assembly
#define SCCFA   	c6, c0, 0	/* SCC Fault Address reg.          */
#endif /* LANGUAGE == Assembly */


/*
 * Cache operation register
 *
 * Register
 *    SCCCO     	System Control Coprocessor (SCC) Cache Operation
 *              	register (write).
 */

#if LANGUAGE == Assembly

#define SCCCO   	c7      	/* SCC Cache Operation reg.        */

#define SCCCO_FlICch	c5, 0   	/* Flush Instr. Cache              */
                	        	/* [SA-1100: implemented]          */
#define SCCCO_FlDCch	c6, 0   	/* Flush Data Cache                */
                	        	/* [SA-1100: implemented]          */
#define SCCCO_FlIDCch	c7, 0   	/* Flush Instr./Data Cache(s)      */
                	        	/* [SA-1100: implemented]          */
#define SCCCO_ClDCch	c10, 0  	/* Clean Data Cache                */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_ClIDCch	c11, 0  	/* Clean Instr./Data Cache(s)      */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_ClFlDCch	c14, 0  	/* Clean & Flush Data Cache        */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_ClFlIDCch	c15, 0  	/* Clean & Flush Instr./Data       */
                	        	/* Cache(s)                        */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_FlIEnt	c5, 1   	/* Flush Instr. cache Entry        */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_FlDEnt	c6, 1   	/* Flush Data cache Entry          */
                	        	/* [SA-1100: implemented]          */
#define SCCCO_FlIDEnt	c7, 1   	/* Flush Instr./Data cache         */
                	        	/* Entry                           */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_ClDEnt	c10, 1  	/* Clean Data cache Entry          */
                	        	/* [SA-1100: implemented]          */
#define SCCCO_ClIDEnt	c11, 1  	/* Clean Instr./Data cache         */
                	        	/* Entry                           */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_ClFlDEnt	c14, 1  	/* Clean & Flush Data cache        */
                	        	/* Entry                           */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_ClFlIDEnt	c15, 1  	/* Clean & Flush Instr./Data       */
                	        	/* cache Entry                     */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_FlPrFBuf	c5, 4   	/* Flush Pre-Fetch Buffer          */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_DrWrBuf	c10, 4  	/* Drain Write Buffer              */
                	        	/* [SA-1100: implemented]          */
#define SCCCO_FlBTCch	c5, 6   	/* Flush Branch Target Cache       */
                	        	/* [SA-1100: not implemented]      */
#define SCCCO_FlBTEnt	c5, 7   	/* Flush Branch Target cache       */
                	        	/* Entry                           */
                	        	/* [SA-1100: not implemented]      */

#endif /* LANGUAGE == Assembly */


/*
 * Translation Look-aside Buffer (TLB) operation register
 *
 * Register
 *    SCCTO     	System Control Coprocessor (SCC) Translation
 *              	Look-aside Buffer (TLB) Operation register (write).
 */

#if LANGUAGE == Assembly

#define SCCTO   	c8      	/* SCC TLB Operation reg.          */

#define SCCTO_FlITBL	c5, 0   	/* Flush Instr. TBL                */
                	        	/* [SA-1100: implemented]          */
#define SCCTO_FlDTBL	c6, 0   	/* Flush Data TBL                  */
                	        	/* [SA-1100: implemented]          */
#define SCCTO_FlIDTBL	c7, 0   	/* Flush Instr./Data TBL(s)        */
                	        	/* [SA-1100: implemented]          */
#define SCCTO_FlIEnt	c5, 1   	/* Flush Instr. TBL Entry          */
                	        	/* [SA-1100: not implemented]      */
#define SCCTO_FlDEnt	c6, 1   	/* Flush Data TBL Entry            */
                	        	/* [SA-1100: implemented]          */
#define SCCTO_FlIDEnt	c7, 1   	/* Flush Instr./Data TBL Entry     */
                	        	/* [SA-1100: not implemented]      */

#endif /* LANGUAGE == Assembly */


/*
 * Read buffer operation register
 *
 * Register
 *    SCCRBO    	System Control Coprocessor (SCC) Read Buffer
 *              	Operation register (write).
 */

#if LANGUAGE == Assembly

#define SCCRBO  	c9      	/* SCC Read Buffer Operation reg.  */
                	        	/* [SA-1100: specific]             */

#define SCCTO_FlAllBuf	c0, 0   	/* Flush All Buffers               */
#define SCCTO_FlBuf0	c0, 1   	/* Flush Buffer 0                  */
#define SCCTO_FlBuf1	c1, 1   	/* Flush Buffer 1                  */
#define SCCTO_FlBuf2	c2, 1   	/* Flush Buffer 2                  */
#define SCCTO_FlBuf3	c3, 1   	/* Flush Buffer 3                  */
#define SCCTO_Ld1Buf0	c0, 2   	/* Load 1 word in Buffer 0         */
#define SCCTO_Ld1Buf1	c1, 2   	/* Load 1 word in Buffer 1         */
#define SCCTO_Ld1Buf2	c2, 2   	/* Load 1 word in Buffer 2         */
#define SCCTO_Ld1Buf3	c3, 2   	/* Load 1 word in Buffer 3         */

#define SCCTO_Ld4Buf0	c4, 2   	/* Load 4 word in Buffer 0         */
#define SCCTO_Ld4Buf1	c5, 2   	/* Load 4 word in Buffer 1         */
#define SCCTO_Ld4Buf2	c6, 2   	/* Load 4 word in Buffer 2         */
#define SCCTO_Ld4Buf3	c7, 2   	/* Load 4 word in Buffer 3         */
#define SCCTO_Ld8Buf0	c8, 2   	/* Load 8 word in Buffer 0         */
#define SCCTO_Ld8Buf1	c9, 2   	/* Load 8 word in Buffer 1         */
#define SCCTO_Ld8Buf2	c10, 2  	/* Load 8 word in Buffer 2         */
#define SCCTO_Ld8Buf3	c11, 2  	/* Load 8 word in Buffer 3         */
#define SCCTO_UsrRBODis	c0, 4   	/* User-level Read Buffer          */
                	        	/* Operation Disable               */
#define SCCTO_UsrRBOEn	c0, 5   	/* User-level Read Buffer          */
                	        	/* Operation Enable                */

#endif /* LANGUAGE == Assembly */


/*
 * Process identification register
 *
 * Register
 *    SCCPID    	System Control Coprocessor (SCC) Process
 *              	IDentification register (read/write).
 */

#if LANGUAGE == Assembly
#define SCCPID  	c13, c0, 0	/* SCC Process IDentification reg. */
                	        	/* [SA-1100: specific]             */
#endif /* LANGUAGE == Assembly */

#define SCCPID_PID	Fld (6, 25)	/* Process IDentification          */


/*
 * Breakpoint register
 *
 * Register
 *    SCCB      	System Control Coprocessor (SCC) Breakpoint register
 *              	(read/write).
 */

#if LANGUAGE == Assembly
#define SCCB    	c14     	/* SCC Breakpoint reg.             */
                	        	/* [SA-1100: specific]             */
#endif /* LANGUAGE == Assembly */

#if LANGUAGE == Assembly
#define SCCB_DBAR	c0, 0   	/* Data Breakpoint Address Reg.    */
#define SCCB_DBVR	c1, 0   	/* Data Breakpoint Value Reg.      */
#define SCCB_DBMR	c2, 0   	/* Data Breakpoint Mask Reg.       */
#define SCCB_DBCR	c3, 0   	/* Data Breakpoint Control Reg.    */
#endif /* LANGUAGE == Assembly */
#define SCCB_LW 	0x00000001	/*  Load Watch                     */
#define SCCB_LdWtchDis	(SCCB_LW*0)	/*  Load Watch Disable             */
#define SCCB_LdWtchEn	(SCCB_LW*1)	/*  Load Watch Enable              */
#define SCCB_SAW	0x00000002	/*  Store Address Watch            */
#define SCCB_StAWtchDis	(SCCB_SAW*0)	/*  Store Address Watch Disable    */
#define SCCB_StAWtchEn	(SCCB_SAW*1)	/*  Store Address Watch Enable     */
#define SCCB_SDW	0x00000004	/*  Store Data Watch               */
#define SCCB_StDWtchDis	(SCCB_SDW*0)	/*  Store Data Watch Disable       */
#define SCCB_StDWtchEn	(SCCB_SDW*1)	/*  Store Data Watch Enable        */
#if LANGUAGE == Assembly
#define SCCB_IBCR	c8, 0   	/* Instr. Breakpoint address       */
                	        	/* and Control Reg. (write)        */
#endif /* LANGUAGE == Assembly */
#define SCCB_E  	0x00000001	/*  instr. breakpoint Enable       */
#define SCCB_IBrPtDis	(SCCB_E*0)	/*  Instr. BreakPoint Disable      */
#define SCCB_IBrPtEn	(SCCB_E*1)	/*  Instr. BreakPoint Enable       */
#define SCCB_IBrPtAdd	Fld (30, 2)	/*  Instr. BreakPoint Address      */


/*
 * Test, clock, and idle control register
 *
 * Register
 *    SCCTCI    	System Control Coprocessor (SCC) Test, Clock, and
 *              	Idle control register (write).
 */

#if LANGUAGE == Assembly

#define SCCTCI  	c15     	/* SCC Test, Clock, and Idle       */
                	        	/* control reg.                    */
                	        	/* [SA-1100: specific]             */

#define SCCTCI_OdLFSREn	c1, 1   	/* Odd word loading of instr.      */
                	        	/* cache Linear Feedback Shift     */
                	        	/* Reg. (LFSR) Enable              */
#define SCCTCI_EvLFSREn	c2, 1   	/* Even word loading of instr.     */
                	        	/* cache Linear Feedback Shift     */
                	        	/* Reg. (LFSR) Enable              */
#define SCCTCI_ClLFSR	c4, 1   	/* Clear instr. cache Linear       */
                	        	/* Feedback Shift Reg. (LFSR)      */
#define SCCTCI_MovLFSR	c8, 1   	/* Move instr. cache Linear        */
                	        	/* Feedback Shift Reg. (LFSR)      */
                	        	/* to R14_Abort                    */
#define SCCTCI_ClkSwEn	c1, 2   	/* Clock Switching Enable          */
#define SCCTCI_ClkSwDis	c2, 2   	/* Clock Switching Disable         */
#define SCCTCI_WaitInt	c8, 2   	/* Wait for Interrupt              */

#endif /* LANGUAGE == Assembly */


/*
 * Memory Management Unit (MMU)
 */

                	        	/* virtual Address:                */
#define Add_TrTblIdx	Fld (12, 20)	/*  Translation Table Index (word) */
#define Add_PgTblIdx	Fld (8, 12)	/*  Page Table Index (word)        */
#define Add_SctIdx	Fld (20, 0)	/*  Section Index (byte)           */
#define Add_LgPgIdx	Fld (16, 0)	/*  Large Page Index (byte)        */
#define Add_SmPgIdx	Fld (12, 0)	/*  Small Page Index (byte)        */
                	        	/* Translation Table descriptor:   */
#define TrTbl_Type	Fld (2, 0)	/*  descriptor Type                */
#define TrTbl_Inval	        	/*   Invalid (translation fault)   */ \
                	FInsrt (0, TrTbl_Type)
#define TrTbl_PgTbl	        	/*   Page Table                    */ \
                	FInsrt (1, TrTbl_Type)
#define TrTbl_Sct	        	/*   Section                       */ \
                	FInsrt (2, TrTbl_Type)
#define TrTbl_B 	0x00000004	/*  Bufferable (section)           */
#define TrTbl_C 	0x00000008	/*  Cacheable (section)            */
#define TrTbl_Dmn	Fld (4, 5)	/*  Domain (page table, section)   */
#define TrTbl_AP	Fld (2, 10)	/*  Access Permissions (section)   */
#define TrTbl_PgTblBs	Fld (22, 10)	/*  Page Table Base address        */
#define TrTbl_SctBs	Fld (12, 20)	/*  Section Base address           */
                	        	/* Page Table descriptor:          */
#define PgTbl_Type	Fld (2, 0)	/*  descriptor Type                */
#define PgTbl_Inval	        	/*   Invalid (translation fault)   */ \
                	FInsrt (0, PgTbl_Type)
#define PgTbl_LgPg	        	/*   Large Page                    */ \
                	FInsrt (1, PgTbl_Type)
#define PgTbl_SmPg	        	/*   Small Page                    */ \
                	FInsrt (2, PgTbl_Type)
#define PgTbl_B 	0x00000004	/*  Bufferable (large/small page)  */
#define PgTbl_C 	0x00000008	/*  Cacheable (large/small page)   */
#define PgTbl_AP(SubPg)	        	/*  Access Permissions sub-page    */ \
                	        	/*  [0..3]                         */ \
                	Fld (2, 4 + 2*(SubPg))
#define PgTbl_AP0	PgTbl_AP (0)	/*  Access Permissions sub-page 0  */
#define PgTbl_AP1	PgTbl_AP (1)	/*  Access Permissions sub-page 1  */
#define PgTbl_AP2	PgTbl_AP (2)	/*  Access Permissions sub-page 2  */
#define PgTbl_AP3	PgTbl_AP (3)	/*  Access Permissions sub-page 3  */
#define PgTbl_NoAcc	0       	/*   No Access (read-only access   */
                	        	/*   granted by SCCC_S or SCCC_R)  */
#define PgTbl_NoUsrAcc	1       	/*   No User Access (supervisor    */
                	        	/*   read/write access)            */
#define PgTbl_UsrRAcc	2       	/*   User Read-only (supervisor    */
                	        	/*   read/write) Access            */
#define PgTbl_UsrRWAcc	3       	/*   User/supervisor Read/Write    */
                	        	/*   Access                        */
#define PgTbl_LgPgBs	Fld (16, 16)	/*  Large Page Base address        */
#define PgTbl_SmPgBs	Fld (20, 12)	/*  Small Page Base address        */


#undef C
#undef Assembly

#endif /* !defined (CP15) */
