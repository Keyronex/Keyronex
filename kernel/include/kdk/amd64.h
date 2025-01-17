#ifndef KRX_KDK_AMD64_H
#define KRX_KDK_AMD64_H

#include <stdint.h>
#include <stdbool.h>

#include <kdk/queue.h>
#include <kdk/amd64/cpulocal.h>

#define KRX_BIG_ENDIAN 0
#define KRX_LITTLE_ENDIAN 1

#define SMP 1
#define ENDIAN KRX_LITTLE_ENDIAN
#define BITS 64
#define KERN_HZ 1000
#define KSTACK_SIZE 32768

#define PGSIZE 0x1000
#define PGSIZE_L2 0x200000

#define LOWER_HALF 0x10000
#define LOWER_HALF_SIZE (0x0000800000000000 - LOWER_HALF)
#define HIGHER_HALF 0xffff800000000000
#define HHDM_BASE 0xffff800000000000
#define HHDM_SIZE 0x10000000000
#define KVM_DYNAMIC_BASE 0xffff810000000000
#define KVM_DYNAMIC_SIZE 0x10000000000
#define KVM_WIRED_BASE 0xffff820000000000
#define KVM_WIRED_SIZE 0x10000000000
#define KVM_UBC_BASE 0xffff830000000000
#define KVM_UBC_SIZE 0x10000000000
#define MISC_BASE 0xffff840000000000
#define PFNDB_BASE 0xfffff00000000000 /* 512tib - 16 tib */
#define KERN_BASE 0xffffffff80000000

#define PADDR_TO_PFN(PADDR) ((uintptr_t)(PADDR) >> 12)
#define PFN_TO_PADDR(PFN) ((uintptr_t)(PFN) << 12)

#define P2V(addr) ((((vaddr_t)(addr)) + HHDM_BASE))
#define V2P(addr) ((((paddr_t)(addr)) - HHDM_BASE))

/* This stuff belongs in inttypes.h, but let's define it temporarily... */
#define PRIu32 "u"
#define PRIb32 "b"
#define PRId64 "ld"
#define PRIu64 "lu"
#define PRIb64 "lb"
#define PRIx64 "lx"

typedef enum ipl {
	kIPL0 = 0,
	kIPLAST = 1,
	kIPLDPC = 2,
	kIPLDevice = 13,
	kIPLHigh = 15,
} ipl_t;

typedef struct __attribute__((packed)) md_intr_frame {
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
	uint64_t rbp, rbx, r12, r13, r14, r15, rdi, rsi, rsp;
	uint64_t align;
	uint8_t fpu[512];
} md_pcb_t;

typedef struct ktrap_recovery_frame {
	uint64_t rbx;
	uint64_t rbp;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t rsp;
	uint64_t rip;
} ktrap_recovery_frame_t;

typedef struct md_cpucb {
	/*! local APIC id */
	uint64_t lapic_id;
	/*! lapic timer ticks per second (divider 1) */
	uint64_t lapic_tps;
	/*! lapic base address */
	uintptr_t lapic_base;
	/*! task state segment */
	struct tss *tss;
} md_cpucb_t;

static inline __attribute__((noreturn)) void
hcf(void)
{
	for (;;)
		asm("hlt");
}

int md_intr_alloc(const char *name, ipl_t prio, intr_handler_t handler,
    void *arg, bool shareable, uint8_t *vector, struct intr_entry *entry);
void md_intr_register(const char *name, uint8_t vec, ipl_t prio,
    intr_handler_t handler, void *arg, bool shareable,
    struct intr_entry *entry);

extern struct kcpu bootstrap_cpu;

#endif /* KRX_KDK_AMD64_H */
