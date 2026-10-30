! A conditional branch into the delay slot of a `jmp`, reaching the rest of the function, as
! Daytona USA 2001 does at 0x0c0244e0: with r4 = 0 the `bt.s` enters the slot, which runs as an
! ordinary instruction, and control carries on after the jmp pair. The harness gives the function a
! range that ends right after the slot, so the continuation is a function of its own and the slot
! copy must tail-call it rather than return.
! Linked at -Ttext=0x8c040000 -e 0x8c040000: the jump resolves its target through the function
! table by address, which must not be shared with the programs linked at 0x8c010000.
	.text
	.global _slotjoin
	.global _slotjoin_tail
	.global _slotjoin_other
_slotjoin:
	mov	r4,r0
	tst	r0,r0
	bt/s	Lslot		! r4 == 0: enter the slot of the jmp below
	add	#1,r5
	jmp	@r0
Lslot:
	add	#2,r5		! the jmp's delay slot, and the bt/s target
_slotjoin_tail:
	mov	r5,r0		! reached only through the slot
	rts
	add	#100,r0
_slotjoin_other:
	rts
	mov	#7,r0
