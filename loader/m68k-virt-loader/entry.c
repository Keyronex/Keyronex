#define NANOPRINTF_IMPLEMENTATION
#include "../../vendor/include/kdk/nanoprintf.h"
/* (must come first) */

#include <stdbool.h>
#include <stdint.h>

#include "../../platform/m68k-virt/handover.h"
#include "../../kernel/vm/m68k/mmu.h"
#include "../../vendor/include/kdk/elf.h"

#define printf(...) npf_pprintf(gftty_putc, NULL, __VA_ARGS__)
#define fatal(...){			\
	printf(__VA_ARGS__);		\
	for (;;)			\
		asm("stop #0x2700");	\
}

#define assert(...) if (!(__VA_ARGS__)) \
	fatal("Assertion failed: %s\n", #__VA_ARGS__)

#define PGSIZE 4096
#define HHDM_BASE 0x80000000

#define ROUNDUP(addr, align) (((addr) + align - 1) & ~(align - 1))
#define ROUNDDOWN(addr, align) ((((uintptr_t)addr)) & ~(align - 1))
#define PGROUNDUP(addr) ROUNDUP(addr, PGSIZE)
#define PGROUNDDOWN(addr) ROUNDDOWN(addr, PGSIZE)

extern uint8_t _bss_end;
size_t memory = 0;
size_t bump_base, bump_original;
void *kernel_base = 0;
size_t kernel_size = 0;
pml3e_t *pml3;

struct bootinfo_item {
	uint16_t tag;
	uint16_t size;
	uint32_t data[0];
};

enum gftty_reg {
	GFTTY_PUT_CHAR = 0x00,
	GFTTY_CMD = 0x08,
};

enum gftty_cmd {
	GFTTY_CMD_INT_DISABLE = 0x00,
};

volatile char *gftty_regs = (void *)0xff008000;

static void
gftty_write(enum gftty_reg reg, uint32_t val)
{
	*((uint32_t *)&gftty_regs[reg]) = val;
}

void
gftty_init(void)
{
	gftty_write(GFTTY_CMD, GFTTY_CMD_INT_DISABLE);
}

void
gftty_putc(int c, void *ctx)
{
	if (c == '\n')
		gftty_write(GFTTY_PUT_CHAR, '\r');
	gftty_write(GFTTY_PUT_CHAR, c);
}

static void
parse_bootinfo(void)
{
	uint8_t *ptr = &_bss_end;

	for (;;) {
		struct bootinfo_item *item = (struct bootinfo_item *)ptr;

		if (item->tag == 0)
			break;

		switch (item->tag) {
		case 0x5: /* BI_MEMCHUNK */
			assert(memory == 0);
			memory = item->data[1];
			printf("%zu.%zuMiB main memory.\n",
			    memory / 1024 / 1024, memory / 1024 % 1024);
			memory = PGROUNDDOWN(memory);
			break;

		case 0x6: /* BI_RAMDISK */
			printf("kernel at 0x%x (size 0x%x)\n", item->data[0],
			    item->data[1]);
			kernel_base = (void *)item->data[0];
			kernel_size = item->data[1];
			break;

		default:
#if 0
			printf("unknown bootinfo tag %hx\n", item->tag);
#endif
			break;
		}

		ptr += item->size;
	}
	bump_base = ROUNDUP((uintptr_t)ptr, PGSIZE);
	bump_original = bump_base;
}

void *
memcpy(void *restrict dstv, const void *restrict srcv, size_t len)
{
	unsigned char *dst = (unsigned char *)dstv;
	const unsigned char *src = (const unsigned char *)srcv;
	for (size_t i = 0; i < len; i++)
		dst[i] = src[i];
	return dst;
}

void *
memset(void *b, int c, size_t len)
{
	char *ss = b;
	while (len-- > 0)
		*ss++ = c;
	return b;
}

int
memcmp(const void *str1, const void *str2, size_t count)
{
	register const unsigned char *c1, *c2;

	c1 = (const unsigned char *)str1;
	c2 = (const unsigned char *)str2;

	while (count-- > 0) {
		if (*c1++ != *c2++)
			return c1[-1] < c2[-1] ? -1 : 1;
	}
	return 0;
}

static void *
aligned_alloc(size_t size, size_t alignment)
{
	void *ret;
	bump_base = ROUNDUP(bump_base, alignment);
	ret = (void *)bump_base;
	bump_base += size;
	memset(ret, 0x0, size);
	return ret;
}

static void
map_page(uint32_t virt, uint32_t phys, bool writeable)
{
	union {
		struct {
			uint32_t l3i : 7, l2i : 7, l1i : 6, pgi : 12;
		};
		uint32_t addr;
	} addr;

	addr.addr = (uint32_t)virt;
	// printf("Mapping virt 0x%lx to phys 0x%lx: l3i %u, l2i %u, l1i %u, pgi
	// %u\n", virt, phys, addr.l3i, addr.l2i, addr.l1i, addr.pgi);

	if (pml3[addr.l3i].addr == 0) {
		uint32_t pml2 = (uint32_t)aligned_alloc(sizeof(pml2_t), 512);
		pml3[addr.l3i].addr = pml2 >> 4;
		pml3[addr.l3i].used = 0;
		pml3[addr.l3i].writeprotect = 0;
		pml3[addr.l3i].type = 3;
	}
	pml2e_t *pml2 = (void *)(pml3[addr.l3i].addr << 4);

	if (pml2[addr.l2i].addr == 0) {
		uint32_t pml1 = (uint32_t)aligned_alloc(sizeof(pml1_t), 512);
		pml2[addr.l2i].addr = pml1 >> 4;
		pml2[addr.l2i].used = 0;
		pml2[addr.l2i].writeprotect = 0;
		pml2[addr.l2i].type = 3;
	}
	pte_hw_t *pml1 = (void *)(pml2[addr.l2i].addr << 4);
	pte_hw_t *pml1e = &pml1[addr.l1i];

	pml1e->pfn = (uintptr_t)phys >> 12;
	pml1e->cachemode = 1; /* cacheable, copyback */
	pml1e->supervisor = 1;
	pml1e->type = 3;
	pml1e->global = 1;
	pml1e->writeprotect = writeable ? 0 : 1;
}

static inline void
load_crp(uintptr_t pml3)
{
	asm volatile("movec %0,%%srp" : : "r"(pml3));
	asm volatile("movec %0,%%urp" : : "r"(pml3));
}

static void
enable_mmu(uintptr_t pml3)
{
	uint32_t dtt = 0;
	uint32_t tcr = 1 << 15;

	/* set up transparent translation of lower-half */
#define MASKBITS(BITS) ((1 << BITS) - 1)
#define TTR_BASE 24 /* 8 bits base address */
#define TTR_MASK 16 /* mask, 8 bits */
#define TTR_EN 15   /* enable, 1 bit */
#define TTR_S 13    /* 2 bits, set to 3 for ignore supervisor/user mode */
#define TTR_CM 5    /* cache mode, 3 = noncacheable */
	dtt |= (unsigned)0 << TTR_BASE; /* want high bit to be zero */
	dtt |= MASKBITS(7) << TTR_MASK; /* ignore all but high bit */
	dtt |= (unsigned)1 << TTR_EN;
	dtt |= (unsigned)3 << TTR_S;
	dtt |= (unsigned)2 << TTR_CM;

	uint32_t nott = 0;
	asm(".chip 68060\n movec %0, %%dtt1" ::"d"(nott));
	asm("movec %0, %%itt1" ::"d"(nott));
	asm("movec %0, %%dtt0" ::"d"(dtt));
	asm("movec %0, %%itt0" ::"d"(dtt));
	load_crp(pml3);
	asm("movec %0, %%tcr" ::"d"(tcr));

#if 0
	mmusr_query(pml3, (void*)0x1000);
	mmusr_query(pml3, (void *)0x80001000);
	mmusr_query(pml3, (void *)0x80000000);
	mmusr_query(pml3, (void *)0x80030000);
	mmusr_query(pml3, 0x15938 + 0x80000000);
#endif
}

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#if 0
void *
pmap_trans(void *virt)
{
	union {
		struct {
			uint32_t l3i : 7, l2i : 7, l1i : 6, pgi : 12;
		};
		uint32_t addr;
	} addr;

	addr.addr = (uint32_t)virt;

	printf("L3i %d, l2i %d, l1i %d, pgi %d\n", addr.l3i, addr.l2i,
	    addr.l1i);

	pml2e_t *pml2 = (void *)(pml3[addr.l3i].addr << 4);
	pml1e_t *pml1 = (void *)(pml2[addr.l2i].addr << 4);
	pml1e_t *pml1e = &pml1[addr.l1i];
	return (void *)(pml1e->pfn << 12);
}

static void
mmusr_query(uintptr_t addr)
{
	asm volatile("ptestw (%0)" : : "a"(addr));
	unsigned long mmusr;
	asm volatile("movec %%mmusr,%0" : "=d"(mmusr));

	union {
		struct {
			uint32_t phys : 20, buserr : 1, global : 1, u1 : 1,
			    u0 : 1, supervisor : 1, cachemode : 2, modified : 1,
			    o : 1, writeprotect : 1, transparent : 1,
			    resident : 1;
		};
		uint32_t val;
	} stuff;

	stuff.val = mmusr;

	printf("mmusr enquiry of virtual address 0x%lx:"
	       "phys=0x%x/buserr:%d/transparent:%d\n",
	    addr, stuff.phys << 12, stuff.buserr, stuff.transparent);
}
#endif

static void
load()
{
	Elf32_Ehdr *ehdr = kernel_base;

	if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0) {
		fatal("bad elf header\n");
	} else if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
		fatal("bad class\n");
	} else if (ehdr->e_type != ET_EXEC) {
		fatal("not a dso: type %hu\n", ehdr->e_type);
	}

	/* let us first conclude the total size */
	for (int i = 0; i < ehdr->e_phnum; i++) {
		Elf32_Phdr *phdr = kernel_base + ehdr->e_phoff +
		    ehdr->e_phentsize * i;

		if (phdr->p_type != PT_LOAD) {
			continue;
		}

		size_t virt_base = phdr->p_vaddr,
		       virt_size = PGROUNDUP(phdr->p_memsz);
		assert(virt_base % PGSIZE == 0);

		for (size_t i = virt_base; i < virt_base + virt_size;
		     i += PGSIZE) {
			void *phys_page = aligned_alloc(PGSIZE, PGSIZE);
			map_page(i, (uint32_t)phys_page, true);
		}

		memcpy((void *)phdr->p_vaddr, kernel_base + phdr->p_offset,
		    phdr->p_filesz);
	}

	void (*start)(struct handover *) = (void *)ehdr->e_entry;
	void *fb = aligned_alloc(1024 * 768 * 4, PGSIZE);
	struct handover *handover = aligned_alloc(sizeof(*handover),
	    sizeof(*handover));
	printf("Jumping to kernel...\n\n\n");
	handover->bootinfo = &_bss_end;
	handover->bumped_start = bump_original;
	handover->bumped_end = PGROUNDUP(bump_base);
	handover->fb_base = (uintptr_t)fb;
	start(handover);
}


void
cstart(void)
{
	printf("Keyronex-lite/virt68k Loader: " __DATE__ " " __TIME__ "\n");

	parse_bootinfo();
	assert(kernel_base != 0);
	assert(memory != 0);
	assert(kernel_size != 0);
	assert(bump_base != 0);
	/* we can only comfortably fit this with the dierct map*/
	assert(memory < 0x40000000);

	pml3 = aligned_alloc(sizeof(pml3_t), 512);

	/* direct map main memory*/
	for (size_t i = 0; i < memory; i += PGSIZE)
		map_page(HHDM_BASE + i, i, true);

	/* direct map goldfish/virtio/virt-ctrl space */
	for (size_t i = 0; i <= (0xfffff000 - 0xff000000) / PGSIZE; i++)
		map_page(0xff000000 + i * PGSIZE, 0xff000000 + i * PGSIZE,
		    true);

	printf("enable MMU\n");
	enable_mmu((uintptr_t)pml3);

	printf("load\n");
	load();
	printf(" returned from load...\n");
}
