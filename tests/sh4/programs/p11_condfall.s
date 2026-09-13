! A function whose last instruction is a conditional branch: the loop in `_loop` exits by falling
! through into the next function, `_done`, which must then run (r1 = 7) and return to the caller.
! Crazy Taxi's kmiFBgetfreememEx is split this way by discovery; the emitter used to let the
! not-taken path fall off the end of the C++ function.
	.text
	.global _loop
	.global _done
_loop:
	add	#1,r0
	dt	r4
	bf	_loop
_done:
	mov	#7,r1
	rts
	nop
