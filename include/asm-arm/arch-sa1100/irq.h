/*
 * include/asm-arm/arch-sa1100/irq.h
 *
 * Copyright (C) 1996-1998 Russell King
 * Copyright (C) 1999 Hugo Fiennes
 *
 * Changelog:
 *   22-08-1998	RMK	Restructured IRQ routines
 *   06-01-1999 HBF     SA1100 twiddles
 *   12-02-1999 NP	added ICCR
 *   17-02-1999 NP	empeg henry ugly hacks now in a separate file ;)
 *   11-08-1999 PD      SA1101 support added
 */

#define fixup_irq(x) 	(x)

static void sa1100_mask_and_ack_irq(unsigned int irq)
{
        /* We don't need to ACK IRQs on the SA1100, unless they're <=10,
           ie an edge-detect. We can't clear the combined edge-detects on
	   other GPIOs as the irq11 handler needs to identify them */

	ICMR &= ~((unsigned long)1<<irq);
	if (irq<=10) GEDR=((unsigned long)1<<irq);
}

static void sa1100_mask_irq(unsigned int irq)
{
	ICMR &= ~((unsigned long)1<<irq);
}


static void sa1100_unmask_irq(unsigned int irq)
{
	ICMR |= ((unsigned long)1<<irq);
}

#ifdef CONFIG_SA1101

static void sa1101_mask_and_ack_lowirq(unsigned int irq)
{
	/* We have to mask and ack SA1101_IRQ too */
	INTENABLE0 &= ~((unsigned long)1<<(irq-32));
	GEDR = ((unsigned long)1<<SA1101_IRQ);
	INTSTATCLR0 = ((unsigned long)1<<(irq-32));
}

static void sa1101_mask_and_ack_highirq(unsigned int irq)
{
	/* We have to mask and ack SA1101_IRQ too */
	INTENABLE1 &= ~((unsigned long)1<<(irq-64));
	GEDR = ((unsigned long)1<<SA1101_IRQ);
	INTSTATCLR1 = ((unsigned long)1<<(irq-64));
}

static void sa1101_mask_lowirq(unsigned int irq)
{
	INTENABLE0 &= ~((unsigned long)1<<(irq-32));
}

static void sa1101_mask_highirq(unsigned int irq)
{
	INTENABLE1 &= ~((unsigned long)1<<(irq-64));
}


/* unmasking an IRQ with the wrong polarity can be
 * fatal, but there are no need to check this in
 * the interrupt code - it will be spotted anyway ;-)
 */
static void sa1101_unmask_lowirq(unsigned int irq)
{
	INTENABLE0 |= ((unsigned long)1<<(irq-32));
	ICMR |= ((unsigned long)1<<SA1101_IRQ);
}

static void sa1101_unmask_highirq(unsigned int irq)
{
	INTENABLE1 |= ((unsigned long)1<<(irq-64));
       	ICMR |= ((unsigned long)1<<SA1101_IRQ);
}

#endif

static __inline__ void irq_init_irq(void)
{
	unsigned long flags;
	int irq;

	save_flags_cli (flags);

        /* Disable all IRQs */
	ICMR = 0x00000000;

	/* All IRQs are IRQ, not FIQ */
	ICLR = 0x00000000;

	/* Clear all GPIO edge detects */
	GEDR = 0xffffffff;

#ifdef CONFIG_SA1101
	/* turn on interrupt controller */
	SKPCR |= 4;

	/* disable all irqs */
	INTENABLE0  = 0x00000000;
	INTENABLE1  = 0x00000000;

	/* detect on rising edge as default */
	INTPOL0     = 0x00000000;
	INTPOL1     = 0x00000000;

	/* clear all irqs */
	INTSTATCLR0 = 0xFFFFFFFF;
	INTSTATCLR1 = 0xFFFFFFFF;

	/* sa1101 generates a rising edge */
	GRER |= ((unsigned long)1<<SA1101_IRQ);
	GFER &= ~((unsigned long)1<<SA1101_IRQ);

#endif


	/* Whatever the doc says, this has to be set for the wait-on-irq
	 * instruction to work... on a SA1100 rev 9 at least.
	 */
	ICCR = 1;

	restore_flags (flags);

#ifndef CONFIG_SA1101
	for (irq = 0; irq < NR_IRQS; irq++) {
		irq_desc[irq].valid	= 1;
		irq_desc[irq].probe_ok	= 1;
		irq_desc[irq].mask_ack	= sa1100_mask_and_ack_irq;
		irq_desc[irq].mask	= sa1100_mask_irq;
		irq_desc[irq].unmask	= sa1100_unmask_irq;
	}
#else
	for (irq = 0; irq < 31; irq++) {
		irq_desc[irq].valid	= 1;
		irq_desc[irq].probe_ok	= 1;
		irq_desc[irq].mask_ack	= sa1100_mask_and_ack_irq;
		irq_desc[irq].mask	= sa1100_mask_irq;
		irq_desc[irq].unmask	= sa1100_unmask_irq;
	}
	for (irq = 32; irq < 63; irq++) {
		irq_desc[irq].valid	= 1;
		irq_desc[irq].probe_ok	= 1;
		irq_desc[irq].mask_ack	= sa1101_mask_and_ack_lowirq;
		irq_desc[irq].mask	= sa1101_mask_lowirq;
		irq_desc[irq].unmask	= sa1101_unmask_lowirq;
	}
	for (irq = 64; irq < NR_IRQS; irq++) {
		irq_desc[irq].valid	= 1;
		irq_desc[irq].probe_ok	= 1;
		irq_desc[irq].mask_ack	= sa1101_mask_and_ack_highirq;
		irq_desc[irq].mask	= sa1101_mask_highirq;
		irq_desc[irq].unmask	= sa1101_unmask_highirq;
	}
#endif
}
