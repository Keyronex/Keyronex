.macro MAKE_HANDLER asm_label, c_func
	.align 7
	\asm_label:
		.cfi_startproc simple
		.cfi_signal_frame
		sub sp, sp, #288
		.cfi_def_cfa %sp, 0

		stp x0, x1, [sp, #8 * 0]
		.cfi_offset x0, 0
		.cfi_offset x1, 8
		stp x2, x3, [sp, #8 * 2]
		.cfi_offset x2, 16
		.cfi_offset x3, 24
		stp x4, x5, [sp, #8 * 4]
		.cfi_offset x4, 32
		.cfi_offset x5, 40
		stp x6, x7, [sp, #8 * 6]
		.cfi_offset x6, 48
		.cfi_offset x7, 56
		stp x8, x9, [sp, #8 * 8]
		.cfi_offset x8, 64
		.cfi_offset x9, 72
		stp x10, x11, [sp, #8 * 10]
		.cfi_offset x10, 80
		.cfi_offset x11, 88
		stp x12, x13, [sp, #8 * 12]
		.cfi_offset x12, 96
		.cfi_offset x13, 104
		stp x14, x15, [sp, #8 * 14]
		.cfi_offset x14, 112
		.cfi_offset x15, 120
		stp x16, x17, [sp, #8 * 16]
		.cfi_offset x16, 128
		.cfi_offset x17, 136
		stp x18, x19, [sp, #8 * 18]
		.cfi_offset x18, 144
		.cfi_offset x19, 152
		stp x20, x21, [sp, #8 * 20]
		.cfi_offset x20, 160
		.cfi_offset x21, 168
		stp x22, x23, [sp, #8 * 22]
		.cfi_offset x22, 176
		.cfi_offset x23, 184
		stp x24, x25, [sp, #8 * 24]
		.cfi_offset x24, 192
		.cfi_offset x25, 200
		stp x26, x27, [sp, #8 * 26]
		.cfi_offset x26, 208
		.cfi_offset x27, 216
		stp x28, x29, [sp, #8 * 28]
		.cfi_offset x28, 224
		.cfi_offset x29, 232
		str x30, [sp, #8 * 30]
		.cfi_offset x30, 240

		mrs x0, elr_el1
		str x0, [sp, #248]
#		.cfi_offset ???, 248

		mrs x0, spsr_el1
		str x0, [sp, #256]
#		.cfi_offset ???, 256

		mrs x0, esr_el1
		str x0, [sp, #264]

		mrs x0, far_el1
		str x0, [sp, #272]

		.cfi_val_offset sp, 288

		mov x0, sp
		bl \c_func

		ldp x0, x1, [sp, #8 * 0]
		.cfi_restore x0
		.cfi_restore x1
		ldp x2, x3, [sp, #8 * 2]
		.cfi_restore x2
		.cfi_restore x3
		ldp x4, x5, [sp, #8 * 4]
		.cfi_restore x4
		.cfi_restore x5
		ldp x6, x7, [sp, #8 * 6]
		.cfi_restore x6
		.cfi_restore x7
		ldp x8, x9, [sp, #8 * 7]
		.cfi_restore x8
		.cfi_restore x9
		ldp x10, x11, [sp, #8 * 10]
		.cfi_restore x10
		.cfi_restore x11
		ldp x12, x13, [sp, #8 * 12]
		.cfi_restore x12
		.cfi_restore x13
		ldp x14, x15, [sp, #8 * 14]
		.cfi_restore x14
		.cfi_restore x15
		ldp x16, x17, [sp, #8 * 16]
		.cfi_restore x16
		.cfi_restore x17
		ldp x18, x19, [sp, #8 * 18]
		.cfi_restore x18
		.cfi_restore x19
		ldp x20, x21, [sp, #8 * 20]
		.cfi_restore x20
		.cfi_restore x21
		ldp x22, x23, [sp, #8 * 22]
		.cfi_restore x22
		.cfi_restore x23
		ldp x24, x25, [sp, #8 * 24]
		.cfi_restore x24
		.cfi_restore x25
		ldp x26, x27, [sp, #8 * 26]
		.cfi_restore x26
		.cfi_restore x27
		ldp x28, x29, [sp, #8 * 28]
		.cfi_restore x28
		.cfi_restore x29
		ldr x30, [sp, #8 * 30]
		.cfi_restore x30

		add sp, sp, #288
		.cfi_restore sp

		eret
		.cfi_endproc
.endm

MAKE_HANDLER asm_el1t_sync, c_el1t_sync
MAKE_HANDLER asm_el1t_irq, c_el1t_irq
MAKE_HANDLER asm_el1t_fiq, c_el1t_fiq
MAKE_HANDLER asm_el1t_error, c_el1t_error

MAKE_HANDLER asm_el1h_sync, c_el1_sync
MAKE_HANDLER asm_el1h_intr, c_el1_intr
MAKE_HANDLER asm_el1h_fiq, c_el1_fiq
MAKE_HANDLER asm_el1h_error, c_el1_error

MAKE_HANDLER asm_el0_sync, c_el0_sync
MAKE_HANDLER asm_el0_intr, c_el0_intr
MAKE_HANDLER asm_el0_fiq, c_el0_fiq
MAKE_HANDLER asm_el0_error, c_el0_error

MAKE_HANDLER asm_el0_sync32, c_exception
MAKE_HANDLER asm_el0_intr32, c_exception
MAKE_HANDLER asm_el0_fiq32, c_exception
MAKE_HANDLER asm_el0_error32, c_exception

.macro	vector, handler
	.align 7
	b \handler
.endm

.align 11
.globl vectors
vectors:
	vector asm_el1t_sync
	vector asm_el1t_irq
	vector asm_el1t_fiq
	vector asm_el1t_error

	vector asm_el1h_sync
	vector asm_el1h_intr
	vector asm_el1h_fiq
	vector asm_el1h_error

	vector asm_el0_sync
	vector asm_el0_intr
	vector asm_el0_fiq
	vector asm_el0_error

	vector asm_el0_sync32
	vector asm_el0_intr32
	vector asm_el0_fiq32
	vector asm_el0_error32

.global asm_swtch
asm_swtch:
	stp x29, x30, [x0]
	stp x19, x20, [x0, #16]
	stp x21, x22, [x0, #32]
	stp x23, x24, [x0, #48]
	stp x25, x26, [x0, #64]
	stp x27, x28, [x0, #80]
	mov x2, sp
	str x2, [x0, #96]

	ldp x29, x30, [x1]
	ldp x19, x20, [x1, #16]
	ldp x21, x22, [x1, #32]
	ldp x23, x24, [x1, #48]
	ldp x25, x26, [x1, #64]
	ldp x27, x28, [x1, #80]
	ldr x2, [x1, #96]
	mov sp, x2

	ret

.global asm_thread_trampoline
asm_thread_trampoline:
	mov x0, x19
	mov x1, x20
	bl thread_trampoline
