! ALU helpers: r0 = 100/7 via the manual's unsigned 32-bit DIV1 sequence, r1 = remainder-ish,
! r2 = shad/shld results, r3 = addc chain carry test, r4 = rotcl/rotcr, r5 = swap/ext, r6 = mac.
	.text
	.global _alu
_alu:
	mov	#100,r0		! dividend, fed into the divider one bit per step
	mov	#7,r1		! divisor
	mov	#0,r2		! working (remainder) register
	div0u
	.rept 32
	rotcl	r0
	div1	r1,r2		! manual's unsigned 32/32 sequence
	.endr
	rotcl	r0		! r0 = quotient (14)
	extu.b	r0,r0		! keep it small
	mov	#-3,r2
	mov	#8,r3
	shad	r2,r3		! r3 = 8 >> 3 = 1
	mov	#0x40,r2
	mov	#-4,r5
	shld	r5,r2		! r2 = 0x40 >> 4 = 4
	add	r3,r2		! r2 = 5
	mov	#-1,r3
	clrt
	addc	r3,r3		! r3 = -1 + -1 + 0 = 0xfffffffe, T=1
	mov	#0,r5
	addc	r5,r5		! r5 = 0 + 0 + T(1) = 1
	mov	#1,r4
	sett
	rotcl	r4		! r4 = 3, T=0
	rotcr	r4		! r4 = 1, T=1 (bit0 of 3 was 1)
	movt	r6		! r6 = 1
	mov	#0x12,r7
	shll8	r7
	or	#0x34,r0	! r0 = 14 | 0x34 = 0x3e
	swap.b	r7,r7		! 0x1200 -> 0x0012
	exts.b	r3,r3		! 0xfffffffe -> -2
	rts
	nop
