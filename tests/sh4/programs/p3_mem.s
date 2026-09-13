! Memory forms and literal pools. r4 = scratch buffer address (RAM). Writes a pattern with several
! addressing modes, reads it back with others, and loads constants from the literal pool.
	.text
	.global _mem
_mem:
	mov	r4,r1
	mov.l	.Lk1,r0		! pc-relative literal
	mov.l	r0,@r1		! [0] = 0x11223344
	mov.w	.Lk2,r0		! sign-extended 16-bit literal (-2)
	mov.l	r0,@(4,r1)	! [4] = 0xfffffffe
	mov	#0x5a,r0
	mov.b	r0,@(8,r1)	! [8] = 0x5a
	mov	#9,r0
	mov	#0x7b,r2
	mov.b	r2,@(r0,r1)	! [9] = 0x7b
	mov	r1,r3
	add	#12,r3
	mov	#0x66,r2
	mov.w	r2,@-r3		! [10] = 0x0066 (r3 = base+10)
	mov.l	@r1,r5		! r5 = 0x11223344
	mov.l	@(4,r1),r6	! r6 = 0xfffffffe
	mov.b	@(8,r1),r0	! r0 = 0x5a
	mov	#9,r0
	mov.b	@(r0,r1),r7	! r7 = 0x7b
	mov.w	@r3+,r2		! r2 = 0x66, r3 = base+12
	mov	r1,r8
	mov.l	@r8+,r9		! r9 = 0x11223344, r8 = base+4
	mova	.Lk1,r0
	sub	r1,r3		! r3 = 12
	rts
	nop
	.align 2
.Lk1:	.long 0x11223344
.Lk2:	.word -2
	.word 0
