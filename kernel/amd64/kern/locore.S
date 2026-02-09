#include "asm_intr.h"

.set ._intWithCode, 1
.set ._intNoCode, 2

.macro SWAPGS_IF_NEEDED
	# check if the %cs pushed indicates CPL 3 - if so, swap GS
	testl $3, 16(%rsp)
	jz 1f
	swapgs

1:
.endm

.macro MAKE_ISR_THUNK kind, number
.global isr_\number
isr_\number:
	.cfi_startproc simple
	.cfi_signal_frame
	.cfi_def_cfa %rsp, 0
	.cfi_offset %rip, 0
	.cfi_offset %rsp, 24

	cli
.if \kind != ._intWithCode
	# need to fill in the space in the interrupt frame struct
	push $0
.endif
	.cfi_adjust_cfa_offset 8

	SWAPGS_IF_NEEDED

	push %rbp
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rbp, 0
	push %r15
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r15, 0
	push %r14
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r14, 0
	push %r13
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r13, 0
	push %r12
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r12, 0
	push %r11
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r11, 0
	push %r10
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r10, 0
	push %r9
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r9, 0
	push %r8
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %r8, 0
	push %rsi
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rsi, 0
	push %rdi
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rdi, 0
	push %rdx
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rdx, 0
	push %rcx
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rcx, 0
	push %rbx
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rbx, 0
	push %rax
	.cfi_adjust_cfa_offset 8
	.cfi_rel_offset %rax, 0

	mov %rsp, %rdi
	mov $\number, %rsi
	xor %ebp, %ebp
	call kep_amd64_interrupt

	pop %rax
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rax
	pop %rbx
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rbx
	pop %rcx
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rcx
	pop %rdx
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rdx
	pop %rdi
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rdi
	pop %rsi
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rsi
	pop %r8
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r8
	pop %r9
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r9
	pop %r10
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r10
	pop %r11
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r11
	pop %r12
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r12
	pop %r13
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r13
	pop %r14
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r14
	pop %r15
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r15
	pop %rbp
	.cfi_adjust_cfa_offset -8
	.cfi_restore %rbp

	cli # ensure no nonsense here
	SWAPGS_IF_NEEDED

	# drop the error code
	add $8, %rsp
	.cfi_adjust_cfa_offset -8
	iretq
	.cfi_endproc
.endm

MAKE_ISR_THUNK ._intNoCode, 0
MAKE_ISR_THUNK ._intNoCode, 1
MAKE_ISR_THUNK ._intNoCode, 2
MAKE_ISR_THUNK ._intNoCode, 3
MAKE_ISR_THUNK ._intNoCode, 4
MAKE_ISR_THUNK ._intNoCode, 5
MAKE_ISR_THUNK ._intNoCode, 6
MAKE_ISR_THUNK ._intNoCode, 7
MAKE_ISR_THUNK ._intWithCode, 8
MAKE_ISR_THUNK ._intNoCode, 9
MAKE_ISR_THUNK ._intWithCode, 10
MAKE_ISR_THUNK ._intWithCode, 11
MAKE_ISR_THUNK ._intWithCode, 12
MAKE_ISR_THUNK ._intWithCode, 13
MAKE_ISR_THUNK ._intWithCode, 14
MAKE_ISR_THUNK ._intNoCode, 15
MAKE_ISR_THUNK ._intNoCode, 16
MAKE_ISR_THUNK ._intWithCode, 17
MAKE_ISR_THUNK ._intNoCode, 18
MAKE_ISR_THUNK ._intNoCode, 19
MAKE_ISR_THUNK ._intNoCode, 20
MAKE_ISR_THUNK ._intWithCode, 21
MAKE_ISR_THUNK ._intNoCode, 22
MAKE_ISR_THUNK ._intNoCode, 23
MAKE_ISR_THUNK ._intNoCode, 24
MAKE_ISR_THUNK ._intNoCode, 25
MAKE_ISR_THUNK ._intNoCode, 26
MAKE_ISR_THUNK ._intNoCode, 27
MAKE_ISR_THUNK ._intNoCode, 28
MAKE_ISR_THUNK ._intWithCode, 29
MAKE_ISR_THUNK ._intWithCode, 30
MAKE_ISR_THUNK ._intNoCode, 31
MAKE_ISR_THUNK ._intNoCode, 128

#define INT_ISR_THUNK(VAL) MAKE_ISR_THUNK ._intNoCode, VAL ;
IDT_ENTRIES(INT_ISR_THUNK)

.global kep_amd64_asm_thread_trampoline
kep_amd64_asm_thread_trampoline:
	call kep_thread_trampoline

	# frame size is a multiple of 8 not 16 bytes, so ke_thread_init_context()
	# pushed an extra 8 bytes below the frame. drop that.
	pop %rax

	pop %rax
	pop %rbx
	pop %rcx
	pop %rdx
	pop %rdi
	pop %rsi
	pop %r8
	pop %r9
	pop %r10
	pop %r11
	pop %r12
	pop %r13
	pop %r14
	pop %r15
	pop %rbp

	cli # ensure no nonsense here
	SWAPGS_IF_NEEDED

	# drop the error code
	add $8, %rsp

	iretq

.globl kep_amd64_asm_switch
kep_amd64_asm_switch:
	movq %rbp, (%rdi)
	movq %rbx, 8(%rdi)
	movq %r12, 16(%rdi)
	movq %r13, 24(%rdi)
	movq %r14, 32(%rdi)
	movq %r15, 40(%rdi)
	movq %rdi, 48(%rdi)
	movq %rsi, 56(%rdi)
	movq %rsp, 64(%rdi)

	movq (%rsi), %rbp
	movq 8(%rsi), %rbx
	movq 16(%rsi), %r12
	movq 24(%rsi), %r13
	movq 32(%rsi), %r14
	movq 40(%rsi), %r15
	movq 48(%rsi), %rdi
	movq 64(%rsi), %rsp
	movq 56(%rsi), %rsi

	ret
