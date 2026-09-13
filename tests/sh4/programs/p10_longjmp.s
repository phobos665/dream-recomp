! Non-local return: a context save/restore pair in the style of Katana's task switcher (Crazy Taxi
! 0x0c088560 / 0x0c0884ec). `_save` stores r8-r15 and PR into a block and returns 0. `_deep`, one
! call down and with a different stack pointer, calls `_restore`, which reloads the block and
! executes `rts` into `_main` right after the `bsr _save`, with r0 = 1. The emitted `rts` must
! not return into `_deep`'s host frame: `_deep` would continue with `_main`'s registers, which is
! how Crazy Taxi's task scheduler crashed on its first switch.
	.text
	.global _main
	.global _save
	.global _deep
	.global _restore
_main:
	sts.l	pr,@-r15
	mov	#5,r9
	mov.l	Lctx,r4
	bsr	_save
	nop
	tst	r0,r0
	bf	Lresumed
	mov	#1,r10
	bsr	_deep
	nop
	mov	#99,r11		! never executed: _deep does not return here
Lresumed:
	add	#10,r9		! r9 comes back from the block: 15
	lds.l	@r15+,pr
	rts
	nop
	.align 2
Lctx:	.long	0x8c00f000
_save:
	mov.l	r15,@r4
	mov.l	r14,@(4,r4)
	mov.l	r13,@(8,r4)
	mov.l	r12,@(12,r4)
	mov.l	r11,@(16,r4)
	mov.l	r10,@(20,r4)
	mov.l	r9,@(24,r4)
	mov.l	r8,@(28,r4)
	sts	pr,r0
	mov.l	r0,@(32,r4)
	rts
	mov	#0,r0
_deep:
	sts.l	pr,@-r15
	add	#-16,r15
	mov	#3,r3		! survives the switch: r3 is not in the block
	mov	#7,r9		! does not survive: r9 is restored from the block
	mov.l	Lctx2,r4
	bsr	_restore
	nop
	mov	#77,r12		! never executed
	add	#16,r15
	lds.l	@r15+,pr
	rts
	nop
	.align 2
Lctx2:	.long	0x8c00f000
_restore:
	mov.l	@r4,r15
	mov.l	@(4,r4),r14
	mov.l	@(8,r4),r13
	mov.l	@(12,r4),r12
	mov.l	@(16,r4),r11
	mov.l	@(20,r4),r10
	mov.l	@(24,r4),r9
	mov.l	@(28,r4),r8
	mov.l	@(32,r4),r0
	lds	r0,pr
	rts
	mov	#1,r0
