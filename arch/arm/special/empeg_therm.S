.global empeg_readtherm
.global empeg_inittherm
	
.set	OSMR0,		0x00
.set	OSCR,		0x10
.set	OSSR,		0x14
	
.set	GPLR,		0x00
.set	GPDR,		0x04
.set	GPSR,		0x08
.set	GPCR,		0x0c
.set	GAFR,		0x1c
		
@------------------------------------------------------------------------------
@ TEMPERATURE SENSOR TESTS
@ r0=timerbase
@ r1=gpiobase
empeg_inittherm:
	stmfd	r13!,{r1-r12,r14}
	mov	r9,r0
	mov	r10,r1

	@ Start conversions
	bl	onewire_reset
	mvnne	r0,#0			@ Reset fail?
	ldmnefd r13!,{r1-r12,pc}	@ Return -1

	mov	r0,#0xee
	bl	onewire_writebyte

	mov	r0,#0
	ldmfd	r13!,{r1-r12,pc}
	
empeg_readtherm:
	stmfd	r13!,{r1-r12,r14}
	mov	r9,r0
	mov	r10,r1

	@ Read temperature
	bl	onewire_reset
	mvnne	r0,#0			@ Reset fail?
	ldmnefd r13!,{r1-r12,pc}	@ Return -1
	
	mov	r0,#0xaa
	bl	onewire_writebyte
	bl	onewire_readbyte

	@ Return temperature in R0
	ldmfd	r13!,{r1-r12,pc}

onewire_reset:
	stmfd	r13!,{r14}
	
	@ Drive it low
	mov	r0,#(1<<26)
	str	r0,[r10,#GPCR]
	ldr	r0,[r10,#GPDR]
	orr	r0,r0,#(1<<26)
	str	r0,[r10,#GPDR]

	@ Wait for 480us->960us
	ldr	r1,onewire_600us
	ldr	r0,[r9,#OSCR]
	add	r1,r1,r0
1:	ldr	r0,[r9,#OSCR]
	cmp	r0,r1
	blt	1b

	@ Let line float
	ldr	r0,[r10,#GPDR]
	bic	r0,r0,#(1<<26)
	str	r0,[r10,#GPDR]

	@ Wait 600us max for pulse from DS1821
	ldr	r1,onewire_600us
	ldr	r0,[r9,#OSCR]
	add	r1,r1,r0
1:	ldr	r0,[r10,#GPLR]
	tst	r0,#(1<<26)		@ Gone low?
	beq	2f
	ldr	r0,[r9,#OSCR]
	cmp	r0,r1
	blt	1b

	@ Didnt get pulse
	movs	r0,#1
	ldmfd	r13!,{pc}
			
2:	@ Wait until end of slot
	ldr	r0,[r9,#OSCR]
	cmp	r0,r1
	blt	2b

	movs	r0,#0	
	ldmfd	r13!,{pc}


onewire_writebyte:
	stmfd	r13!,{r14}
	mov	r4,r0
	mov	r5,#8
1:	tst	r4,#1
	moveq	r0,#0
	movne	r0,#1
	bl	onewire_write
	mov	r4,r4,lsr#1
	subs	r5,r5,#1
	bne	1b
	ldmfd	r13!,{pc}

onewire_readbyte:
	stmfd	r13!,{r14}
	mov	r4,#0
	mov	r5,#8
1:	bl	onewire_read
	mov	r4,r4,lsr#1
	orr	r4,r4,r0,lsl#7
	subs	r5,r5,#1
	bne	1b
	mov	r0,r4
	ldmfd	r13!,{pc}
	
onewire_write:	
	@ Drive it low
	mov	r1,#(1<<26)
	str	r1,[r10,#GPCR]
	ldr	r1,[r10,#GPDR]
	orr	r1,r1,#(1<<26)
	str	r1,[r10,#GPDR]

	cmp	r0,#0
	moveq	r2,#250			@ Doing a zero - long slot
	movne	r2,#10			@ One - short slot
	
	ldr	r1,[r9,#OSCR]
	add	r2,r2,r1
	add	r3,r1,#255
1:	ldr	r1,[r9,#OSCR]		@ Wait for timer to expire
	cmp	r1,r2
	blt	1b

	@ Let line float
	ldr	r2,[r10,#GPDR]
	bic	r2,r2,#(1<<26)
	str	r2,[r10,#GPDR]

	@ Recovery time (up to 64us slot time)
1:	ldr	r1,[r9,#OSCR]		@ Wait for timer to expire
	cmp	r1,r3
	blt	1b

 	mov	pc,r14

onewire_read:	
	@ Drive it low
	mov	r0,#(1<<26)
	str	r0,[r10,#GPCR]
	ldr	r0,[r10,#GPDR]
	orr	r0,r0,#(1<<26)
	str	r0,[r10,#GPDR]

	@ Wait for 1us
	ldr	r1,[r9,#OSCR]
	add	r1,r1,#8
	add	r3,r1,#16
1:	ldr	r0,[r9,#OSCR]
	cmp	r0,r1
	blt	1b
	
	@ Let it float
	ldr	r2,[r10,#GPDR]
	bic	r2,r2,#(1<<26)
	str	r2,[r10,#GPDR]

	@ Wait for 10us for line to be pulled high if necessary
1:	ldr	r0,[r9,#OSCR]
	cmp	r0,r3
	bne	1b

	@ Read bit
	ldr	r1,[r10,#GPLR]

	@ Entire slot should be 64us
	add	r3,r3,#(255-16)
1:	ldr	r0,[r9,#OSCR]
	cmp	r0,r3
	blt	1b

	@ Work out what we got back
	tst	r1,#(1<<26)
	moveq	r0,#0
	movne	r0,#1	
	mov	pc,r14
	
onewire_600us:
	.word	2212
