/*
 * empeg_proc.c
 *
 * Simple implementation of a few entries in proc that don't
 * warrant a complete driver for themselves.
 *
 * 1999/07/11 MAC First version
 * 1999/09/01 MAC Add support for power source checking
 * 2000/03/04 HBF init code marked as __init
 * 2000/05/24 HBF Added first boot & accessory sense stuff to the empeg_power
 */

#include <linux/config.h>
#include <linux/proc_fs.h>
#include <linux/init.h>

/* Read thermometer */
extern int empeg_inittherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);
extern int empeg_readtherm(volatile unsigned int *timerbase, volatile unsigned int *gpiobase);

static int id_read_procmem(char *buf, char **start, off_t offset,
			   int len, int unused)
{
	unsigned int *permset=(unsigned int*)(EMPEG_FLASHBASE+0x2000);
	unsigned int *modset=(unsigned int*)(EMPEG_FLASHBASE+0x2000);
	unsigned long *user_splash=(unsigned long*)(EMPEG_FLASHBASE+0xa000);
	len = 0;
	len += sprintf(buf+len, "hwrev : %02d\n", permset[0]);
	len += sprintf(buf+len, "serial: %05d\n", permset[1]);
	len += sprintf(buf+len, "build : %08x\n", permset[3]);
	len += sprintf(buf+len, "id    : %08x-%08x-%08x-%08x\n",
		       permset[4],permset[5],permset[6],permset[7]);
	len += sprintf(buf+len, "ram   : %dK\n",
		       permset[8]==0xffffffff?8192:permset[8]);
	len += sprintf(buf+len, "flash : %dK\n",
		       permset[9]==0xffffffff?1024:permset[9]);
	len += sprintf(buf+len, "drives: %d\n", modset[0]);
	len += sprintf(buf+len, "image : %08lx\n", *user_splash);
	return len;
}

static int therm_read_procmem(char *buf, char **start, off_t offset,
			      int len, int unused)
{
	int temp;
	unsigned long flags;
	extern int hijack_temperature_correction;	// arch/arm/special/hijack.c

	/* Need to disable IRQs & FIQs during temperature read */
	save_flags_clif(flags);
	temp=empeg_readtherm(&OSMR0,&GPLR);
	restore_flags(flags);

	/* Correct for negative temperatures (sign extend) */
	if (temp&0x80) temp=-(128-(temp^0x80));

	len = 0;
	len += sprintf(buf+len, "%d\n",temp + hijack_temperature_correction);
	return len;
}

static struct proc_dir_entry id_proc_entry = {
	0,			/* inode (dynamic) */
	8, "empeg_id",  	/* length and name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	0, 			/* size */
	NULL, 			/* use default operations */
	&id_read_procmem, 	/* function used to read data */
};

static struct proc_dir_entry therm_proc_entry = {
	0,			/* inode (dynamic) */
	11, "empeg_therm",  	/* length and name */
	S_IFREG | S_IRUGO, 	/* mode */
	1, 0, 0, 		/* links, owner, group */
	0, 			/* size */
	NULL, 			/* use default operations */
	&therm_read_procmem, 	/* function used to read data */
};

void __init empeg_proc_init(void)
{
	unsigned long flags;

	/* Initialise thermometer */
	save_flags_cli(flags);
	empeg_inittherm(&OSMR0,&GPLR);
	restore_flags(flags);

#ifdef CONFIG_PROC_FS
	proc_register(&proc_root, &id_proc_entry);
	proc_register(&proc_root, &therm_proc_entry);
#endif
}
