/*
 * MCP Module
 * Copyright (c) Carl Waldspurger, 1998
 *
 * $Log: mcp_common.c,v $
 * Revision 2.0  1998/06/24  21:43:09  kramer
 * Major Bump: Move all version numbers up to 2.0 for external release.
 * -drew
 *
 * Revision 1.10  1998/06/16  19:19:41  kramer
 * Added copyright information
 *
 * Revision 1.9  1998/04/14  02:11:31  caw
 * Minor update to reflect new powermgr interface.
 *
 * Revision 1.8  1998/03/31  00:02:44  caw
 * Explicitly disable MCP during suspend.  Removed debugging output.
 *
 * Revision 1.7  1998/03/28  00:17:48  caw
 * Added support for power management.
 *
 * Revision 1.6  1998/03/17  23:34:19  caw
 * Added statistics, report stats and mccr/mcsr on /proc/mcp device.
 *
 * Revision 1.5  1998/03/07  00:10:41  kerr
 * Took out ifdef MODULE which were causing problems with symbol generation.
 *
 * Revision 1.4  1998/03/06  03:23:41  caw
 * Commented out include of version.h to prevent compilation troubles.
 *
 * Revision 1.3  1998/03/06  00:27:57  caw
 * Module debugged and working.  Reduced logging, fixed /proc/mcp formatting.
 *
 * Revision 1.2  1998/03/05  22:51:01  caw
 * Completely revised interface after discussion with Larry.
 *
 * Revision 1.1  1998/03/05  03:55:24  caw
 * Initial revision.
 *
 */

/*
 *
 * debugging 
 *
 */

#define	MCP_DEBUG			(1)
#define	MCP_DEBUG_VERBOSE		(0)

/*
 * includes
 *
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/types.h>
#include <linux/errno.h>

#ifdef	CONFIG_PROC_FS
#include <linux/stat.h>
#include <linux/proc_fs.h>
#endif

#ifdef	CONFIG_POWERMGR
#include <linux/powermgr.h>
#endif

#include <linux/itsy_audio.h>

#include "mcp_common.h"
#include "mcp-sa1100.h"

/*
 * constants
 *
 */

/* names */
#define	MCP_MODULE_NAME			"mcp-common"
#define	MCP_MODULE_NAME_VERBOSE		"MCP SA1100/UCB1200 module"
#define	MCP_VERSION_STRING		"$Revision: 2.0 $"

/* codec */
#define	CODEC_ASD_NUMERATOR		(375000)

/*
 * types
 *
 */

typedef struct {
  /* memory-mapped mcp */
  mcp_reg *mcp;

  /* bitmasks */
  ulong id_mask;
  ulong enabled_mask;

  /* statistics */
  ulong enable_count;
  ulong disable_count;
  ulong prim_enable_count;
  ulong prim_disable_count;

#ifdef	CONFIG_POWERMGR
  /* power management */
  powermgr_id pm_id;
#endif
} mcp_state;

/*
 * globals
 *
 */

static mcp_state mcp_global;

#ifdef	CONFIG_PROC_FS
static int mcp_get_info(char *, char **, off_t, int, int);
static struct proc_dir_entry mcp_proc_entry = {
  0, 3, "mcp", S_IFREG | S_IRUGO, 1, 0, 0, 0, NULL, mcp_get_info,
};
#endif	/* CONFIG_PROC_FS */

/*
 * procfs operations
 *
 */

#ifdef	CONFIG_PROC_FS
static int mcp_get_info(char *buf, 
			char **start,
			off_t offset,
			int length,
			int unused)
{
  mcp_state *m = &mcp_global;
  int len, i;

  /* format header */
  len = sprintf(buf,
		"mcp-vaddr:   %08lx\n"
		"mcp-mccr:    %08lx\n"
		"mcp-mcsr:    %08lx\n"
		"enable-rqs:  %8lu\n"
		"enables:     %8lu\n"
		"disable-rqs: %8lu\n"
		"disables:    %8lu\n"
		"mcp-ids:     ",
		(ulong) m->mcp,
		m->mcp->mccr,
		m->mcp->mcsr,
		m->enable_count,
		m->prim_enable_count,
		m->disable_count,
		m->prim_disable_count);

  /* format registered ids */
  if (m->id_mask == 0)
    {
      len += sprintf(buf + len, "none");
    }
  else
    {
      for (i = 0; i <= MCP_COMMON_ID_MAX; i++)
	if (m->id_mask & (1 << i))
	  len += sprintf(buf + len, "%d ", i);
    }

  /* format enabled ids */
  len += sprintf(buf + len, "\nmcp-enabled: ");
  if (m->enabled_mask == 0)
    {
      len += sprintf(buf + len, "none");
    }
  else
    {
      for (i = 0; i <= MCP_COMMON_ID_MAX; i++)
	if (m->enabled_mask & (1 << i))
	  len += sprintf(buf + len, "%d ", i);
    }
  len += sprintf(buf + len, "\n");

  return(len);
}
#endif	/* CONFIG_PROC_FS */

