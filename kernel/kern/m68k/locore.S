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

.section .note.GNU-stack,"",%progbits
