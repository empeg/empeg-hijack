	.globl	empeg_ramtest_critical_start
	.globl	empeg_ramtest_critical_end
	.globl	empeg_ramtest_preserve
	.globl	empeg_ramtest_destructive

#define PAGESIZE		4096
#define FLASHBASE		0x00000000

#define FLUSH_BASE		0xdf000000
#define FLUSH_BASE_MINICACHE	0xdf800000
#define MAIN_DCACHE_SIZE	16384
#define FLUSH_OFFSET		32768

#define UTDR			0x14
#define UTSR1			0x20
	
	.text

empeg_ramtest_critical_start:
	/* Don't wipe me */

Lclean_switch:
	.long	0

regs_save_preserve:
	.long	0	@ r4
	.long	0	@ r5
	.long	0	@ r6
	.long	0	@ r7
	.long	0	@ r8
	.long	0	@ r9
	.long	0	@ r10
	.long	0	@ r11
	.long	0	@ r12
	.long	0	@ lr

regs_save_destructive:
	.long	0	@ r4
	.long	0	@ r5
	.long	0	@ r6
	.long	0	@ r7
	.long	0	@ r8
	.long	0	@ r9
	.long	0	@ r10
	.long	0	@ r11
	.long	0	@ r12
	.long	0	@ lr

regs_save_wrapper:	
	.long	0	@ r4
	.long	0	@ r5
	.long	0	@ r6
	.long	0	@ lr
	
irqstate_save:
	.long	0

tablebase_save:	
	.long	0
	
domain_save:	
	.long	0

	
	/* on entry, r0 = test_page
		     r1 = 0 for low 16 bits
			  1 for high 16 bits
	   on exit,  r0 = failure code */
empeg_ramtest_destructive:
	adr	r3, regs_save_wrapper
	stmia	r3, {r4-r5, lr}

	mov	r4, r0
	mov	r5, r1
	bl	empeg_ramtest_mmu_off
	mov	r0, r4
	mov	r1, r5
	bl	empeg_ramtest_destructive_no_mmu
	mov	r4, r0
	bl	empeg_ramtest_mmu_on
	mov	r0, r4

	adr	r3, regs_save_wrapper
	ldmia	r3, {r4-r5, pc}

	
	/* on entry, r0 = safe_page (assumed)
		     r1 = test_page
		     r2 = 0 for low 16 bits
			  1 for high 16 bits
	   on exit,  r0 = failure code */
empeg_ramtest_preserve:
	adr	r3, regs_save_wrapper
	stmia	r3, {r4-r6, lr}
	
	mov	r4, r0
	mov	r5, r1
	mov	r6, r2
	bl	empeg_ramtest_mmu_off
	
	mov	r0, r4
	mov	r1, r5
	mov	r2, r6
	bl	empeg_ramtest_preserve_no_mmu
	mov	r4, r0

	bl	empeg_ramtest_mmu_on
	mov	r0, r4

	adr	r3, regs_save_wrapper
	ldmia	r3, {r4-r6, pc}

		
empeg_ramtest_mmu_off:
	@ Turn FIQ/IRQ off
	mrs	r0, cpsr
	and	r1, r0, #192
	str	r1, irqstate_save
	orr	r0, r0, #192
	msr	cpsr, r0

	@ Save domain and table base
	mrc	p15, 0, r1, c2, c0, 0	@ Get table base
	bic	r1,r1,#0x00003f00
	bic	r1,r1,#0x000000ff
	str	r1,tablebase_save
	mrc	p15, 0, r1, c3, c0, 0	@ Get domain register
	str	r1,domain_save

	@ main dcache
	mov	r2, #FLUSH_BASE
	add	r1, r2, #MAIN_DCACHE_SIZE
1:	ldr	r3, [r2], #32
	teq	r1, r2
	bne	1b

	@ mini dcache
	add	r2, r2, #(FLUSH_BASE_MINICACHE - FLUSH_BASE)
	add	r1, r2, #512	@ 512 bytes minicache
1:	ldr	r3, [r2], #32
	teq	r1, r2
	bne	1b

	mov	r1, #0
	mcr	p15, 0, r1, c7, c10, 4	@ drain WB
	mcr	p15, 0, r1, c7, c7, 0	@ flush I & D
	mcr	p15, 0, r1, c8, c7, 0   @ flush I & D tlbs
	
	mrc	p15, 0, r1, c1, c0, 0	@ Get control register
	bic	r1,r1,#0x000d		@ Turn off D-cache & MMU
	mcr	p15, 0, r1, c1, c0, 0
	mov	r0,r0
	mov	r0,r0
	mov	r0,r0	
	mov	pc,lr

	
