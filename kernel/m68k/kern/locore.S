.cfi_sections .eh_frame, .debug_frame

.global asm_trap
asm_trap:
	.cfi_startproc
	.cfi_signal_frame

	clr.w   -(%sp) 	/* add padding */

	.cfi_def_cfa %sp, 0	/* CFA anchored at karch_trapframe::padding */
	.cfi_offset  %pc, 4

	movem.l %d0-%d7/%a0-%a6, -(%sp)
	.cfi_adjust_cfa_offset 60
	.cfi_offset %d0, -60
	.cfi_offset %d1, -56
	.cfi_offset %d2, -52
	.cfi_offset %d3, -48
	.cfi_offset %d4, -44
	.cfi_offset %d5, -40
	.cfi_offset %d6, -36
	.cfi_offset %d7, -32
	.cfi_offset %a0, -28
	.cfi_offset %a1, -24
	.cfi_offset %a2, -20
	.cfi_offset %a3, -16
	.cfi_offset %a4, -12
	.cfi_offset %a5, -8
	.cfi_offset %a6, -4

	move.l  %usp, %a0
	move.l  %a0, -(%sp)
	.cfi_adjust_cfa_offset 4

	/* %sp seems to needs to be 4-byte aligned to not make varargs explode */
	move.l  %sp, %d0
	btst	#1, %d0
	bne	 .Lcall_misaligned

.Lcall_aligned:
	move.l  %sp, %a0
	move.l  %a0, -(%sp)
	.cfi_adjust_cfa_offset 4

	jsr	 c_trap

	addq.l  #4, %sp
	.cfi_adjust_cfa_offset -4
	bra	 .Lafter_call

.Lcall_misaligned:
	.cfi_remember_state

	subq.l  #2, %sp
	.cfi_adjust_cfa_offset 2

	lea	 2(%sp), %a0
	move.l  %a0, -(%sp)
	.cfi_adjust_cfa_offset 4

	jsr	 c_trap

	addq.l  #4, %sp
	.cfi_adjust_cfa_offset -4
	addq.l  #2, %sp
	.cfi_adjust_cfa_offset -2

	.cfi_restore_state
	bra	 .Lafter_call

.Lafter_call:
	move.l  (%sp)+, %a0
	move.l  %a0, %usp
	.cfi_adjust_cfa_offset -4

	movem.l (%sp)+, %d0-%d7/%a0-%a6
	.cfi_adjust_cfa_offset -60

	addq.l  #2, %sp	/* drop padding */
	rte
	.cfi_endproc


.section .note.GNU-stack,"",%progbits
