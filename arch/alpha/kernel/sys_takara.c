/*
 *	linux/arch/alpha/kernel/sys_takara.c
 *
 *	Copyright (C) 1995 David A Rusling
 *	Copyright (C) 1996 Jay A Estabrook
 *	Copyright (C) 1998, 1999 Richard Henderson
 *
 * Code supporting the TAKARA.
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
#include <asm/mmu_context.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/core_cia.h>

#include "proto.h"
#include "irq.h"
#include "bios32.h"
#include "machvec.h"


static void 
takara_update_irq_hw(unsigned long irq, unsigned long unused, int unmask_p)
{
	unsigned int regaddr;
	unsigned long mask;

	if (irq <= 15) {
		if (irq <= 7)
			outb(alpha_irq_mask, 0x21);		/* ISA PIC1 */
		else
			outb(alpha_irq_mask >> 8, 0xA1);	/* ISA PIC2 */
	} else {
		if (irq > 63)
			mask = _alpha_irq_masks[1] << 16;
		else
			mask = _alpha_irq_masks[0] >> ((irq - 16) & 0x30);

		regaddr = 0x510 + (((irq - 16) >> 2) & 0x0c);

		outl(mask & 0xffff0000UL, regaddr);
	}
}

static void
takara_device_interrupt(unsigned long vector, struct pt_regs *regs)
{
	unsigned intstatus;

	/*
	 * The PALcode will have passed us vectors 0x800 or 0x810,
	 * which are fairly arbitrary values and serve only to tell
	 * us whether an interrupt has come in on IRQ0 or IRQ1. If
	 * it's IRQ1 it's a PCI interrupt; if it's IRQ0, it's
	 * probably ISA, but PCI interrupts can come through IRQ0
	 * as well if the interrupt controller isn't in accelerated
	 * mode.
	 *
	 * OTOH, the accelerator thing doesn't seem to be working
	 * overly well, so what we'll do instead is try directly
	 * examining the Master Interrupt Register to see if it's a
	 * PCI interrupt, and if _not_ then we'll pass it on to the
	 * ISA handler.
	 */

	intstatus = inw(0x500) & 15;
	if (intstatus) {
		/*
		 * This is a PCI interrupt. Check each bit and
		 * despatch an interrupt if it's set.
		 */

		if (intstatus & 8) handle_irq(16+3, 16+3, regs);
		if (intstatus & 4) handle_irq(16+2, 16+2, regs);
		if (intstatus & 2) handle_irq(16+1, 16+1, regs);
		if (intstatus & 1) handle_irq(16+0, 16+0, regs);
	} else {
		isa_device_interrupt (vector, regs);
	}
}

static void 
takara_srm_device_interrupt(unsigned long vector, struct pt_regs * regs)
{
	int irq = (vector - 0x800) >> 4;

	handle_irq(irq, irq, regs);
}

static void __init
takara_init_irq(void)
{
	STANDARD_INIT_IRQ_PROLOG;

	if (alpha_using_srm)
		alpha_mv.device_interrupt = takara_srm_device_interrupt;
	else {
		unsigned int ctlreg = inl(0x500);

		/* Return to non-accelerated mode.  */
		ctlreg &= ~0x8000;
		outl(ctlreg, 0x500);

		/* Enable the PCI interrupt register.  */
		ctlreg = 0x05107c00;
		outl(ctlreg, 0x500);
	}

	enable_irq(2);
}

/*
 * The Takara has PCI devices 1, 2, and 3 configured to slots 20,
 * 19, and 18 respectively, in the default configuration. They can
 * also be jumpered to slots 8, 7, and 6 respectively, which is fun
 * because the SIO ISA bridge can also be slot 7. However, the SIO
 * doesn't explicitly generate PCI-type interrupts, so we can
 * assign it whatever the hell IRQ we like and it doesn't matter.
 */

