/*
 * empeg IR driver FIQ section
 *
 * (C)2000 empeg ltd, http://www.empeg.com
 *
 * Authors:
 *   Mike Crowe, <mac@empeg.com>
 *   Hugo Fiennes, <hugo@empeg.com>
 *
 */

#include <linux/config.h>
#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/arch/hardware.h>
#include "empeg_input.h"
		.text
	
.set		BUFFERS_USED,		0
.set		BUFFERS_FREE,		4
.set		BUFFERS_HEAD,		8
.set		BUFFERS_TAIL,		12
.set		BUFFERS_POINTER,	16

		.global	SYMBOL_NAME(empeg_input_fiqend)
ENTRY(empeg_input_fiq)
		#   r8  - scratch
	        #   r9  - pointer to empeg device structure
		#   r10 - pointer to OSCR
		#   r11 - scratch
		#   r12 - scratch
		#   r13	- pointer to GPLR

		# First get the value of OSCR
		ldr	r8, [r10]

		# Now get the value of the IR bit in GPLR
		ldr	r12, [r13,#0]		@ GPLR

		# Check the IR input and set the bottom bit of the timer
		# value to indicate the state
		tst	r12, #(1<<EMPEG_IRINPUT_BIT)
		orrne	r8,r8,#1
		biceq	r8,r8,#1

		# Now we have our entry, we had better put it in the queue.
		# Is there room?
		ldr	r12, [r9, #BUFFERS_FREE]
		cmp	r12, #0
		beq	no_room

		# First decrement the free and store it.
		sub	r12, r12, #1
		str	r12, [r9, #BUFFERS_FREE]

		# Load up the head pointer
		ldr	r12, [r9, #BUFFERS_HEAD]

		# Load up the address of the queue and add on the head pointer
		ldr	r11, [r9, #BUFFERS_POINTER]

		# Store the value we have
		str	r8, [r11,r12,lsl#2]

		# Increment the head pointer, if we have reached the maximum
		# then set it to zero
		add	r12, r12, #1
		cmp	r12, #TIMINGS_BUFFER_SIZE
		moveq	r12, #0               @  This is wierd for a 68000 programmer like me :)

		# Store the head pointer back again.
		str	r12, [r9, #BUFFERS_HEAD]

		# Increment the used pointer
		ldr	r12, [r9, #BUFFERS_USED]
		add	r12, r12, #1
		str	r12, [r9, #BUFFERS_USED]

no_room:
		# Lastly, we need to clear GEDR so we get re-triggered
		mov	r8,#(1<<EMPEG_IRINPUT_BIT)
		str	r8,[r13,#0x18]		@ GEDR

		# Return
		subs	pc,lr,#4
SYMBOL_NAME(empeg_input_fiqend):
/* ENTRY(empeg_input_fiqend) */

