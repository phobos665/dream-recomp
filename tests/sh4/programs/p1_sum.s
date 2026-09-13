! square(n): r0 = r4 * r4 by adding r4 to r0 n times, via dt/bf.s with the add in the delay slot.
! Exercises: loop back-edge, T bit, delayed conditional branch, delay slot executed on both paths.
	.text
	.global _sum
_sum:
	mov	#0,r0
	mov	r4,r1
	cmp/pl	r1
	bf	.Ldone
.Lloop:
	dt	r1
	bf.s	.Lloop
	add	r4,r0		! slot: r0 += r4, executes whether or not the branch is taken
	nop
.Ldone:
	rts
	nop