/*
 * mcp_state operations
 *
 */

static void mcp_state_init(mcp_state *m)
{
  /* initialize mcp state */
  memset((void *) m, 0, sizeof(mcp_state));
  m->mcp = MCP_BASE_V;
  m->id_mask = 0;
  m->enabled_mask = 0;

  /* initialize stats */
  m->enable_count = 0;
  m->disable_count = 0;
  m->prim_enable_count = 0;
  m->prim_disable_count = 0;

#ifdef	CONFIG_POWERMGR
  /* initialize powermgr state */
  m->pm_id = POWERMGR_ID_INVALID;
#endif	/* CONFIG_POWERMGR */
}

static inline void mcp_enable_prim(mcp_state *m)
{
  /* turn on the MCP */
  int asd = CODEC_ASD_NUMERATOR / AUDIO_RATE_DEFAULT;
  m->mcp->mccr = 0;
  m->mcp->mccr = MCCR_M_ADM | MCCR_M_MCE | (40 << MCCR_V_TSD) | asd;

  /* update stats */
  m->prim_enable_count++;

  if (MCP_DEBUG_VERBOSE)
    printk(MCP_MODULE_NAME ": mcp_enable_prim\n");
}

static inline void mcp_disable_prim(mcp_state *m)
{
  /* turn off the MCP */
  m->mcp->mccr = 0;

  /* update stats */
  m->prim_disable_count++;

  if (MCP_DEBUG_VERBOSE)
    printk(MCP_MODULE_NAME ": mcp_disable_prim\n");
}

/*
 * exported operations
 *
 */

mcp_common_id mcp_common_register(void)
{
  /*
   * modifies: mcp_global
   * effects:  Register as user of SA1100 MCP.  Returns a unique mcp id
   *           for use with future calls, or MCP_COMMON_ID_INVALID if
   *           unable to register.
   *
   */

  mcp_state *m = &mcp_global;
  mcp_common_id id;
  ulong flags, i;

  /* search bitmask for available id */
  id = MCP_COMMON_ID_INVALID;
  save_flags_cli(flags);
  for (i = 0; i <= MCP_COMMON_ID_MAX; i++)
    if ((m->id_mask & (1 << i)) == 0)
      {
	/* allocate id, clear enabled */
	m->id_mask |= (1 << i);
	m->enabled_mask &= ~(1 << i);
	id = (mcp_common_id) i;
	break;
      }
  restore_flags(flags);

#ifdef	MODULE
  if (id != MCP_COMMON_ID_INVALID)
    {
      MOD_INC_USE_COUNT;
    }
#endif

  return(id);
}

int mcp_common_unregister(mcp_common_id id)
{
  /*
   * modifies: mcp_global
   * effects:  Deregisters id as user of SA1100 MCP. 
   *           Returns 0 iff successful, otherwise returns error code.
   *
   */

  mcp_state *m = &mcp_global;
  ulong flags;
  int retval;

  /* sanity check */
  if ((id < 0) || (id > MCP_COMMON_ID_MAX))
    return(-EINVAL);

  /* search bitmask for id */
  save_flags_cli(flags);
  if (m->id_mask & (1 << id))
    {
      /* deallocate id, forcibly disable */
      retval = 0;
      m->id_mask &= ~(1 << id);
      if (m->enabled_mask & (1 << id))
	(void) mcp_common_disable(id);
    }
  else
    {
      /* id not allocated */
      retval = -EINVAL;
    }
  restore_flags(flags);

#ifdef	MODULE
  if (retval == 0)
    {
      MOD_DEC_USE_COUNT;
    }
#endif

  return(retval);
}

int mcp_common_enable(mcp_common_id id)
{
  /*
   * modifies: mcp_global
   * effects:  Turns on SA1100 MCP; no effect if MCP is already on.
   *           Redundant calls with the same id are safely ignored.
   *           Returns 0 iff successful, otherwise returns error code.
   *
   */

  mcp_state *m = &mcp_global;
  ulong flags;

  /* sanity checks */
  if ((id < 0) || (id > MCP_COMMON_ID_MAX))
    return(-EINVAL);
  if ((m->id_mask & (1 << id)) == 0)
    return(-EINVAL);

  save_flags_cli(flags);
  if ((m->enabled_mask & (1 << id)) == 0)
    {
      /* turn on if necessary */
      if (m->enabled_mask == 0)
	mcp_enable_prim(m);	
      m->enabled_mask |= (1 << id);
    }
  restore_flags(flags);

  /* update stats */
  m->enable_count++;

  /* successful */
  return(0);
}

int mcp_common_disable(mcp_common_id id)
{
  /*
   * modifies: mcp_global
   * effects:  Turns off SA1100 MCP; no effect if MCP is already off.
   *           Redundant calls with the same id are safely ignored.
   *           Returns 0 iff successful, otherwise returns error code.
   *
   */

  mcp_state *m = &mcp_global;
  ulong flags;

  /* sanity checks */
  if ((id < 0) || (id > MCP_COMMON_ID_MAX))
    return(-EINVAL);
  if ((m->id_mask & (1 << id)) == 0)
    return(-EINVAL);

  save_flags_cli(flags);
  if (m->enabled_mask & (1 << id))
    {
      /* turn off if necessary */
      m->enabled_mask &= ~(1 << id);      
      if (m->enabled_mask == 0)
	mcp_disable_prim(m);
    }
  restore_flags(flags);

  /* update stats */
  m->disable_count++;

  /* successful */
  return(0);
}

