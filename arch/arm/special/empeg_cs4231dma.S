/*
 * empeg_cs4231dma.S
 *
 * Copyright (C) 1999 Hugo Fiennes
 */
#include <linux/linkage.h>
#include <asm/assembler.h>
		.text

		/* On entry - r8=buffer, r9=buffer end, r10=dma reg, r11=GEDR,
		              r12=scratch, r13=scratch */
		/* GEDR=0xfa040018, ICLR=0xfa050008 */
		.global	SYMBOL_NAME(empeg_fiq4231_end)
ENTRY(empeg_fiq4231_start)
		mov	r12,#(1<<2)		/* DRQ */
	        str	r12,[r11,#0x18]		/* Clear GEDR */

1:		ldrb	r12,[r10]		/* Get low-byte */
		ldrb	r13,[r10]		/* Get high-byte */
		orr	r12,r12,r13,lsl#8
		strh	r12,[r8],#2		/* Save in buffer */
		ldr	r12,[r11,#0]		/* More samples? */
		tst	r12,#(1<<2)
		beq	1b

		cmp	r8,r9			/* Done a bufferload? */
		sublts	pc,lr,#4		/* No, return */

		mov	r13,#0xfa000000
		orr	r13,r13,#0x00050000
		ldr	r12,[r13,#8]		/* Modify ICLR */
		bic	r12,r12,#(1<<2)		/* Next is an IRQ */
		str	r12,[r13,#8]

		subs	pc,lr,#4		/* Return */
SYMBOL_NAME(empeg_fiq4231_end):
