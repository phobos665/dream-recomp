! Calls, returns and the two delay-slot hazards. r0 = f(r4) where f uses bsr/jsr/rts and a slot
! that modifies the register the jsr read. Expected: r0 = ((r4 + 1) * 2) + 100 + 5.
	.text
	.global _calls
_calls:
	sts.l	pr,@-r15
	mov	r4,r0
	bsr	_inc		! r0 = r4 + 1
	nop
	mov	r0,r2
	mova	.Ldbl,r0	! r0 = address of the pool word holding _dbl
	mov.l	@r0,r1		! r1 = _dbl
	mov	r2,r0
	jsr	@r1		! call _dbl ...
	mov	#0,r1		! ... while the slot clobbers r1: must not affect the target
	add	#100,r0
	bra	.Ltail
	add	#5,r0		! slot executes before the jump: r0 += 5
	add	#77,r0		! skipped
.Ltail:
	lds.l	@r15+,pr
	rts
	nop
	.global _inc
_inc:
	rts
	add	#1,r0		! slot after rts still executes
	.global _dbl
_dbl:
	add	r0,r0
	rts
	nop
	.align 2
.Ldbl:	.long _dbl
