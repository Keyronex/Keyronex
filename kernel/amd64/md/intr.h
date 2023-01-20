#ifndef MD_INTR_H_
#define MD_INTR_H_

#include <md/spl.h>

#include <stdbool.h>
#include <stdint.h>

struct kthread;

/*!
 * interrupt frame, also the saved state of a thread
 */
typedef struct md_intr_frame {
	uint64_t rax;
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t rbp;
	uint64_t code; /* may be fake */

	uintptr_t rip;
	uint64_t  cs;
	uint64_t  rflags;
	uintptr_t rsp;
	uint64_t  ss;
} __attribute__((packed)) md_intr_frame_t;

typedef struct md_cpu {
	uint64_t    lapic_id;
	uint64_t    lapic_tps; /* lapic timer ticks per second (divider 1) */
	uintptr_t   lapic_base;
	struct tss *tss;
	ipl_t	    switchipl;
	struct kthread *old, *new;
} md_cpu_t;

typedef void (*intr_handler_fn_t)(md_intr_frame_t *frame, void *arg);

static inline struct kcpu *
curcpu()
{
	struct kcpu *val;
	asm volatile("mov %%gs:0, %0" : "=r"(val));
	return val;
}

static inline bool
md_intr_disable()
{
	uintptr_t flags;
	asm volatile("pushf\n\t"
		     "pop %0\n\t"
		     "cli"
		     : "=rm"(flags)
		     : /* epsilon */
		     : "memory", "cc");
	return flags & (1 << 9);
}

static inline void
md_intr_x(bool en)
{
	if (en)
		asm volatile("pushf\n\t"
			     "pop %%r10\n\t"
			     "bts $9, %%r10\n\t"
			     "push %%r10\n\t"
			     "popf"
			     : /* epsilon */
			     : /* epsilon */
			     : "r10", "memory");
}

/*! allocate and register a suitable interrupt vector, returns vector number */
uint8_t md_intr_alloc(ipl_t prio, intr_handler_fn_t handler, void *arg);
/*! register a handler for a given interrupt vector at a given priority */
void md_intr_register(int vec, ipl_t prio, intr_handler_fn_t handler,
    void *arg);
/*! trace an interrupt frame with nk_dbg */
void md_intr_frame_trace(md_intr_frame_t *frame);

/* send an invlpg IPI to a particular CPU */
void md_ipi_invlpg(struct kcpu *cpu);
/* send a reschedule IPI  to a particular CPU */
void md_ipi_reschedule(struct kcpu *cpu);

/*! set the per-cpu timer */
void md_timer_set(struct kcpu *cpu, uint64_t nanos);
/*! get remaining nanosecs on per-cpu timer */
uint64_t md_timer_get_remaining(struct kcpu *cpu);

/* initialise a thread's state */
void md_thread_init(struct kthread *thread, void (*start_fun)(void *),
    void			   *start_arg);
void md_switch(ipl_t switchipl, struct kthread *from, struct kthread *to);

#endif /* INTR_H_ */
