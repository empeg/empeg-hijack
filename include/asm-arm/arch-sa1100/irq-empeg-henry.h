/*
 * include/asm-arm/arch-sa1100/irq.h
 *
 * Copyright (C) 1996-1998 Russell King
 * Copyright (C) 1999 Hugo Fiennes
 *
 * Changelog:
 *   22-08-1998	RMK	Restructured IRQ routines
 *   06-01-1999 HBF     SA1100 twiddles
 *   30-01-1999 HBF     Hideous nastyness for Henry
 */

#ifndef CONFIG_EMPEG_HENRY
#error Why are you including this?
#endif



static unsigned int fixup_irq(unsigned int irq)
{
    if (irq==11 && (GEDR&GPIO_GPIO23)!=0)
    {
          /* Henry has IR input on GPIO23, whereas Sonja has it on GPIO4. Some
             skullduggery required here */

          /* Clear IRQ23 first */
          GEDR=GPIO_GPIO23;

          /* Oh look, that was GPIO4, wasn't it? */
	  return(4);
    }
    return(irq);
}

static void sa1100_mask_and_ack_irq(unsigned int irq)
{
        if (irq==4)
	  {
          /* IRQ4 is not what it seems */
          ICMR&=~(1<<11);
          GEDR=GPIO_GPIO23; 
	  }
	else
	  {
  	  ICMR &= ~((unsigned long)1<<irq);
	  if (irq<=10) GEDR=((unsigned long)1<<irq);
	  }
}

static void sa1100_mask_irq(unsigned int irq)
{
        if (irq==4)
	  {
	    /* Don't actually do this: the IDE may be using it */
	    /*ICMR &= ~(1<<11);*/
	  }
	else
	  ICMR &= ~((unsigned long)1<<irq);
}

static void sa1100_unmask_irq(unsigned int irq)
{
        if (irq==4)
          ICMR |= (1<<11);
	else
 	  ICMR |= ((unsigned long)1<<irq);
}
 
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

	/* Whatever the doc says, this has to be set for the wait-on-irq
	 * instruction to work... on a SA1100 rev 9 at least.
	 */
	ICCR = 1;

	restore_flags (flags);

	for (irq = 0; irq < NR_IRQS; irq++) {
		irq_desc[irq].valid	= 1;
		irq_desc[irq].probe_ok	= 1;
		irq_desc[irq].mask_ack	= sa1100_mask_and_ack_irq;
		irq_desc[irq].mask	= sa1100_mask_irq;
		irq_desc[irq].unmask	= sa1100_unmask_irq;
	}
}
