/*
 *  linux/include/asm-alpha/ide.h
 *
 *  Copyright (C) 1994-1996  Linus Torvalds & authors
 */

/*
 *  This file contains the alpha architecture specific IDE code.
 */

#ifndef __ASMalpha_IDE_H
#define __ASMalpha_IDE_H

#ifdef __KERNEL__

#ifndef MAX_HWIFS
#define MAX_HWIFS	4
#endif

#define ide__sti()	__sti()

/*
 * Set up a hw structure for a specified data port, control port and IRQ.
 * This should follow whatever the default interface uses.
 */
static __inline__ void
ide_init_hwif_ports(hw_regs_t *hw, int data_port, int ctrl_port, int irq)
{
	ide_ioreg_t reg = (ide_ioreg_t) data_port;
	int i;

	memset(hw, 0, sizeof(*hw));

	for (i = IDE_DATA_OFFSET; i <= IDE_STATUS_OFFSET; i++) {
		hw->io_ports[i] = reg;
		reg += 1;
	}
	hw->io_ports[IDE_CONTROL_OFFSET] = (ide_ioreg_t) ctrl_port;
	hw->irq = irq;
}

/*
 * This registers the standard ports for this architecture with the IDE
 * driver.
 */
static __inline__ void
ide_init_default_hwifs(void)
{
	hw_regs_t hw;

	ide_init_hwif_ports(&hw, 0x1f0, 0x3f6, 14);
	ide_register_hw(&hw, NULL);
	ide_init_hwif_ports(&hw, 0x170, 0x376, 15);
	ide_register_hw(&hw, NULL);
	ide_init_hwif_ports(&hw, 0x1e8, 0x3ee, 11);
	ide_register_hw(&hw, NULL);
	ide_init_hwif_ports(&hw, 0x168, 0x36e, 10);
	ide_register_hw(&hw, NULL);
}

typedef union {
	unsigned all			: 8;	/* all of the bits together */
	struct {
		unsigned head		: 4;	/* always zeros here */
		unsigned unit		: 1;	/* drive select number, 0 or 1 */
		unsigned bit5		: 1;	/* always 1 */
		unsigned lba		: 1;	/* using LBA instead of CHS */
		unsigned bit7		: 1;	/* always 1 */
	} b;
	} select_t;

static __inline__ int ide_request_irq(unsigned int irq, void (*handler)(int, void *, struct pt_regs *),
			unsigned long flags, const char *device, void *dev_id)
{
	return request_irq(irq, handler, flags, device, dev_id);
}			

static __inline__ void ide_free_irq(unsigned int irq, void *dev_id)
{
	free_irq(irq, dev_id);
}

static __inline__ int ide_check_region (ide_ioreg_t from, unsigned int extent)
{
	return check_region(from, extent);
}

static __inline__ void ide_request_region (ide_ioreg_t from, unsigned int extent, const char *name)
{
	request_region(from, extent, name);
}

static __inline__ void ide_release_region (ide_ioreg_t from, unsigned int extent)
{
	release_region(from, extent);
}

/*
 * The following are not needed for the non-m68k ports
 */
#define ide_ack_intr(hwif)		(1)
#define ide_fix_driveid(id)		do {} while (0)
#define ide_release_lock(lock)	 	do {} while (0)
#define ide_get_lock(lock, hdlr, data)	do {} while (0)

#endif /* __KERNEL__ */

#endif /* __ASMalpha_IDE_H */
