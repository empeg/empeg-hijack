/*
 * SA1100 MCP Header File
 * Carl Waldspurger
 *
 * $Log: mcp-sa1100.h,v $
 * Revision 2.0  1998/06/25  18:40:43  kramer
 * Major Bump: Update all files to version 2.0 for external release
 * -drew
 *
 * Revision 1.2  1998/02/06  21:08:52  caw
 * Minor cleanup and renaming.
 *
 * Revision 1.1  1998/01/20  03:31:56  caw
 * Initial revision.
 *
 */

#ifndef	mcp_sa1100_h
#define	mcp_sa1100_h

#include <asm/arch/hardware.h>

/*
 * types
 *
 */

typedef struct {
  volatile uint mccr;
  volatile uint reserved0;
  volatile uint mcdr0;
  volatile uint mcdr1;
  volatile uint mcdr2;
  volatile uint reserved1;
  volatile uint mcsr;
} mcp_reg;

/*
 * constants
 *
 */

#define	MCP_IRQ		IRQ_Ser4MCP

#define MCP_BASE_P	((mcp_reg *) _Ser4MCCR0)
#define MCP_BASE_V	((mcp_reg *) &Ser4MCCR0)

#define MCCR_V_ASD	0
#define MCCR_V_TSD	8
#define MCCR_M_MCE	0x010000
#define MCCR_M_ECS	0x020000
#define MCCR_M_ADM	0x040000
#define MCCR_M_TTM	0x080000
#define MCCR_M_TRM	0x100000
#define MCCR_M_ATE	0x200000
#define MCCR_M_ARE	0x400000
#define MCCR_M_LBM	0x800000

#define MCDR2_V_RN	17
#define MCDR2_M_nRW	0x10000

#define MCSR_M_ATS	0x00000001
#define MCSR_M_ARS	0x00000002
#define MCSR_M_TTS	0x00000004
#define MCSR_M_TRS	0x00000008
#define MCSR_M_ATU	0x00000010
#define MCSR_M_ARO	0x00000020
#define MCSR_M_TTU	0x00000040
#define MCSR_M_TRO	0x00000080
#define MCSR_M_ANF	0x00000100
#define MCSR_M_ANE	0x00000200
#define MCSR_M_TNF	0x00000400
#define MCSR_M_TNE	0x00000800
#define MCSR_M_CWC	0x00001000
#define MCSR_M_CRC	0x00002000
#define MCSR_M_ACE	0x00004000
#define MCSR_M_TCE	0x00008000

#endif	/* mcp_sa1100_h */
