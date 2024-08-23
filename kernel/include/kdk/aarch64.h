#ifndef KRX_KDK_AARCH64_H
#define KRX_KDK_AARCH64_H

#include <stdbool.h>
#include <stdint.h>

#include "kdk/queue.h"

#define KRX_BIG_ENDIAN 0
#define KRX_LITTLE_ENDIAN 1

#define SMP 1
#define ENDIAN KRX_LITTLE_ENDIAN
#define BITS 64
#define KERN_HZ 64
#define KSTACK_SIZE 32768

#define PGSIZE 0x1000
#define PGSIZE_L2 0x200000

#define LOWER_HALF 0x10000
#define LOWER_HALF_SIZE (0x0000800000000000 - LOWER_HALF)
#define HIGHER_HALF 0xffff000000000000
#define HHDM_BASE 0xffff000000000000
#define HHDM_SIZE 0x10000000000
#define MISC_BASE 0xffff010000000000
#define MISC_SIZE 0x10000000000
#define KVM_DYNAMIC_BASE 0xffff110000000000
#define KVM_DYNAMIC_SIZE 0x10000000000
#define KVM_WIRED_BASE 0xffff120000000000
#define KVM_WIRED_SIZE 0x10000000000
#define KVM_UBC_BASE 0xffff130000000000
#define KVM_UBC_SIZE 0x10000000000
#define PFNDB_BASE 0xfffff00000000000 /* 512tib - 16 tib */
#define KERN_BASE 0xffffffff80000000

#define PADDR_TO_PFN(PADDR) ((uintptr_t)(PADDR) >> 12)
#define PFN_TO_PADDR(PFN) ((uintptr_t)(PFN) << 12)

#define P2V(addr) ((((vaddr_t)(addr)) + HHDM_BASE))
#define V2P(addr) ((((paddr_t)(addr)) - HHDM_BASE))

/* This stuff belongs in inttypes.h, but let's define it temporarily... */
#define PRIu32 "u"
#define PRIb32 "b"
#define PRIu64 "lu"
#define PRIb64 "lb"
#define PRIx64 "lx"

typedef enum ipl {
	kIPL0 = 0,
	kIPLAST = 1,
	kIPLDPC = 2,
	kIPLDevice = 14, /* most external IRQs */
	kIPLHigh = 15, /* hardclock, invlpg IPI */
} ipl_t;

typedef struct aarch64_context {
	uint64_t x29, x30, x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, sp;
} aarch64_context_t;

typedef struct __attribute__((packed)) md_intr_frame {
	uint64_t x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,
	    x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,
	    x27, x28, x29, x30;
	uint64_t elr;
	uint64_t spsr;
	uint64_t esr;
	uint64_t far;
	uint64_t sp_el0;
	uint64_t tpidr_el0;
} md_intr_frame_t;

typedef bool (*intr_handler_t)(md_intr_frame_t *frame, void *arg);

/*! A private structure. */
struct intr_entry {
	TAILQ_ENTRY(intr_entry) queue_entry;
	const char *name;
	ipl_t ipl;
	intr_handler_t handler;
	void *arg;
	bool shareable;
};

typedef struct md_pcb {
	aarch64_context_t genregs;
	uint64_t fp[66];
} md_pcb_t;

typedef struct md_cpucb {
	int dpc_int;
	ipl_t ipl;
	ipl_t hard_ipl;
	uint32_t mpidr;
	uint32_t gic_interface_number;
	uintptr_t gicc_base;
} md_cpucb_t;

static inline __attribute__((noreturn)) void
hcf(void)
{
	for (;;)
		asm("wfi");
}

static inline struct kthread *
curthread(void)
{
	struct kthread *thread;
	asm volatile("mrs %0, tpidr_el1" : "=r"(thread));
	return thread;
}

void md_intr_register(const char *name, uint32_t gsi, ipl_t prio,
    intr_handler_t handler, void *arg, bool shareable,
    struct intr_entry *entry);

extern struct kthread thread0;
extern struct kcpu bootstrap_cpu;

#endif /* KRX_KDK_AARCH64_H */
