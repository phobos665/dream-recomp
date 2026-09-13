! FPU under the Katana default FPSCR (PR=0, SZ=0), a double-precision section entered through
! `mov.l @(disp,pc),rN ; lds rN,fpscr` (which the emitter's analysis must see), a pair move under
! SZ=1 via fschg, and a return to the default mode. r4 = scratch buffer.
	.text
	.global _fpu
_fpu:
	fldi1	fr0		! 1.0
	fldi1	fr1
	fadd	fr0,fr1		! fr1 = 2.0
	fmov	fr1,fr2
	fmul	fr1,fr2		! fr2 = 4.0
	fsqrt	fr2		! fr2 = 2.0
	fldi0	fr3
	fsub	fr1,fr3		! fr3 = -2.0
	fabs	fr3		! 2.0
	fneg	fr3		! -2.0
	mov	#7,r0
	lds	r0,fpul
	float	fpul,fr4	! 7.0
	fdiv	fr1,fr4		! 3.5
	ftrc	fr4,fpul	! 3
	sts	fpul,r1		! r1 = 3
	fmac	fr0,fr1,fr4	! fr4 = 1*2 + 3.5 = 5.5
	fcmp/gt	fr1,fr4		! 5.5 > 2 -> T
	movt	r2		! r2 = 1
	fldi1	fr5		! 1.0
	mov	#0,r0
	lds	r0,fpul
	fsca	fpul,dr6	! angle 0: fr6 = sin = 0, fr7 = cos = 1
	fipr	fv0,fv4		! fr7 = 1*5.5 + 2*1 + 2*0 + (-2)*1 = 5.5
	fmov.s	fr1,@r4		! [0] = 2.0f
	mov	r4,r5
	add	#4,r5
	fmov.s	fr4,@r5		! [4] = 5.5f
	add	#4,r5
	fmov.s	fr7,@r5		! [8] = 5.5f (fipr result)
	! ---- double precision ----
	mov.l	.Lfpscr_pr,r0
	lds	r0,fpscr	! PR=1
	mov	#3,r0
	lds	r0,fpul
	float	fpul,dr8	! dr8 = 3.0
	mov	#4,r0
	lds	r0,fpul
	float	fpul,dr10	! dr10 = 4.0
	fadd	dr8,dr10	! dr10 = 7.0
	fcmp/gt	dr8,dr10	! T = 1
	movt	r3
	ftrc	dr10,fpul
	sts	fpul,r6		! r6 = 7
	mov.l	.Lfpscr_def,r0
	lds	r0,fpscr	! back to PR=0, SZ=0
	! ---- pair move ----
	fschg			! SZ=1
	mov	r4,r5
	add	#12,r5
	fmov	dr10,@r5	! [12..19] = 7.0 as a double, high word first
	fschg			! SZ=0
	rts
	nop
	.align 2
.Lfpscr_pr:	.long 0x000c0001
.Lfpscr_def:	.long 0x00040001
