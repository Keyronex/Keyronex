.global asm_swtch
asm_swtch: /* (void)(tcb_t *old, tcb_t *new) */
	move.l %sp@+,%d0	| pop return address to d0
	move.l %sp@,%a0		| old tcb in %a0
	move.l %sp@(4),%a1	| new tcb in %a1
	movem.l  %d2-%d7/%a2-%a6, %a0@ | save callee-saved registers
	move.l %d0, %a0@(44) | save old saved-%pc
	move.l %sp, %a0@(48) | save old %sp
	move.w %sr, %a0@(52) | save old %sr
	move.w %a0@(52), %sr | load new %sr
	move.l %a1@(48), %sp | load new %sp
	move.l %a1@(44), %sp@ | load new saved-%pc
	movem.l %a1@, %d2-%d7/%a2-%a6 | load new callee-saved registers
	rts | finally return into new context

.extern thread_trampoline

.global asm_thread_trampoline
asm_thread_trampoline: /* (void)(void(*)(void *), void *) */
	jsr thread_trampoline | call thread_trampoline

	# drop the sp we pushed
	add	 #4, %sp

	# restore the usp we pushed
	move.l %sp@+, %a0
	move.l %a0, %usp

	# restore the general regs we pushed
	movem.l  %sp@+, %d0-%d7/%a0-%a6

	# removed the alignment
	add #2, %sp

	rte

.section .note.GNU-stack,"",%progbits
