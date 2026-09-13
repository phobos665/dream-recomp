! A function whose second instruction is also a discovered entry (Crazy Taxi has a pointer seed
! one instruction into a prologue): discovery trims `split` to two bytes and the emitter must tail-
! call `tail` where control falls off the end instead of faulting. r4 = input.
	.text
	.global _split
	.global _tail
_split:
	mov.l	r14,@-r15	! prologue; discovery also seeds the next instruction
_tail:
	mov	#-4,r3
	mov	#100,r0
	shad	r3,r0		! 100 >> 4 = 6
	mov	r4,r14
	add	#3,r14
	add	r14,r0
	mov	r0,r1
	mov.l	@r15+,r14
	rts
	nop
