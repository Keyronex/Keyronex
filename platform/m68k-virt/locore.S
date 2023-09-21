.cfi_sections .eh_frame, .debug_frame

.global _start

_start:
	move.l %sp, %a0
	lea stack_top,%sp
	move.l 4(%a0), -(%sp)
	jsr cstart
loop:
	bra loop

.global asm_exception
asm_exception:
	.cfi_startproc
	.cfi_signal_frame

	# align so we can get cfa_offset
	move.w  #123, -(%sp)

	.cfi_def_cfa %sp, 0
	.cfi_offset %pc, 4

	movem.l  %d0-%d7/%a0-%a6, %sp@-
	.cfi_adjust_cfa_offset 60

	# push user stack pointer
	move.l   %usp, %a0
	move.l   %a0, %sp@-
	.cfi_adjust_cfa_offset 4

	# finally push the stack pointer so c_exception receives it as arg
	move.l   %sp, %sp@-
	.cfi_adjust_cfa_offset 4

	jsr	 c_exception

	# drop the sp we pushed
	add	 #4, %sp
	.cfi_adjust_cfa_offset -4

	# drop the usp we pushed
	add	 #4, %sp
	.cfi_adjust_cfa_offset -4

	# restore the general regs we pushed
	movem.l  %sp@+, %d0-%d7/%a0-%a6
	.cfi_adjust_cfa_offset -60

	# removed the alignment
	add #2, %sp

	rte
	.cfi_endproc

.section .bootstrap_stack, "aw", @nobits
.align 64
stack_bottom:
.skip 65536
stack_top:
