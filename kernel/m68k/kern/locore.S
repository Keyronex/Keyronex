.cfi_sections .eh_frame, .debug_frame

.global asm_trap
asm_trap:
	.cfi_startproc
	.cfi_signal_frame

	ori.w #0x0700, %sr

	clr.w -(%sp) 	/* add padding */

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

	move.l %usp, %a0
	move.l %a0, -(%sp)
	.cfi_adjust_cfa_offset 4

	/* %sp seems to need to be 4-byte aligned to not make varargs explode */
	move.l %sp, %d0
	btst #1, %d0
	bne	 .Lcall_misaligned

.Lcall_aligned:
	move.l %sp, %a0
	move.l %a0, -(%sp)
	.cfi_adjust_cfa_offset 4

	jsr c_trap

	addq.l #4, %sp
	.cfi_adjust_cfa_offset -4
	bra .Lafter_call

.Lcall_misaligned:
	.cfi_remember_state

	subq.l #2, %sp
	.cfi_adjust_cfa_offset 2

	lea 2(%sp), %a0
	move.l %a0, -(%sp)
	.cfi_adjust_cfa_offset 4

	jsr c_trap

	addq.l #4, %sp
	.cfi_adjust_cfa_offset -4
	addq.l #2, %sp
	.cfi_adjust_cfa_offset -2

	.cfi_restore_state
	bra .Lafter_call

.Lafter_call:
	move.l (%sp)+, %a0
	move.l %a0, %usp
	.cfi_adjust_cfa_offset -4

	movem.l (%sp)+, %d0-%d7/%a0-%a6
	.cfi_adjust_cfa_offset -60

	addq.l #2, %sp	/* drop padding */
	rte
	.cfi_endproc


/*
 * return address goes into %d0, old PCB in %a0, new PCB in %a1
 * PCB+44 = return address
 * PCB+48 = stack pointer
 * PCB+52 = %sr
 */
.global kep_m68k_asm_switch
kep_m68k_asm_switch: /* (void)(pcb_t *old, pcb_t *new) */
	move.l %sp@+, %d0
	move.l %sp@, %a0
	move.l %sp@(4), %a1
	movem.l  %d2-%d7/%a2-%a6, %a0@
	move.l %d0, %a0@(44)
	move.l %sp, %a0@(48)
	move.w %sr, %a0@(52)
	move.w %a1@(52), %sr
	move.l %a1@(48), %sp
	move.l %a1@(44), %sp@
	movem.l %a1@, %d2-%d7/%a2-%a6
	rts

.extern kep_m68k_thread_trampoline

.global kep_m68k_asm_thread_trampoline
kep_m68k_asm_thread_trampoline: /* (void)(void(*)(void *), void *) */
	jsr kep_m68k_thread_trampoline

	/* restore as in asm_trap */

	add	#4, %sp
	move.l	%sp@+, %a0
	move.l	%a0, %usp
	movem.l	%sp@+, %d0-%d7/%a0-%a6
	addq.l	#2, %sp
	rte

.section .note.GNU-stack,"",%progbits
