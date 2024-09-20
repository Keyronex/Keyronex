#ifndef KRX_KDK_M68K_H
#define KRX_KDK_M68K_H

#include <stdbool.h>
#include <stdint.h>

#include <kdk/m68k/cpulocal.h>

#define KRX_BIG_ENDIAN 0
#define KRX_LITTLE_ENDIAN 1

#define SMP 0
#define KRX_ENDIAN KRX_BIG_ENDIAN
#define BITS 32
#define KERN_HZ 64
#define KSTACK_SIZE 32768

#define PGSIZE 4096

#define LOWER_HALF 0x1000
#define LOWER_HALF_SIZE (0x80000000 - LOWER_HALF)
#define HIGHER_HALF 0x80000000
#define HHDM_BASE 0x80000000
#define HHDM_SIZE 0x40000000
#define KVM_WIRED_BASE 0xc0000000
#define KVM_WIRED_SIZE 0x10000000
#define KVM_UBC_BASE 0xd0000000
#define KVM_UBC_SIZE 0x10000000
#define KVM_DYNAMIC_BASE 0xe0000000
#define KVM_DYNAMIC_SIZE 0x10000000
#define KVM_PAGED_BASE 0xf0000000
#define KVM_PAGED_SIZE 0xc000000
#define PFNDB_BASE 0xfc000000
#define KERN_BASE 0xfd000000

#define PADDR_TO_PFN(PADDR) ((uintptr_t)(PADDR) >> 12)
#define PFN_TO_PADDR(PFN) ((uintptr_t)(PFN) << 12)

#define P2V(addr) ((((vaddr_t)(addr)) + HHDM_BASE))
#define V2P(addr) ((((paddr_t)(addr)) - HHDM_BASE))

/* This stuff belongs in inttypes.h, but let's define it temporarily... */
#define PRIu32 "u"
#define PRIb32 "b"
#define PRId64 "lld"
#define PRIu64 "llu"
#define PRIb64 "llb"
#define PRIx64 "llx"

typedef enum ipl {
	kIPL0 = 0,
	kIPLAST = 1,
	kIPLDPC = 2,
	kIPLDevice = 12,
	kIPLHigh = 15,
} ipl_t;

typedef struct m68k_context {
	uint32_t d2, d3, d4, d5, d6, d7;
	uint32_t a2, a3, a4, a5, a6;
	uint32_t pc, sp;
	uint16_t sr;
} m68k_context_t;

struct __attribute__((packed)) ssw_68040 {
	uint16_t cp : 1;
	uint16_t cu : 1;
	uint16_t ct : 1;
	uint16_t cm : 1;
	uint16_t ma : 1;
	uint16_t atc : 1;
	uint16_t lk : 1;
	uint16_t rw : 1;
	uint16_t x : 1;
	uint16_t size : 2;
	uint16_t tt : 2;
	uint16_t tm : 3;
};

struct __attribute__((packed)) fslw_68060 {
	uint32_t fixme : 32;
};

typedef struct __attribute__((packed)) md_intr_frame {
	/* Pushed by ourselves. */
	uint32_t usp;
	uint32_t d0, d1, d2, d3, d4, d5, d6, d7;
	uint32_t a0, a1, a2, a3, a4, a5, a6;
	uint16_t padding;

	/* Pushed by the CPU */
	uint16_t sr;
	uint32_t pc;
	uint16_t format : 4;
	uint16_t vector_offset : 12;

	/* Variable parts follow. */
	union {
		/*! 68060 bus error */
		struct __attribute__((packed)) format_4 {
			uint32_t fa;
			struct fslw_68060 fslw;
		} format_4;
		/*! 68040 bus error */
		struct __attribute__((packed)) format_7 {
			/*! effective address*/
			uint32_t ea;
			/*! special status word */
			struct ssw_68040 ssw;
			/*! write-back status */
			uint16_t wb3s, wb2s, wb1s;
			/*! fault address */
			uint32_t fa;
			/* write-back data/addresses (wb1wd can = pd0) */
			uint32_t wb3a, wb3d, wb2a, fb2d, wb1a, wb1d;
			/*! push data lw */
			uint32_t pd1, pd2, pd3;
		} format_7;
	};
} md_intr_frame_t;

typedef struct md_pcb {
	m68k_context_t genregs;
} md_pcb_t;

typedef struct md_cpucb {
	int dpc_int;
	ipl_t ipl;
} md_cpucb_t;

typedef bool (*intr_handler_t)(md_intr_frame_t *frame, void *arg);

/*! A private structure. */
struct intr_entry {
	const char *name;
	ipl_t ipl;
	void *arg;
	bool shareable;
};

static inline __attribute__((noreturn)) void
hcf(void)
{
	for (;;)
		asm("stop #0x2700");
}

extern struct kcpu bootstrap_cpu;
extern struct kthread **threads;

#define curthread() bootstrap_cpu.curthread

#endif /* KRX_KDK_M68K_H */
