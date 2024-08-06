.cfi_sections .debug_frame, .eh_frame

.globl c_trap
.global c_thread_trampoline

#define FRAME_RA 0*8
#define FRAME_SP 1*8
#define FRAME_GP 2*8
#define FRAME_TP 3*8
#define FRAME_T0 4*8
#define FRAME_T1 5*8
#define FRAME_T2 6*8
#define FRAME_S0 7*8
#define FRAME_S1 8*8
#define FRAME_A0 9*8
#define FRAME_A1 10*8
#define FRAME_A2 11*8
#define FRAME_A3 12*8
#define FRAME_A4 13*8
#define FRAME_A5 14*8
#define FRAME_A6 15*8
#define FRAME_A7 16*8
#define FRAME_S2 17*8
#define FRAME_S3 18*8
#define FRAME_S4 19*8
#define FRAME_S5 20*8
#define FRAME_S6 21*8
#define FRAME_S7 22*8
#define FRAME_S8 23*8
#define FRAME_S9 24*8
#define FRAME_S10 25*8
#define FRAME_S11 26*8
#define FRAME_T3 27*8
#define FRAME_T4 28*8
#define FRAME_T5 29*8
#define FRAME_T6 30*8
#define FRAME_SEPC 31*8
#define FRAME_SSTATUS 32*8
#define FRAME_SCAUSE 33*8
#define FRAME_STVAL 34*8
#define FRAME_IS_USERMODE 35*8
#define FRAME_SIZE 36*8

#define SAVE(R, OFF) \
	sd 		R, (OFF)(sp);\
	.cfi_rel_offset R, OFF

#define PUSH_WITH_CFI(R, OFF) \
	sd 		R, (OFF)(sp);\
	.cfi_rel_offset R, OFF

#define LOAD(R, OFF) \
	sd 		R, (OFF)(sp);\
	.cfi_rel_offset R, OFF

.globl trap
.align 8
trap:
	.cfi_startproc simple
	# doesn't work - why?!
	#.cfi_signal_frame
	.cfi_def_cfa	sp, 0

	/* swap tp and sscratch; if then tp = 0, branch as trapped supervisor */
	csrrw	tp, sscratch, tp
	beqz	tp, 1f

	/* this is a trap from user mode */
	// todo code
	wfi

	1:
	/* this is a trap from supervisor mode */
	/* restore tp */
	csrrw	tp, sscratch, zero
	/* make space for the trap frame */
	addi	sp, sp, -FRAME_SIZE
	/* write 0 to indicate a trap from supervisor mode */
	sd		x0, (FRAME_IS_USERMODE)(sp)

	PUSH_WITH_CFI(ra, FRAME_RA)
	PUSH_WITH_CFI(sp, FRAME_SP)
	PUSH_WITH_CFI(gp, FRAME_GP)
	PUSH_WITH_CFI(tp, FRAME_TP)
	PUSH_WITH_CFI(t0, FRAME_T0)
	PUSH_WITH_CFI(t1, FRAME_T1)
	PUSH_WITH_CFI(t2, FRAME_T2)
	PUSH_WITH_CFI(s0, FRAME_S0)
	PUSH_WITH_CFI(s1, FRAME_S1)
	PUSH_WITH_CFI(a0, FRAME_A0)
	PUSH_WITH_CFI(a1, FRAME_A1)
	PUSH_WITH_CFI(a2, FRAME_A2)
	PUSH_WITH_CFI(a3, FRAME_A3)
	PUSH_WITH_CFI(a4, FRAME_A4)
	PUSH_WITH_CFI(a5, FRAME_A5)
	PUSH_WITH_CFI(a6, FRAME_A6)
	PUSH_WITH_CFI(a7, FRAME_A7)
	PUSH_WITH_CFI(s2, FRAME_S2)
	PUSH_WITH_CFI(s3, FRAME_S3)
	PUSH_WITH_CFI(s4, FRAME_S4)
	PUSH_WITH_CFI(s5, FRAME_S5)
	PUSH_WITH_CFI(s6, FRAME_S6)
	PUSH_WITH_CFI(s7, FRAME_S7)
	PUSH_WITH_CFI(s8, FRAME_S8)
	PUSH_WITH_CFI(s9, FRAME_S9)
	PUSH_WITH_CFI(s10, FRAME_S10)
	PUSH_WITH_CFI(s11, FRAME_S11)
	PUSH_WITH_CFI(t3, FRAME_T3)
	PUSH_WITH_CFI(t4, FRAME_T4)
	PUSH_WITH_CFI(t5, FRAME_T5)
	sd		t6, FRAME_T6(sp)

	csrr	t0, sepc
	csrr	t1, sstatus
	csrr	t2, scause
	csrr	t3, stval

	sd		t0, FRAME_SEPC(sp)
	sd		t1, FRAME_SSTATUS(sp)
	sd		t2, FRAME_SCAUSE(sp)
	sd		t3, FRAME_STVAL(sp)

	.cfi_return_column t0
	.cfi_rel_offset t0, 31*8

	mv		a0, sp
	call	c_trap

	ld		t0, FRAME_SEPC(sp)
	ld		t1, FRAME_SSTATUS(sp)

	csrw	sepc, t0
	csrw	sstatus, t1

	ld		ra, FRAME_RA(sp)
	ld		sp, FRAME_SP(sp)
	ld		gp, FRAME_GP(sp)
	ld		tp, FRAME_TP(sp)
	ld		t0, FRAME_T0(sp)
	ld		t1, FRAME_T1(sp)
	ld		t2, FRAME_T2(sp)
	ld		s0, FRAME_S0(sp)
	ld		s1, FRAME_S1(sp)
	ld		a0, FRAME_A0(sp)
	ld		a1, FRAME_A1(sp)
	ld		a2, FRAME_A2(sp)
	ld		a3, FRAME_A3(sp)
	ld		a4, FRAME_A4(sp)
	ld		a5, FRAME_A5(sp)
	ld		a6, FRAME_A6(sp)
	ld		a7, FRAME_A7(sp)
	ld		s2, FRAME_S2(sp)
	ld		s3, FRAME_S3(sp)
	ld		s4, FRAME_S4(sp)
	ld		s5, FRAME_S5(sp)
	ld		s6, FRAME_S6(sp)
	ld		s7, FRAME_S7(sp)
	ld		s8, FRAME_S8(sp)
	ld		s9, FRAME_S9(sp)
	ld		s10, FRAME_S10(sp)
	ld		s11, FRAME_S11(sp)
	ld		t3, FRAME_T3(sp)
	ld		t4, FRAME_T4(sp)
	ld		t5, FRAME_T5(sp)
	ld		t6, FRAME_T6(sp)

	addi	sp, sp, FRAME_SIZE

	sret

	.cfi_endproc

