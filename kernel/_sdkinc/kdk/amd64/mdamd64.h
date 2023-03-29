/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Feb 13 2023.
 */

#ifndef KRX_HL_HL_H
#define KRX_HL_HL_H

#include <bsdqueue/queue.h>
#include <stdbool.h>
#include <stdint.h>
#include <limine.h>

#include "kdk/kerndefs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ipl {
	kIPL0 = 0,
	kIPLAPC = 1,
	kIPLDPC = 2,
	kIPLDevice = 13,
	kIPLClock = 14,
	kIPLHigh = 15,
} ipl_t;

/*!
 * interrupt frame contents
 */
typedef struct hl_intr_frame {
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
	uint64_t cs;
	uint64_t rflags;
	uintptr_t rsp;
	uint64_t ss;
} __attribute__((packed)) hl_intr_frame_t;

/*!
 * Type of an interrupt service routine. It should return true if it handled
 * the interrupt, and false otherwise.
 */
typedef bool (*intr_handler_t)(hl_intr_frame_t *frame, void *arg);

struct hl_thread {
	/*! FSBase */
	uintptr_t fs;
	/*! FXSAVE-saved FPU context */
	char fpu_context[512];
};

struct hl_cpu {
	/*! local APIC id */
	uint64_t lapic_id;
	/*! lapic timer ticks per second (divider 1) */
	uint64_t lapic_tps;
	/*! lapic base address */
	uintptr_t lapic_base;
	/*! task state segment */
	struct tss *tss;

	/*! old thread for context switch */
	struct kthread *oldthread;
	/*! new thread for context switch */
	struct kthread *newthread;
};

/*! A private structure. */
struct intr_entry {
	TAILQ_ENTRY(intr_entry) queue_entry;
	const char *name;
	ipl_t ipl;
	intr_handler_t handler;
	void *arg;
	bool shareable;
};

/*! @brief Kernel upcall made when IPL is lowered. */
void ki_ipl_lowered(ipl_t from, ipl_t to);

/*! @brief Get pointer to this CPU's kcpu structure. */
static inline struct kcpu *
hl_curcpu()
{
	struct kcpu *val;
	asm volatile("mov %%gs:0, %0" : "=r"(val));
	return val;
}

/*!
 * @brief Allocates a vector and registers an interrupt handler for it.
 *
 * This function allocates an interrupt vector suitable for specified priority
 * and vector shareability, and registers it with a handler function and
 * argument.
 *
 * It can fail if there is no suitable vector available, e.g. because all
 * vectors for that priority level have already been taken non-shareable.
 *
 * @param name The name of this interrupt handler.
 * @param prio The priority at which the interrupt should run.
 * @param handler The function to be called when the interrupt occurs.
 * @param arg An optional argument that will be passed to the handler function.
 * @param shareable Whether the interrupt vector is shareable or not.
 * @param[out] vector An optional pointer to an `uint8_t` where the allocated
 * interrupt vector will be stored.
 * @param entry An interrupt entry structure allocated by the caller.
 *
 * @return 0 if the allocation was successful, 1 otherwise.
 */
int md_intr_alloc(const char *name, ipl_t prio, intr_handler_t handler,
    void *arg, bool shareable, uint8_t *vector, struct intr_entry *entry);

/*!
 * @brief Registers an interrupt handler for a specified vector.
 *
 * @param name The name of this interrupt handler.
 * @param vec The interrupt vector to which the handler should be attached.
 * @param prio The priority of the interrupt.
 * @param handler The function to be called when the interrupt occurs.
 * @param arg An optional argument that will be passed to the handler function.
 * @param entry An interrupt entry structure allocated by the caller.
 *
 * @note This function does not perform any validation on the arguments, so it
 * is the responsibility of the caller to ensure that the specified interrupt
 * vector and priority are suitable.
 */
void md_intr_register(const char *name, uint8_t vec, ipl_t prio,
    intr_handler_t handler, void *arg, bool shareable,
    struct intr_entry *entry);

/*!
 * @brief Print a stack trace for an interrupt frame using ke_dprintf.
 */
void md_intr_frame_trace(hl_intr_frame_t *frame);

/*!
 * @brief Disable interrupts and return previous interrupt state.
 */
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

/*!
 * @brief Enable or disable interrupts.
 */
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

/*! @brief Start the periodic timer at KERN_HZ. */
void hl_clock_start();

/*! @brief Low-level context switch. */
void hl_switch(struct kthread *from, struct kthread *to);

/*! @brief Send a reschedule IPI to a CPU. */
void hl_ipi_reschedule(struct kcpu *cpu);

/*! @brief Send an invlpg IPI to a CPU. */
void hl_ipi_invlpg(struct kcpu *cpu);

/*!
 * @brief Get thec current interrupt priority level.
 */
static inline ipl_t
splget()
{
	uint64_t ipl;
	asm volatile("mov %%cr8, %0\n" : "=r"(ipl));
	return (ipl_t)ipl;
}

/*!
 * @brief Change interrupt priority level and return the old IPL.
 */
static inline ipl_t
splx(ipl_t ipl)
{
	uint64_t spli = (ipl_t)ipl;
	uint64_t oldipl = (ipl_t)splget();
	asm volatile("movq %0,%%cr8" ::"r"(spli) : "memory");
	if (oldipl > (uint64_t)ipl) {
		ki_ipl_lowered((ipl_t)oldipl, ipl);
	}
	return (ipl_t)oldipl;
}

/*!
 * @brief Raise interrupt priority level.
 */
ipl_t splraise(ipl_t ipl);

#define splapc() splraise(kIPLAPC)
#define spldpc() splraise(kIPLDPC)

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_module_request module_request;
extern volatile struct limine_rsdp_request rsdp_request;
extern volatile struct limine_terminal_request terminal_request;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_HL_HL_H */