static int __init
takara_map_irq_srm(struct pci_dev *dev, int slot, int pin)
{
	static char irq_tab[15][5] __initlocaldata = {
		{ 16+3, 16+3, 16+3, 16+3, 16+3},   /* slot  6 == device 3 */
		{ 16+2, 16+2, 16+2, 16+2, 16+2},   /* slot  7 == device 2 */
		{ 16+1, 16+1, 16+1, 16+1, 16+1},   /* slot  8 == device 1 */
		{   -1,   -1,   -1,   -1,   -1},   /* slot  9 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 10 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 11 == nothing */
#if 0
		{   -1,   -1,   -1,   -1,   -1},   /* slot 12 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 13 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 14 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 15 == nothing */
#else
		/* these are behind the bridges */
		{   12,   12,   13,   14,   15},   /* slot 12 == nothing */
		{    8,    8,    9,   19,   11},   /* slot 13 == nothing */
		{    4,    4,    5,    6,    7},   /* slot 14 == nothing */
		{    0,    0,    1,    2,    3},   /* slot 15 == nothing */
#endif
		{   -1,   -1,   -1,   -1,   -1},   /* slot 16 == nothing */
		{64+ 0, 64+0, 64+1, 64+2, 64+3},   /* slot 17= device 4 */
		{48+ 0, 48+0, 48+1, 48+2, 48+3},   /* slot 18= device 3 */
		{32+ 0, 32+0, 32+1, 32+2, 32+3},   /* slot 19= device 2 */
		{16+ 0, 16+0, 16+1, 16+2, 16+3},   /* slot 20= device 1 */
	};
	const long min_idsel = 6, max_idsel = 20, irqs_per_slot = 5;
        int irq = COMMON_TABLE_LOOKUP;
	if (irq >= 0 && irq < 16) { /* guess that we are behind a bridge */
	    unsigned int busslot = PCI_SLOT(dev->bus->self->devfn);
	    irq += irq_tab[busslot-min_idsel][0];
	}
	return irq;
}

static int __init
takara_map_irq(struct pci_dev *dev, int slot, int pin)
{
	static char irq_tab[15][5] __initlocaldata = {
		{ 16+3, 16+3, 16+3, 16+3, 16+3},   /* slot  6 == device 3 */
		{ 16+2, 16+2, 16+2, 16+2, 16+2},   /* slot  7 == device 2 */
		{ 16+1, 16+1, 16+1, 16+1, 16+1},   /* slot  8 == device 1 */
		{   -1,   -1,   -1,   -1,   -1},   /* slot  9 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 10 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 11 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 12 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 13 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 14 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 15 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 16 == nothing */
		{   -1,   -1,   -1,   -1,   -1},   /* slot 17 == nothing */
		{ 16+3, 16+3, 16+3, 16+3, 16+3},   /* slot 18 == device 3 */
		{ 16+2, 16+2, 16+2, 16+2, 16+2},   /* slot 19 == device 2 */
		{ 16+1, 16+1, 16+1, 16+1, 16+1},   /* slot 20 == device 1 */
	};
	const long min_idsel = 6, max_idsel = 20, irqs_per_slot = 5;
        return COMMON_TABLE_LOOKUP;
}

static int __init
takara_swizzle(struct pci_dev *dev, int *pinp)
{
	int slot = PCI_SLOT(dev->devfn);
	int pin = *pinp;
	unsigned int ctlreg = inl(0x500);
	unsigned int busslot = PCI_SLOT(dev->bus->self->devfn);

	/* Check first for built-in bridges.  */
	if (busslot > 16 && ((1U << (36 - busslot)) & ctlreg)) {
#if 0
		if (pin == 1)
			pin += (20 - busslot);
		else {
			/* Can only handle INTA pins currently.  */
			printk(KERN_WARNING "takara_swizzle: cannot only "
			       "handle cards with INTA IRQ pin now.\n");
		}
#else
#endif
	} else {
		/* Must be a card-based bridge.  */
		printk(KERN_WARNING "takara_swizzle: cannot handle "
		       "card-bridge behind builtin bridge yet.\n");
	}

	*pinp = pin;
	return slot;
}

static void __init
takara_pci_fixup(void)
{
	layout_all_busses(DEFAULT_IO_BASE, DEFAULT_MEM_BASE);
	if (alpha_using_srm)
		common_pci_fixup(takara_map_irq_srm, takara_swizzle);
	else
		common_pci_fixup(takara_map_irq, takara_swizzle);
	enable_ide(0x26e);
}


/*
 * The System Vector
 */

struct alpha_machine_vector takara_mv __initmv = {
	vector_name:		"Takara",
	DO_EV5_MMU,
	DO_DEFAULT_RTC,
	DO_CIA_IO,
	DO_CIA_BUS,
	machine_check:		cia_machine_check,
	max_dma_address:	ALPHA_MAX_DMA_ADDRESS,

	nr_irqs:		128,
	irq_probe_mask:		_PROBE_MASK(48),
	update_irq_hw:		takara_update_irq_hw,
	ack_irq:		generic_ack_irq,
	device_interrupt:	takara_device_interrupt,

	init_arch:		cia_init_arch,
	init_irq:		takara_init_irq,
	init_pit:		generic_init_pit,
	pci_fixup:		takara_pci_fixup,
	kill_arch:		generic_kill_arch,
};
ALIAS_MV(takara)
