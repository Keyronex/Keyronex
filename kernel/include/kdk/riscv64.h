#ifndef KRX_KDK_RISCV64_H
#define KRX_KDK_RISCV64_H

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

#define PGSIZE 4096
#define LOWER_HALF 0x10000
#define LOWER_HALF_SIZE (0x0000800000000000 - LOWER_HALF)
#define HIGHER_HALF 0xffff000000000000
#define HHDM_BASE 0xffff000000000000
#define HHDM_SIZE 0x10000000000
#define KVM_DYNAMIC_BASE 0xffff110000000000
#define KVM_DYNAMIC_SIZE 0x10000000000
#define KVM_WIRED_BASE 0xffff120000000000
#define KVM_WIRED_SIZE 0x10000000000
#define KVM_UBC_BASE 0xffff130000000000
#define KVM_UBC_SIZE 0x10000000000
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

typedef struct riscv64_context {

} riscv64_context_t;

typedef struct __attribute__((packed)) md_intr_frame {

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
	riscv64_context_t genregs;
	uint64_t fp[66];
} md_pcb_t;

typedef struct md_cpucb {
	int dpc_int;
	ipl_t ipl;
	uint32_t hartid;
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
    asm volatile("mv %0, tp" : "=r"(thread));
    return thread;
}

void md_intr_register(const char *name, uint32_t gsi, ipl_t prio,
    intr_handler_t handler, void *arg, bool shareable,
    struct intr_entry *entry);

extern struct kthread thread0;
extern struct kcpu bootstrap_cpu;

#endif /* KRX_KDK_RISCV64_H */
