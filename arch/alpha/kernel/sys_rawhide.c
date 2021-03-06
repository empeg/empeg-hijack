/*
 *	linux/arch/alpha/kernel/sys_rawhide.c
 *
 *	Copyright (C) 1995 David A Rusling
 *	Copyright (C) 1996 Jay A Estabrook
 *	Copyright (C) 1998 Richard Henderson
 *
 * Code supporting the RAWHIDE.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/pci.h>
#include <linux/init.h>

#include <asm/ptrace.h>
#include <asm/system.h>
#include <asm/dma.h>
#include <asm/irq.h>
#include <asm/pci.h>
#include <asm/mmu_context.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/core_mcpcia.h>

#include "proto.h"
#include "irq.h"
#include "bios32.h"
#include "machvec.h"

static unsigned int hose_irq_masks[4] = { 0xff0000, 0xfe0000,
					  0xff0000, 0xff0000 };

static void
rawhide_update_irq_hw(unsigned long irq, unsigned long unused, int unmask_p)
{
	unsigned int saddle, hose, new_irq;
	unsigned long mask;

	saddle = (irq > 63); /* Which saddle are we on? */
	mask = _alpha_irq_masks[saddle]; /* Use the correct mask. */

	if (irq < 16) {
		if (irq < 8)
			outb(mask, 0x21);	/* ISA PIC1 */
		else
			outb(mask >> 8, 0xA1);	/* ISA PIC2 */
		return;
	}

	if (!saddle) {
		mask >>= 16; /* Saddle 0 includes EISA interrupts. */
		new_irq = irq - 16;
	} else {
		new_irq = irq - 64;
	}

	hose = (saddle << 1);

	if (new_irq >= 24) {
		mask >>= 24;
		hose++;
	}

	*(vuip)MCPCIA_INT_MASK0(hose) =
		(~(mask) & 0x00ffffffU) | hose_irq_masks[hose];
	mb();
	/* ... and read it back to make sure it got written.  */
	*(vuip)MCPCIA_INT_MASK0(hose);
}

static void 
rawhide_srm_device_interrupt(unsigned long vector, struct pt_regs * regs)
{
	int irq;

	irq = (vector - 0x800) >> 4;

        /*
         * The RAWHIDE SRM console reports PCI interrupts with a vector
	 * 0x80 *higher* than one might expect, as PCI IRQ 0 (ie bit 0)
	 * shows up as IRQ 24, etc, etc. We adjust it down by 8 to have
	 * it line up with the actual bit numbers from the REQ registers,
	 * which is how we manage the interrupts/mask. Sigh...
	 *
	 * also, PCI #1 interrupts are offset some more... :-(
         */
	if (irq == 52)
		irq = 72; /* SCSI on PCI 1 is special */

	/* Adjust by which hose it is from. */
	irq -= (((irq + 16) >> 2) & 0x38);

	handle_irq(irq, irq, regs);
}

static void __init
rawhide_init_irq(void)
{
	unsigned int hose;

	mcpcia_init_hoses();

	STANDARD_INIT_IRQ_PROLOG;

	/* HACK ALERT! routing is only to CPU #0. */

	for (hose = 0; hose < hose_count; hose++) {
		*(vuip)MCPCIA_INT_MASK0(hose) = hose_irq_masks[hose];
		mb();
		/* ... and read it back to make sure it got written.  */
		*(vuip)MCPCIA_INT_MASK0(hose);
	}

	enable_irq(2);
}

/*
 * PCI Fixup configuration.
 *
 * Summary @ MCPCIA_PCI0_INT_REQ:
 * Bit      Meaning
 * 0        Interrupt Line A from slot 2 PCI0
 * 1        Interrupt Line B from slot 2 PCI0
 * 2        Interrupt Line C from slot 2 PCI0
 * 3        Interrupt Line D from slot 2 PCI0
 * 4        Interrupt Line A from slot 3 PCI0
 * 5        Interrupt Line B from slot 3 PCI0
 * 6        Interrupt Line C from slot 3 PCI0
 * 7        Interrupt Line D from slot 3 PCI0
 * 8        Interrupt Line A from slot 4 PCI0
 * 9        Interrupt Line B from slot 4 PCI0
 * 10       Interrupt Line C from slot 4 PCI0
 * 11       Interrupt Line D from slot 4 PCI0
 * 12       Interrupt Line A from slot 5 PCI0
 * 13       Interrupt Line B from slot 5 PCI0
 * 14       Interrupt Line C from slot 5 PCI0
 * 15       Interrupt Line D from slot 5 PCI0
 * 16       EISA interrupt (PCI 0) or SCSI interrupt (PCI 1)
 * 17-23    NA
 *
 * IdSel	
 *   1	 EISA bridge (PCI bus 0 only)
 *   2 	 PCI option slot 2
 *   3	 PCI option slot 3
 *   4   PCI option slot 4
 *   5   PCI option slot 5
 * 
 */

static int __init
rawhide_map_irq(struct pci_dev *dev, int slot, int pin)
{
	static char irq_tab[5][5] __initlocaldata = {
		/*INT    INTA   INTB   INTC   INTD */
		{ 16+16, 16+16, 16+16, 16+16, 16+16}, /* IdSel 1 SCSI PCI 1 */
		{ 16+ 0, 16+ 0, 16+ 1, 16+ 2, 16+ 3}, /* IdSel 2 slot 2 */
		{ 16+ 4, 16+ 4, 16+ 5, 16+ 6, 16+ 7}, /* IdSel 3 slot 3 */
		{ 16+ 8, 16+ 8, 16+ 9, 16+10, 16+11}, /* IdSel 4 slot 4 */
		{ 16+12, 16+12, 16+13, 16+14, 16+15}  /* IdSel 5 slot 5 */
	};
	const long min_idsel = 1, max_idsel = 5, irqs_per_slot = 5;
	int irq = COMMON_TABLE_LOOKUP;
	if (irq >= 0)
		irq += 24 * bus2hose[dev->bus->number]->pci_hose_index;
	return irq;
}

static void __init
rawhide_pci_fixup(void)
{
	layout_all_busses(DEFAULT_IO_BASE, RAWHIDE_DEFAULT_MEM_BASE);
	common_pci_fixup(rawhide_map_irq, common_swizzle);
}


/*
 * The System Vector
 */

struct alpha_machine_vector rawhide_mv __initmv = {
	vector_name:		"Rawhide",
	DO_EV5_MMU,
	DO_DEFAULT_RTC,
	DO_MCPCIA_IO,
	DO_MCPCIA_BUS,
	machine_check:		mcpcia_machine_check,
	max_dma_address:	ALPHA_MAX_DMA_ADDRESS,

	nr_irqs:		128,
	irq_probe_mask:		_PROBE_MASK(128),
	update_irq_hw:		rawhide_update_irq_hw,
	ack_irq:		generic_ack_irq,
	device_interrupt:	rawhide_srm_device_interrupt,

	init_arch:		mcpcia_init_arch,
	init_irq:		rawhide_init_irq,
	init_pit:		generic_init_pit,
	pci_fixup:		rawhide_pci_fixup,
	kill_arch:		generic_kill_arch,
};
ALIAS_MV(rawhide)
