/*
 * linux/include/asm-arm/arch-sa1100/ide.h
 *
 * Copyright (c) 1997 Russell King
 *
 * Modifications:
 *  29-07-1998	RMK	Major re-work of IDE architecture specific code
 */
#include <asm/irq.h>

/*
 * Set up a hw structure for a specified data port, control port and IRQ.
 * This should follow whatever the default interface uses.
 */
static __inline__ void
ide_init_hwif_ports(hw_regs_t *hw, int data_port, int ctrl_port, int irq)
{
	ide_ioreg_t reg;
	int i;

	memset(hw, 0, sizeof(*hw));

#ifdef CONFIG_SA1100_EMPEG
/* The Empeg board has the first two address lines unused */
#define IO_SHIFT 2
#else
#define IO_SHIFT 0
#endif

	reg = (ide_ioreg_t) (data_port << IO_SHIFT);
	for (i = IDE_DATA_OFFSET; i <= IDE_STATUS_OFFSET; i++) {
		hw->io_ports[i] = reg;
		reg += (1 << IO_SHIFT);
	}
	hw->io_ports[IDE_CONTROL_OFFSET] = (ide_ioreg_t) (ctrl_port << IO_SHIFT);
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

#if defined( CONFIG_SA1100_EMPEG )
	/* Are HDDs still in reset? If so, we're out of temperature range.
	   Don't scan for HDDs */
	if ((GPLR&EMPEG_IDERESET)==0) {
		printk("empeg ide: outside environmental limits, skipping\n");
		return;
	}

	/* Check hardware revision */
	switch(empeg_hardwarerevision()) {
	case 5:  /* flateric has only one IDE bus */
	case 6: /* ...and marvin */
	case 7: /* ...and trillian */
	case 9: /* ...and seven (of 9) */
	case 105: /* modela has only one IDE bus too */
		printk("empeg single channel IDE\n");

		/* Control line setup */
		GPDR&=~(EMPEG_IDE1IRQ);
		GRER&=~(EMPEG_IDE1IRQ);
		GFER|=(EMPEG_IDE1IRQ);
		GEDR=(EMPEG_IDE1IRQ);

		ide_init_hwif_ports(&hw,0x00,0x0e,EMPEG_IRQ_IDE1);
		ide_register_hw(&hw, NULL);

		break;

	case 4:  /* Sonja & Kate have two IDE ports */
	default: /* If there's no id, just pretend... */
		printk("empeg dual channel IDE\n");

		/* Control line setup */
		GPDR&=~(EMPEG_IDE1IRQ|EMPEG_IDE2IRQ);
		GRER&=~(EMPEG_IDE1IRQ|EMPEG_IDE2IRQ);
		GFER|=(EMPEG_IDE1IRQ|EMPEG_IDE2IRQ);
		GEDR=(EMPEG_IDE1IRQ|EMPEG_IDE2IRQ);

		ide_init_hwif_ports(&hw,0x10,0x1e,EMPEG_IRQ_IDE2);
		ide_register_hw(&hw, NULL);
		ide_init_hwif_ports(&hw,0x00,0x0e,EMPEG_IRQ_IDE1);
		ide_register_hw(&hw, NULL);
		break;
	}

#elif defined( CONFIG_SA1100_VICTOR )
	/* Enable appropriate GPIOs as interrupt lines */
	GPDR &= ~GPIO_GPIO7;
	GRER |= GPIO_GPIO7;
	GFER &= ~GPIO_GPIO7;
	GEDR = GPIO_GPIO7;
	/* set the pcmcia interface timing */
	MECR = 0x00060006;

	ide_init_hwif_ports(&hw, 0x1f0, 0x3f6, IRQ_GPIO7);
	ide_register_hw(&hw, NULL);
#else
#error Missing IDE interface definition in include/asm/arch/ide.h
#endif
}
