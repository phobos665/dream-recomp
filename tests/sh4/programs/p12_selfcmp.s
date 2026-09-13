! A register compared with itself. SH-4 code does this to set or clear T in one instruction, and
! Charge 'N Blast does (`cmp/hi r15,r15`). Emitted literally it becomes `c.r[15] > c.r[15]`, which
! -Wtautological-compare rejects under -Werror, so a title using the idiom would not build at all.
! The emitter now emits the constant the hardware produces; these are the constants.
!
! Each comparison's T is stored as one byte, so a wrong answer names itself rather than showing up
! as one bad bit in a total. Run twice, once with a positive register and once with a negative one,
! because the signed forms are the ones where "compared with itself" could plausibly differ.
	.text
	.global _selfcmp
_selfcmp:
	mov	#5,r1
	mov	#-1,r3
	mov.l	.Lbuf,r4
	cmp/eq	r1,r1		! 1
	movt	r0
	mov.b	r0,@r4
	add	#1,r4
	cmp/hs	r1,r1		! 1
	movt	r0
	mov.b	r0,@r4
	add	#1,r4
	cmp/ge	r1,r1		! 1
	movt	r0
	mov.b	r0,@r4
	add	#1,r4
	cmp/hi	r1,r1		! 0
	movt	r0
	mov.b	r0,@r4
	add	#1,r4
	cmp/gt	r1,r1		! 0
	movt	r0
	mov.b	r0,@r4
	add	#1,r4
	cmp/eq	r3,r3		! 1
	movt	r0
	mov.b	r0,@r4
	add	#1,r4
	cmp/hs	r3,r3		! 1
	movt	r0
	mov.b	r0,@r4
	add	#1,r4
	cmp/ge	r3,r3		! 1
	movt	r0
	mov.b	r0,@r4
	add	#1,r4
	cmp/hi	r3,r3		! 0
	movt	r0
	mov.b	r0,@r4
	add	#1,r4
	cmp/gt	r3,r3		! 0
	movt	r0
	mov.b	r0,@r4
	rts
	nop
	.align 2
.Lbuf:	.long	0x8c020000
