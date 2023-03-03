#
# ISR thunks
#

#define ASM

#include "asmintr.h"

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
.global isr_thunk_\number
isr_thunk_\number:
	cli
.if \kind != ._intWithCode
	# need to fill in the space in the interrupt frame struct
	push $0
.endif

	SWAPGS_IF_NEEDED

	push %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %r11
	push %r10
	push %r9
	push %r8
	push %rsi
	push %rdi
	push %rdx
	push %rcx
	push %rbx
	push %rax

	mov %rsp, %rdi
	mov $\number, %rsi
	call handle_int

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
.endm

MAKE_ISR_THUNK ._intNoCode, 4
MAKE_ISR_THUNK ._intNoCode, 6
MAKE_ISR_THUNK ._intNoCode, 8
MAKE_ISR_THUNK ._intWithCode, 10
MAKE_ISR_THUNK ._intWithCode, 11
MAKE_ISR_THUNK ._intWithCode, 12
MAKE_ISR_THUNK ._intWithCode, 13
MAKE_ISR_THUNK ._intWithCode, 14
MAKE_ISR_THUNK ._intNoCode, 128 # posix syscalls

#define INT_ISR_THUNK(VAL) MAKE_ISR_THUNK ._intNoCode, VAL ;
NORMAL_INTS(INT_ISR_THUNK)


.global swtch
swtch:
	mov %rdi, %rsp

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

.section .note.GNU-stack,"",%progbits
