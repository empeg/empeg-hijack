/*
 * File: sa1101.c
 *
 * Routines for setting up the sa1101
 *
 * (NP) This code needs to be modified in order to use virtual mapped 
 *      definition, or be called with MMU off.
 */

#include <linux/config.h>
#include <asm/arch/hardware.h>
#include <asm/delay.h>


#define val_at( x )   ( *(volatile long *)(x))

#define DELAY_FIRST   28
#define DELAY_NEXT    28
#define RECOVERY_TIME  6

#define BANK_SETUP ((DELAY_FIRST<<3)|(DELAY_NEXT<<8)|(RECOVERY_TIME<<13))

void setup_sa1101(void)
{

    /* Set up SA1100 to support SA1100:
     *  SRAM-bank 2 is set to Flash-ROM type
     *  GPIO 21 & 22 is MBREQ and MBGNT
     *  GPIO 27 is 3.68 MHz osc.
     */
    
#if (SA1101_BASE == 0x08000000 )  /* bank 1 */
    val_at(_MSC0) = (val_at(_MSC0) & 0x0000ffff ) | (BANK_SETUP << 16);
#elif (SA1101_BASE == 0x10000000 ) /* bank 2 */
    val_at(_MSC1) = (val_at(_MSC1) & 0xffff0000 ) | BANK_SETUP;
#elif (SA1101_BASE == 0x18000000 ) /* bank 3 */
    val_at(_MSC1) = (val_at(_MSC1) & 0x0000ffff ) | (BANK_SETUP << 16);
#endif

    val_at(_GPDR) |= 0x08200000;       /* Outp. 27 CLK, 21 MBGNT*/
    val_at(_GPDR) &= ~0x00400000;      /* Inp. 22 MBREQ */
    val_at(_GAFR) |= 0x08600000;       /* GAFR  27          1,2 */
    val_at(_TUCR) |= 0x20000400;       /* 3.68 Mhz MBRQ,GNT */


    do {    /* Wait for sa1101 to wake up */
            val_at(_SKCR) = 0x19;              
    }while( val_at(_SKCR) == 0x00 );   

    udelay(1000);
        
    val_at(_SKCR) = 0x1b; /* enable sa1101      */      
    val_at(_SKPCR) = 0x6; /* skpcr enable ints + ps2 */

    printk("SA1101 initialized\n");
}
