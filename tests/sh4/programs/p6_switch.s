! Two switch idioms. r4 = case index (0..3), returns r0 = 10/20/30/40, or -1 for out of range.
! Part 1: GCC style, 16-bit offsets relative to the braf, table located with mova.
! Part 2: SHC style, absolute 32-bit addresses via jmp @rN, table via mov.l @(disp,pc).
	.text
	.global _sw
_sw:
	mov	#3,r1
	cmp/hi	r1,r4		! r4 > 3 ?
	bt	.Lbad
	mov	r4,r1
	shll	r1		! index * 2
	mova	.Ltab16,r0
	mov.w	@(r0,r1),r1	! offset
	braf	r1
	nop
.Lbase:
.Lc0:	mov	#10,r0
	bra	.Lpart2
	nop
.Lc1:	mov	#20,r0
	bra	.Lpart2
	nop
.Lc2:	mov	#30,r0
	bra	.Lpart2
	nop
.Lc3:	mov	#40,r0
	bra	.Lpart2
	nop
.Lbad:	rts
	mov	#-1,r0
	.align 2
.Ltab16:
	.word	.Lc0 - .Lbase
	.word	.Lc1 - .Lbase
	.word	.Lc2 - .Lbase
	.word	.Lc3 - .Lbase
.Lpart2:
	! r0 holds 10..40; now add 1..4 through an absolute-address jump table indexed by r4
	mov.l	.Ltab32p,r1
	mov	r4,r2
	shll2	r2
	mov.l	@(r0,r1),r3	! r0 is the value here, so use r2 as the index via r0 swap:
	nop
	mov	r0,r5		! save value
	mov	r2,r0		! r0 = index*4
	mov.l	@(r0,r1),r3	! r3 = absolute target
	mov	r5,r0		! r0 = value
	jmp	@r3
	nop
.Ld0:	rts
	add	#1,r0
.Ld1:	rts
	add	#2,r0
.Ld2:	rts
	add	#3,r0
.Ld3:	rts
	add	#4,r0
	.align 2
.Ltab32p: .long .Ltab32
.Ltab32:
	.long	.Ld0
	.long	.Ld1
	.long	.Ld2
	.long	.Ld3
