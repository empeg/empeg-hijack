/*
 * empeg_usbdma.S
 *
 * Copyright (C) 1998 Hugo Fiennes
 */
#include <linux/linkage.h>
#include <asm/assembler.h>
		.text

		/* On entry - r8=scratch, r9=dma base, r10=buffer, r11=GEDR, r12=scratch */
		.global	SYMBOL_NAME(empeg_fiqin_end)
ENTRY(empeg_fiqin_start)
		mov	r12,#0x10		/* FWEV register */
		strb	r12,[r9,#4]
		ldrb	r12,[r9]		/* Get FWEV */
		tst	r12,#0x40		/* RX event pending on FIFO1? */
		addeq	pc,pc,#24 
	
		mov	r12,#0x35		/* RXD2 register */
		strb	r12,[r9,#4]		/* Set address in USBN9602 */
	
		mov	r8,#16			/* We can get 16 bytes in one hit */
	
		ldrb	r12,[r9]		/* Get byte from FIFO */
		strb	r12,[r10],#1		/* Store it in buffer */
		subs	r8,r8,#1		/* Any more? */
		subgt	pc,pc,#20		/* Loop back to ldrb r12... */

		mov	r12,#0x14		/* DMACNTRL register */
		strb	r12,[r9,#4]		/* Set address in USBN9602 */
		
		mov	r12,#(1<<2)		/* DRQ */
	        str	r12,[r11,#0x18]		/* Clear GEDR */

		mov	r12,#0x0b		/* Disable DMA */
		strb	r12,[r9]
		mov	r12,#0x8b		/* Reenable DMA */
		strb	r12,[r9]

		ldr	r12,[r13]		/* Reset last address */
		strb	r12,[r9,#4]

		subs	pc,lr,#4		/* Return */
SYMBOL_NAME(empeg_fiqin_end):

		.global	SYMBOL_NAME(empeg_fiqout_end)
ENTRY(empeg_fiqout_start)
		subs	r9, r9, #1
		ldrgeb	r12, [r10], #1
		movlt	r12, #0
		strleb	r12, [r11], #0
		subles	pc, lr, #4
		strb	r12, [r11, #-4]
		subs	pc, lr, #4
SYMBOL_NAME(empeg_fiqout_end):
