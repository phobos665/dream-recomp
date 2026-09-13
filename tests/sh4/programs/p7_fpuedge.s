! FPU edge cases the differential harness arbitrates against Flycast (docs/differential-harness.md):
! FMAC fusing, FTRC saturation and special values, FCMP with NaN, FLOAT of extreme ints, a denormal
! input under DN=1, inexact FSQRT/FDIV/FSRRA, FIPR and FTRV summation order, FSCA at three angles,
! FCNVSD/FCNVDS under PR=1, and SZ=1 pair loads into the back bank. r4 = scratch buffer (32 bytes).
	.text
	.global _fpuedge
	.global _fpuedge_end
	.global _fpuedge_data
_fpuedge:
	mova	.Ldata,r0
	mov	r0,r5		! constant stream
	mov	r4,r12		! output cursor
	! ---- FMAC: is the product rounded before the add? ----
	fmov.s	@r5+,fr0	! 1+2^-23
	fmov.s	@r5+,fr1	! 1+2^-23
	fmov.s	@r5+,fr2	! -(1+2^-22)
	fmac	fr0,fr1,fr2	! fused: 2^-46; separately rounded: 0
	! ---- FTRC saturation and special values ----
	fmov.s	@r5+,fr3	! 3e9
	ftrc	fr3,fpul
	sts	fpul,r1
	fmov.s	@r5+,fr3	! -3e9
	ftrc	fr3,fpul
	sts	fpul,r2
	fmov.s	@r5+,fr3	! +inf
	ftrc	fr3,fpul
	sts	fpul,r3
	fmov.s	@r5+,fr3	! quiet NaN
	ftrc	fr3,fpul
	sts	fpul,r6
	fcmp/eq	fr3,fr3		! NaN == NaN -> 0
	movt	r10
	fcmp/gt	fr3,fr0		! unordered -> 0
	movt	r11
	fmov.s	@r5+,fr3	! 2^31
	ftrc	fr3,fpul
	sts	fpul,r7
	fmov.s	@r5+,fr3	! -(2^31+256)
	ftrc	fr3,fpul
	sts	fpul,r8
	fmov.s	@r5+,fr3	! -2.5
	ftrc	fr3,fpul
	sts	fpul,r9		! -2 (toward zero)
	fabs	fr3		! 2.5
	fsrra	fr3		! 1/sqrt(2.5)
	! ---- FLOAT of extreme ints ----
	mov.l	@r5+,r0		! 0x7FFFFFFF
	lds	r0,fpul
	float	fpul,fr4
	fmov.s	fr4,@r12	! [0]
	add	#4,r12
	mov.l	@r5+,r0		! 0x80000001
	lds	r0,fpul
	float	fpul,fr4
	fmov.s	fr4,@r12	! [4]
	add	#4,r12
	! ---- denormal input under DN=1 ----
	fmov.s	@r5+,fr4	! 0x00000001
	fadd	fr4,fr4		! flushed to 0, or 0x00000002 if denormals survive
	fmov.s	fr4,@r12	! [8]
	add	#4,r12
	! ---- inexact sqrt, div, reciprocal sqrt ----
	fldi1	fr12
	fadd	fr12,fr12	! 2.0
	fsqrt	fr12		! sqrt(2)
	fldi1	fr13
	fldi1	fr14
	fadd	fr14,fr14	! 2.0
	fadd	fr13,fr14	! 3.0
	fdiv	fr14,fr13	! 1/3
	fmov.s	fr13,@r12	! [12]
	add	#4,r12
	fsrra	fr13		! sqrt(3)
	! ---- FIPR with inexact products ----
	fmov.s	@r5+,fr4	! 0.1
	fmov.s	@r5+,fr5	! 0.2
	fmov.s	@r5+,fr6	! 0.3
	fmov.s	@r5+,fr7	! 0.4
	fmov.s	@r5+,fr8	! 1.1
	fmov.s	@r5+,fr9	! 2.2
	fmov.s	@r5+,fr10	! 3.3
	fmov.s	@r5+,fr11	! 4.4
	fipr	fv4,fv8		! fr11 = 0.11 + 0.44 + 0.99 + 1.76
	! ---- FTRV: load the matrix into the back bank ----
	frchg
	fmov.s	@r5+,fr0
	fmov.s	@r5+,fr1
	fmov.s	@r5+,fr2
	fmov.s	@r5+,fr3
	fmov.s	@r5+,fr4
	fmov.s	@r5+,fr5
	fmov.s	@r5+,fr6
	fmov.s	@r5+,fr7
	fmov.s	@r5+,fr8
	fmov.s	@r5+,fr9
	fmov.s	@r5+,fr10
	fmov.s	@r5+,fr11
	fmov.s	@r5+,fr12
	fmov.s	@r5+,fr13
	fmov.s	@r5+,fr14
	fmov.s	@r5+,fr15
	frchg
	ftrv	xmtrx,fv4	! fr4..fr7 = XMTRX * (0.1, 0.2, 0.3, 0.4)
	! ---- FSCA at 90, 45 and an arbitrary angle ----
	mov.l	@r5+,r0		! 0x4000
	lds	r0,fpul
	fsca	fpul,dr14	! fr14 = sin, fr15 = cos
	fmov.s	fr14,@r12	! [16]
	add	#4,r12
	fmov.s	fr15,@r12	! [20]
	add	#4,r12
	mov.l	@r5+,r0		! 0x2000
	lds	r0,fpul
	fsca	fpul,dr14
	fmov.s	fr14,@r12	! [24]
	add	#4,r12
	fmov.s	fr15,@r12	! [28]
	add	#4,r12
	mov.l	@r5+,r0		! 0x12345: only the low 16 bits count
	lds	r0,fpul
	fsca	fpul,dr14
	! ---- PR=1: FCNVSD, double FSQRT, FCNVDS ----
	mov.l	.Lfpscr_pr,r0
	lds	r0,fpscr
	flds	fr12,fpul	! sqrt(2) bits
	fcnvsd	fpul,dr0	! dr0 = (double)sqrt2f
	fsqrt	dr0
	fcnvds	dr0,fpul
	sts	fpul,r13	! (float)sqrt(sqrt(2)) bits
	mov.l	.Lfpscr_def,r0
	lds	r0,fpscr
	! ---- SZ=1 pair loads into the back bank ----
	fschg
	fmov	@r5+,xd0
	fmov	@r5+,xd2
	fschg
	rts
	nop
	.align 2
_fpuedge_end:
.Lfpscr_pr:	.long 0x000c0001
.Lfpscr_def:	.long 0x00040001
_fpuedge_data:
.Ldata:
	.long 0x3F800001, 0x3F800001, 0xBF800002
	.long 0x4F32D05E, 0xCF32D05E, 0x7F800000, 0x7FC00000, 0x4F000000, 0xCF000001, 0xC0200000
	.long 0x7FFFFFFF, 0x80000001
	.long 0x00000001
	.long 0x3DCCCCCD, 0x3E4CCCCD, 0x3E99999A, 0x3ECCCCCD
	.long 0x3F8CCCCD, 0x400CCCCD, 0x40533333, 0x408CCCCD
	.long 0x3FC00000, 0xC0100000, 0x3F400000, 0x42C84000
	.long 0xBA83126F, 0x40E00000, 0x3A83126F, 0x41480000
	.long 0xC0D80000, 0x40490FD0, 0x3F000000, 0xBF800000
	.long 0x40000000, 0x411FD70A, 0x3D800000, 0xC2053333
	.long 0x00004000, 0x00002000, 0x00012345
	.long 0x11111111, 0x22222222, 0x33333333, 0x44444444
