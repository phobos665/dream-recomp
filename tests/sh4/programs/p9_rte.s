! RTE used as a jump, the way Katana's startup hands the first task its entry point: SPC and SSR are
! loaded by hand and `rte` continues at SPC with no exception frame active. The emitted code must
! transfer control to SPC (a translated function here) rather than return to its C++ caller.
! SR is carried in r8 (not banked): the Flycast oracle swaps r0-r7 on its first SR write because
! its bank-tracking copy of SR starts stale, which real hardware and the recompiler do not do.
	.text
	.global _rtejump
	.global _target
_rtejump:
	mova	_target,r0
	ldc	r0,spc
	stc	sr,r8
	ldc	r8,ssr
	rte
	nop
	.align 2
_target:
	mov	#7,r0
	rts
	nop
