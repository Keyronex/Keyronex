.globl asm_swtch
asm_swtch:
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