/*
 * power management operations
 *
 */

#ifdef	CONFIG_POWERMGR
static int mcp_common_suspend_check(void *cookie, int idle_jiffies)
{
  /*
   * modifies: nothing
   * effects:  Always returns zero; i.e. willing to sleep immediately.
   *
   */

  return(0);
}

static int mcp_common_suspend(void *cookie)
{
  /*
   * modifies: nothing
   * effects:  Saves state in preparation for sleep mode.
   *           Always returns zero (ignored).
   *
   */

  mcp_state *m = &mcp_global;

  if (MCP_DEBUG_VERBOSE)
    printk(MCP_MODULE_NAME ": mcp_common_suspend\n");

  /* turn off mcp, if necessary */
  if (m->enabled_mask != 0)
    mcp_disable_prim(m);

  /* return value ignored */
  return(0);
}

static int mcp_common_resume(void *cookie, int resume_flags)
{
  /*
   * modifies: mcp_global
   * effects:  Resumes mcp state after wakeup from sleep mode.
   *           Always returns zero (ignored).
   *
   */

  mcp_state *m = &mcp_global;

  /* restore mcp state */
  if (m->enabled_mask == 0)
    mcp_disable_prim(m);
  else
    mcp_enable_prim(m);

  if (MCP_DEBUG_VERBOSE)
    printk(MCP_MODULE_NAME ": mcp_common_resume (%lx)\n", m->enabled_mask);

  /* return value ignored */
  return(0);
}

static const powermgr_client mcp_powermgr = {
  /* callback functions */
  mcp_common_suspend_check,
  mcp_common_suspend,
  mcp_common_resume,
  
  /* uninterpreted client data */
  NULL,

  /* identity */
  POWERMGR_ID_INVALID,
  MCP_MODULE_NAME_VERBOSE,

  /* power-consumption info */
  0,
  0
};

#endif	/* CONFIG_POWERMGR */

int sa1100_mcp_common_init(void)
{
  if (MCP_DEBUG_VERBOSE)
    printk(MCP_MODULE_NAME ": initializing\n");

  /* initialize global state */
  mcp_state_init(&mcp_global);

#ifdef	CONFIG_PROC_FS
  /* register procfs devices */
  proc_register(&proc_root, &mcp_proc_entry);
#endif	/* CONFIG_PROC_FS */

#ifdef	CONFIG_POWERMGR
  mcp_global.pm_id = powermgr_register(&mcp_powermgr);
  if (mcp_global.pm_id == POWERMGR_ID_INVALID)
    if (MCP_DEBUG)
      printk(MCP_MODULE_NAME ": init_module: powermgr_register failed\n");
#endif	/* CONFIG_POWERMGR */

  /* log registration */
  printk(MCP_MODULE_NAME_VERBOSE " initalized\n");

  /* everything OK */
  return(0);
}

/*
 * module operations
 *
 */

EXPORT_SYMBOL(mcp_common_register);
EXPORT_SYMBOL(mcp_common_unregister);
EXPORT_SYMBOL(mcp_common_enable);
EXPORT_SYMBOL(mcp_common_disable);

#ifdef	MODULE
int init_module(void)
{
  return sa1100_mcp_common_init();
}

void cleanup_module(void)
{
  mcp_state *m = &mcp_global;
  int err;

  if (MCP_DEBUG_VERBOSE)
    printk(MCP_MODULE_NAME ": cleanup_module\n");

#ifdef	CONFIG_PROC_FS
  /* unregister procfs entries */
  if ((err = proc_unregister(&proc_root, mcp_proc_entry.low_ino)) != 0)
    if (MCP_DEBUG)
      printk(MCP_MODULE_NAME ": cleanup_module: proc_unregister %d\n", err);
#endif	/* CONFIG_PROC_FS */

#ifdef	CONFIG_POWERMGR
  if ((err = powermgr_unregister(m->pm_id)) != POWERMGR_SUCCESS)
    if (MCP_DEBUG)
      printk(MCP_MODULE_NAME ": cleanup_module: powermgr_unregister %d\n",err);
#endif	/* CONFIG_POWERMGR */

  /* sanity check */
  if ((m->id_mask != 0) || (m->enabled_mask != 0))
    printk(MCP_MODULE_NAME ": cleanup_module: id=%lx, enabled=%lx\n",
	   m->id_mask,
	   m->enabled_mask);
  
  /* forcibly disable mcp */
  mcp_disable_prim(m);

  /* log unload */
  printk(MCP_MODULE_NAME_VERBOSE " unloaded\n");
}
#endif	/* MODULE */