#define CTX_RA 0*8
#define CTX_S0 1*8
#define CTX_S1 2*8
#define CTX_S2 3*8
#define CTX_S3 4*8
#define CTX_S4 5*8
#define CTX_S5 6*8
#define CTX_S6 7*8
#define CTX_S7 8*8
#define CTX_S8 9*8
#define CTX_S9 10*8
#define CTX_S10 11*8
#define CTX_S11 12*8
#define CTX_SIZE 13*8

.globl asm_swtch
/* void asm_swtch(void **oldsp, void *newsp) */
asm_swtch:
	addi sp, sp, -CTX_SIZE
	sd	ra, CTX_RA(sp)
	sd	s0, CTX_S0(sp)
	sd	s1, CTX_S1(sp)
	sd	s2, CTX_S2(sp)
	sd	s3, CTX_S3(sp)
	sd	s4, CTX_S4(sp)
	sd	s5, CTX_S5(sp)
	sd	s6, CTX_S6(sp)
	sd	s7, CTX_S7(sp)
	sd	s8, CTX_S8(sp)
	sd	s9, CTX_S9(sp)
	sd	s10, CTX_S10(sp)
	sd	s11, CTX_S11(sp)
	sd	sp, (a0) # save sp into oldsp

	mv	sp, a1 # restore sp from newsp
	ld	ra, CTX_RA(sp)
	ld	s0, CTX_S0(sp)
	ld	s1, CTX_S1(sp)
	ld	s2, CTX_S2(sp)
	ld	s3, CTX_S3(sp)
	ld	s4, CTX_S4(sp)
	ld	s5, CTX_S5(sp)
	ld	s6, CTX_S6(sp)
	ld	s7, CTX_S7(sp)
	ld	s8, CTX_S8(sp)
	ld	s9, CTX_S9(sp)
	ld	s10, CTX_S10(sp)
	ld	s11, CTX_S11(sp)

	addi sp, sp, CTX_SIZE
	ret

.globl asm_thread_trampoline
asm_thread_trampoline:
	mv a0, s0
	mv a1, s1
	call c_thread_trampoline
