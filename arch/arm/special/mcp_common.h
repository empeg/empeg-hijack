/*
 * SA1100 MCP Common Module Header File
 * Copyright (c) Carl Waldspurger, 1998
 *
 * $Log: mcp_common.h,v $
 * Revision 2.0  1998/06/25  18:40:44  kramer
 * Major Bump: Update all files to version 2.0 for external release
 * -drew
 *
 * Revision 1.2  1998/06/16  19:20:08  kramer
 * Added copyright information
 *
 * Revision 1.1  1998/03/05  22:45:46  caw
 * Initial revision.
 *
 */

#ifndef	mcp_common_h
#define	mcp_common_h

/*
 * constants
 *
 */

#define	MCP_COMMON_ID_INVALID	(-1)
#define	MCP_COMMON_ID_MAX	(31)

/*
 * types
 *
 */

typedef int mcp_common_id;

/*
 * operations
 *
 */

extern mcp_common_id mcp_common_register(void);
  /*
   * effects:  Register as user of SA1100 MCP.  Returns a unique mcp id
   *           for use with future calls, or MCP_COMMON_ID_INVALID if
   *           unable to register.
   *
   */

extern int mcp_common_unregister(mcp_common_id id);
  /*
   * effects:  Deregisters id as user of SA1100 MCP. 
   *           Returns 0 iff successful, otherwise returns error code.
   *
   */

extern int mcp_common_enable(mcp_common_id id);
  /*
   * effects:  Turns on SA1100 MCP; no effect if MCP is already on.
   *           Redundant calls with the same id are safely ignored.
   *           Returns 0 iff successful, otherwise returns error code.
   *
   */

extern int mcp_common_disable(mcp_common_id id);
  /*
   * effects:  Turns off SA1100 MCP; no effect if MCP is already off.
   *           Redundant calls with the same id are safely ignored.
   *           Returns 0 iff successful, otherwise returns error code.
   *
   */

#endif	/* mcp_common_h */
