#ifndef KRX_KDK_AARCH64_H
#define KRX_KDK_AARCH64_H

#include <stdint.h>

#define BIG_ENDIAN 0
#define LITTLE_ENDIAN 1

#define SMP 0
#define ENDIAN LITTLE_ENDIAN
#define BITS 64
#define KERN_HZ 64
#define KSTACK_SIZE 8192

#define PGSIZE 4096
#define HHDM_BASE 0xffff000000000000
#define HHDM_SIZE 0x10000000000
#define KVM_DYNAMIC_BASE 0xffff100000000000
#define KVM_DYNAMIC_SIZE 0x10000000000
#define KERN_BASE 0xffffffff80000000

#define PADDR_TO_PFN(PADDR) ((uintptr_t)(PADDR) >> 12)
#define PFN_TO_PADDR(PFN) ((uintptr_t)(PFN) << 12)

#define P2V(addr) ((((char *)(addr)) + HHDM_BASE))
#define V2P(addr) ((((char *)(addr)) - HHDM_BASE))

/* This stuff belongs in inttypes.h, but let's define it temporarily... */
#define PRIu32 "u"
#define PRIb32 "b"

typedef enum ipl {
	kIPL0,
	kIPLDPC,
	kIPLHigh,
} ipl_t;

typedef struct aarch64_context {
	uint64_t x29, x30, x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, sp;
} aarch64_context_t;

typedef struct __attribute__((packed)) md_intr_frame {
	uint64_t x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13,
	    x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26,
	    x27, x28, x29, x30;
	void *elr_el1;
} md_intr_frame_t;

typedef bool (*intr_handler_t)(md_intr_frame_t *frame, void *arg);

/*! A private structure. */
struct intr_entry {
	const char *name;
	ipl_t ipl;
	void *arg;
	bool shareable;
};

typedef struct md_pcb {
	aarch64_context_t genregs;
} md_pcb_t;

typedef struct md_cpucb {
	ipl_t ipl;
} md_cpucb_t;

static inline void
hcf(void)
{
	for (;;)
		asm("wfi");
}

extern struct kcpu bootstrap_cpu;

#define curcpu() (&bootstrap_cpu)

#endif /* KRX_KDK_AARCH64_H */