empeg_ramtest_mmu_on:
	@ Load domain and table base
	ldr	r1,tablebase_save
	mcr	p15, 0, r1, c2, c0, 0	@ Get table base
	ldr	r1,domain_save
	mcr	p15, 0, r1, c3, c0, 0	@ Get domain register

	mov	r1, #0
	mcr	p15, 0, r1, c7, c7, 0	@ flush I & D caches
	mcr	p15, 0, r1, c7, c10, 4	@ drain WB
	mcr	p15, 0, r1, c8, c7, 0	@ flush I & D TLBs
	
	mrc	p15, 0, r1, c1, c0, 0	@ Get control register
	orr	r1, r1, #0x000d
	mcr	p15, 0, r1, c1, c0, 0	@ Turn on D-cache & MMU
	mov	r0, r0
	mov	r0, r0
	
	@ Turn FIQ/IRQ on again
	mrs	r0, cpsr
	bic	r0, r0, #192
	ldr	r1, irqstate_save
	orr	r0, r0, r1
	msr	cpsr, r0
		
	mov	pc,lr		

	@ corrupts r1
empeg_ramtest_putchar:
1:	mov	r1, #0x80000000
	orr	r1, r1, #0x50000
	ldrb	r1, [r1, #UTSR1]
	tst	r1, #4
	beq	1b
	mov	r1, #0x80000000
	orr	r1, r1, #0x50000
	strb	r0, [r1, #UTDR]
	mov	pc, lr
	
	/* failure codes:
		0 - success
		1 - zero test failed
		2 - all 1's test failed
		3 - 0x55 bit pattern failed
		4 - 0xaa bit pattern failed
		5 - 0x5555/0xaaaa bit pattern failed
		6 - 0xaaaa/0x5555 bit pattern failed
		7 - romcopy test failed
	*/
	
	/* on entry, r0 = safe_page (assumed)
		     r1 = test_page
		     r2 = 0 for low 16 bits
			  1 for high 16 bits
	   on exit,  r0 = failure code */


empeg_ramtest_preserve_no_mmu:
	adr	r3, regs_save_preserve
	stmia	r3, {r4-r12, lr}

	mov	r11, r0			@ safe_page
	mov	r12, r1			@ test_page
	mov	r10, r2			@ half

	/* loop through blat_page array */
	mov	r7, #PAGESIZE
1:	
	/* copy test_page to safe_page */
	mov	r3, r11
	mov	r4, r12
	mov	r5, #PAGESIZE
2:	ldr	r6, [r4], #4
	str	r6, [r3], #4
	subs	r5, r5, #4
	bne	2b

	mov	r0, r12
	mov	r1, r10
	bl	empeg_ramtest_destructive_no_mmu
	/* r0 is error code */

	/* copy r11 (safe_page) to r12 (test_page), 4096 bytes */
	mov	r3, r11
	mov	r4, r12
	mov	r5, #PAGESIZE
3:	ldr	r6, [r3], #4
	str	r6, [r4], #4
	subs	r5, r5, #4
	bne	3b

	/* r0 still contains error code */
	adr	r3, regs_save_preserve
	ldmia	r3, {r4-r12, pc}
	
	
	/* on entry, r0 = test_page
		     r1 = 0 for low 16 bits
			  1 for high 16 bits
	   on exit,  r0 = failure code */

empeg_ramtest_destructive_no_mmu:
	@ can't use stack, hmm...
	adr	r3, regs_save_destructive
	stmia	r3, {r4-r12, lr}

	mov	r10, r0			@ r10 = address of test_page
	tst	r1, #1
	moveq	r11, #0x000000ff
	movne	r11, #0x00ff0000
	orr	r11, r11, r11, lsl#8	@ make bank mask

	@ First, zero test
	mov	r0, #0
	bl	testram_fill
	mov	r0, #0
	bl	testram_check
	cmp	r0, #0
	movne	r0, #1
	bne	1f

	@ Now 0xff test
	mvn	r0, #0
	bl	testram_fill
	mvn	r0, #0
	bl	testram_check
	cmp	r0, #0
	movne	r0, #2
	bne	1f
	
	@ 0x55 alternate bit pattern
	ldr	r0,test_55
	bl	testram_fill
	ldr	r0,test_55
	bl	testram_check
	cmp	r0, #0
	movne	r0, #3
	bne	1f

	@ 0xaa alternate bit pattern
	ldr	r0,test_aa
	bl	testram_fill
	ldr	r0,test_aa
	bl	testram_check
	cmp	r0, #0
	movne	r0, #4
	bne	1f

	@ 0x5555/0xaaaa alternate words
	ldr	r0,test_55
	ldr	r1,test_aa
	bl	testram_fillalt
	ldr	r0,test_55
	ldr	r1,test_aa
	bl	testram_checkalt
	cmp	r0, #0
	movne	r0, #5
	bne	1f

	@ 0xaaaa/0x5555 alternate words
	ldr	r0,test_aa
	ldr	r1,test_55
	bl	testram_fillalt
	ldr	r0,test_aa
	ldr	r1,test_55
	bl	testram_checkalt
	cmp	r0, #0
	movne	r0, #6
	bne	1f

	@ romcopy test
	bl	testram_fillrom
	bl	testram_checkrom
	cmp	r0, #0
	movne	r0, #7
	bne	1f

	@mov	r0, r10
	@bl	sendhex32
	@mov	r0, #'\r'
	@bl	empeg_ramtest_putchar
	@mov	r0, #'\n'
	@bl	empeg_ramtest_putchar
	
	@ Pass
	mov	r0, #0
1:	/* failure jumps here with r0 set */

	adr	r3, regs_save_destructive
	ldmia	r3, {r4-r12, pc}
	
test_55:
	.word	0x55555555
test_aa:
	.word	0xaaaaaaaa

	
testram_fill:
	mov	r4,r0
	mov	r5,r0
	mov	r6,r0
	mov	r7,r0
	mov	r8,r10
	mov	r9,#PAGESIZE
1:	stmia	r8!,{r4-r7}
	subs	r9,r9,#16
	bne	1b
	mov	pc,lr

testram_check:
	and	r5,r0,r11
	mov	r0,#0
	mov	r8,r10
	mov	r9,#PAGESIZE
1:	ldr	r4,[r8],#4
	and	r4,r4,r11
	teq	r4,r5
	movne	r0,#1
	subs	r9,r9,#4
	bne	1b
	mov	pc,lr

testram_fillalt:
	mov	r4,r0
	mov	r5,r1
	mov	r6,r0
	mov	r7,r1
	mov	r8,r10
	mov	r9,#PAGESIZE
1:	stmia	r8!,{r4-r7}
	subs	r9,r9,#16
	bne	1b
	mov	pc,lr

testram_checkalt:
	and	r5,r0,r11
	mov	r0,#0
	and	r6,r1,r11	
	mov	r8,r10
	mov	r9,#PAGESIZE
1:	ldr	r4,[r8],#4
	and	r4,r4,r11
	teq	r4,r5
	movne	r0,#1
	ldr	r4,[r8],#4
	and	r4,r4,r11
	teq	r4,r6
	movne	r0,#1
	subs	r9,r9,#8
	bne	1b
	mov	pc,lr

testram_fillrom:
	mov	r8,r10
	mov	r9,#PAGESIZE
	mov	r6,#FLASHBASE
1:	ldr	r0,[r6],#4
	str	r0,[r8],#4
	subs	r9,r9,#4
	bne	1b
	mov	pc,lr

testram_checkrom:
	mov	r2,#0
	mov	r8,r10
	mov	r9,#PAGESIZE
	mov	r6,#FLASHBASE
1:	ldr	r0,[r6],#4
	and	r0,r0,r11
	ldr	r1,[r8],#4
	and	r1,r1,r11
	teq	r0,r1
	movne	r2,#1
	subs	r9,r9,#4
	bne	1b
	mov	r0,r2
	mov	pc,lr

	
sendhex32:
        mov     r3, #8
sendhexg:       
	mov	r2, #0x80000000
	orr	r2, r2, #0x50000
1:      ldrb    r1, [r2, #UTSR1]
        tst     r1, #4
        beq     1b
        and     r1, r0, #0xf0000000
        adr     r2, hextable
        ldrb    r1, [r2, r1, lsr#28]
	mov	r2, #0x80000000
	orr	r2, r2, #0x50000
        strb    r1, [r2, #UTDR]
        mov     r0, r0, lsl#4
        subs    r3, r3, #1
        bne     1b
        mov     pc, lr

hextable:
        .ascii  "0123456789abcdef"
        .align  0


empeg_ramtest_critical_end:
	/* You can wipe this lot though */



