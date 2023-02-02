/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2020-2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

#include <sys/param.h>

#include <assert.h>
#include <clib/exec_protos.h>
#include <elf.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmu.h"

#define ROUNDUP(addr, align) ((((uintptr_t)addr) + align - 1) & ~(align - 1))
#define PGSIZE 0x1000

#define err(...)                     \
	{                            \
		perror(__VA_ARGS__); \
		exit(EXIT_FAILURE);  \
	}

typedef struct image {
	/*! physiacl base */
	void *base;
	/*! virtual base = base + 0x80000000 */
	void  *vbase;
	size_t mem_size; /* total size of virt address space */

	Elf32_Dyn *dyn;

	/*
	 * Elf32_Word nbuckets;
	 * Elf32_Word nchain;
	 * Elf32_Word bucket[nbucket];
	 * Elf32_Word chain[nchain];
	 */
	const Elf32_Word *hashtab;

	void (**init_array)(void);
	size_t init_array_size;

	char *strtab;

	const Elf32_Sym *symtab;
	size_t		 symtab_size;
} image_t;

image_t image;

static uint32_t
elf32_hash(const char *name)
{
	uint32_t h = 0, g;
	for (; *name; name++) {
		h = (h << 4) + (uint8_t)*name;
		g = h & 0xf0000000;
		if (g)
			h ^= g >> 24;
		h &= 0x0FFFFFFF;
	}
	return h;
}

static const Elf32_Sym *
elf32_hashlookup(const Elf32_Sym *symtab, const char *strtab,
    const uint32_t *hashtab, const char *symname)
{
	const uint32_t hash = elf32_hash(symname);
	const uint32_t nbucket = hashtab[0];

	for (uint32_t i = hashtab[2 + hash % nbucket]; i;
	     i = hashtab[2 + nbucket + i]) {
		if (strcmp(symname, strtab + symtab[i].st_name) == 0)
			return &symtab[i];
	}

	return NULL;
}

static const void *
image_lookupsym(image_t *image, const char *name)
{
	const Elf32_Sym *sym = NULL;
	int		 bind;

	if (image->hashtab)
		sym = elf32_hashlookup(image->symtab, image->strtab,
		    image->hashtab, name);
	else if (image->symtab)
		for (int i = 0; i < image->symtab_size; i++) {
			const char *cand = image->strtab +
			    image->symtab[i].st_name;
			if (strcmp(name, cand) == 0) {
				sym = &image->symtab[i];
				break;
			}
		}

	if (!sym)
		return NULL;

	bind = ELF32_ST_BIND(sym->st_info);
	if (bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_GNU_UNIQUE) {
		printf("binding for %s is not global/weak/unique", name);
		return NULL;
	}

	if (image->hashtab) /* meaning is a shared library */
		return image->vbase + sym->st_value;
	else
		return (void *)sym->st_value; /* kernel addrs are absolute */
}

/* return true if this reloc type doesn't need a symbol resolved */
bool
reloc_need_resolution(int type)
{
	switch (type) {
	case R_68K_RELATIVE:
		return false;

	default:
		return true;
	}
}

static int
do_reloc(image_t *image, const Elf32_Rela *reloc)
{
	uint32_t	*dest = (uint32_t *)(image->base + reloc->r_offset);
	unsigned int	 symn = ELF32_R_SYM(reloc->r_info);
	const Elf32_Sym *sym = &image->symtab[symn];
	const char	*symname = image->strtab + sym->st_name;
	const void	*symv = 0x0; /* symbol virtual address */
	int		 type = ELF32_R_TYPE(reloc->r_info);

	printf("sym %s: relocation type %u/symidx %hu: ", symname,
	    ELF32_R_TYPE(reloc->r_info), sym->st_shndx);

	if (reloc_need_resolution(type) && sym->st_shndx == SHN_UNDEF) {
		symv = image_lookupsym(image, symname);
		if (!symv) {
			printf("missing symbol %s, quitting\n", symname);
			return -1;
		} else {
			printf("resolved to %p\n", symv);
		}
	} else {
		symv = image->vbase + sym->st_value;
		printf("0x%x -> %p\n", sym->st_value, symv);
	}

	switch (type) {
	case R_68K_32:
		*dest = (uint32_t)symv + reloc->r_addend;
		break;

	case R_68K_GLOB_DAT:
	case R_68K_JMP_SLOT:
		*dest = (uint32_t)symv;
		break;

	case R_68K_RELATIVE:
		*dest = (uint32_t)image->vbase + reloc->r_addend;
		break;

	default:
		printf("Unsupported reloc %u\n", ELF32_R_TYPE(reloc->r_info));
		return -1;
	}

	return 0;
}

void *
image_load(void *addr)
{
	Elf32_Ehdr ehdr;
	void	  *mod_init = NULL;

	memcpy(&ehdr, addr, sizeof(Elf32_Ehdr));

	if (memcmp(ehdr.e_ident, ELFMAG, 4) != 0) {
		printf("bad elf header\n");
		return NULL;
	} else if (ehdr.e_ident[EI_CLASS] != ELFCLASS32) {
		printf("bad class\n");
		return NULL;
	} else if (ehdr.e_type != ET_DYN) {
		printf("not a dso: type %hu\n", ehdr.e_type);
		return NULL;
	}

	/* let us first conclude the total size */
	for (int i = 0; i < ehdr.e_phnum; i++) {
		Elf32_Phdr *phdr = addr + ehdr.e_phoff + ehdr.e_phentsize * i;
		image.mem_size = MAX(phdr->p_vaddr + phdr->p_memsz,
		    image.mem_size);
	}

	image.base = AllocMem(image.mem_size + (PGSIZE * 2), MEMF_FAST);
	if (image.base == NULL)
		err("failed to allocate memory for kernel");

	image.base = (char *)ROUNDUP(image.base, PGSIZE);
	image.vbase = image.base + 0x80000000;

	for (int i = 0; i < ehdr.e_phnum; i++) {
		Elf32_Phdr phdr;

		memcpy(&phdr, addr + ehdr.e_phoff + ehdr.e_phentsize * i,
		    sizeof(Elf32_Phdr));

		printf("phdr: type %u memsz %u 0x%x\n", phdr.p_type,
		    phdr.p_memsz, phdr.p_vaddr);

		if (phdr.p_type == PT_LOAD) {
			memset(image.base + phdr.p_vaddr, 0, phdr.p_memsz);
			memcpy(image.base + phdr.p_vaddr, addr + phdr.p_offset,
			    phdr.p_filesz);
		} else if (phdr.p_type == PT_DYNAMIC)
			image.dyn = (Elf32_Dyn *)(image.base + phdr.p_vaddr);
		else if (phdr.p_type == PT_NOTE ||
		    phdr.p_type == PT_GNU_EH_FRAME ||
		    phdr.p_type == PT_GNU_STACK || phdr.p_type == PT_GNU_RELRO)
			/* epsilon */;
		else
			printf("...unrecognised type, ignoring\n");
	}

	for (size_t i = 0; image.dyn[i].d_tag != DT_NULL; i++) {
		Elf32_Dyn *ent = &image.dyn[i];
		switch (ent->d_tag) {
		case DT_STRTAB:
			image.strtab = image.base + ent->d_un.d_ptr;
			break;

		case DT_SYMTAB:
			image.symtab = (const Elf32_Sym *)(image.base +
			    ent->d_un.d_ptr);
			break;

		case DT_HASH:
			image.hashtab = (const Elf32_Word *)(image.base +
			    ent->d_un.d_ptr);
			image.symtab_size = image.hashtab[1];
			break;

		/* ignore the rest */
		default:
			break;
		}
	}

	for (int i = 0; i < image.symtab_size; i++) {
		const char *symname = image.strtab + image.symtab[i].st_name;
		if (strcmp(symname, "modinit") == 0)
			mod_init = (void *)(image.symtab[i].st_value +
			    image.vbase);
	}

	for (int x = 0; x < ehdr.e_shentsize * ehdr.e_shnum;
	     x += ehdr.e_shentsize) {
		Elf32_Shdr shdr;
		memcpy(&shdr, addr + ehdr.e_shoff + x, ehdr.e_shentsize);

		if (shdr.sh_type == SHT_RELA) {
			const Elf32_Rela *reloc = (Elf32_Rela *)(image.base +
			    shdr.sh_addr);
			const Elf32_Rela *lim = (Elf32_Rela *)(image.base +
			    shdr.sh_addr + shdr.sh_size);

			for (; reloc < lim; reloc++)
				if (do_reloc(&image, reloc) == -1)
					return NULL;
		}
	}

	return (void *)(mod_init);
}

/*!
 * we identity map 256mib.
 * accordingly, we need:
 * one pml3;
 * 8 pml2s;
 * and 1024 pml1s (one for every pml2's slot)
 */
#define NPML2 8
#define NPML1 NPML2 * 128
#define BS_PAGETABLES_TOTAL \
	(sizeof(pml3_t) + sizeof(pml2_t) * NPML2 + sizeof(pml1_t) * NPML1)

#define TO_PML3_ADDR(ADDR) addr

pml3e_t *bspml3;

bool
verify(uintptr_t pml3, uintptr_t ptr)
{
	if (ptr > pml3 && ptr < (uintptr_t)pml3 + BS_PAGETABLES_TOTAL)
		return true;
	return false;
}

void
pmap_enter(pml3e_t *pml3, void *virt, void *phys)
{
	union {
		struct {
			uint32_t l3i : 7, l2i : 7, l1i : 6, pgi : 12;
		};
		uint32_t addr;
	} addr;

	addr.addr = (uint32_t)virt;

	pml2e_t *pml2 = (void *)(pml3[addr.l3i].addr << 4);
	if (!verify((uintptr_t)pml3, (uintptr_t)pml2)) {
		printf("PML2 bad: is %p\n", pml2);
		exit(1);
	}
	pml1e_t *pml1 = (void *)(pml2[addr.l2i].addr << 4);
	if (!verify((uintptr_t)pml3, (uintptr_t)pml1)) {
		printf("PML1 bad: is %p\n", pml1);
		exit(1);
	}
	pml1e_t *pml1e = &pml1[addr.l1i];
	if (!verify((uintptr_t)pml3, (uintptr_t)pml1e)) {
		printf("PML1E bad: is %p\n", pml1e);
		exit(1);
	}

	// printf("VIRT %p TO PHYS %p\n", virt, phys);
	// printf("l3i %u l2i %u l1i %u pgi %u\n", addr.l3i, addr .l2i,
	// addr.l1i, addr.pgi); printf("pml2 = %p pml1 = %p pml1e = %p;\n",
	// pml2, pml1, pml1e);

	pml1e->pfn = (uintptr_t)phys >> 12;
	/* disable cache */
	pml1e->cachemode = 3;
	pml1e->supervisor = true;
	pml1e->writeable = false;
	pml1e->type = 3;
}

void *
pmap_trans(pml3e_t *pml3, void *virt)
{
	union {
		struct {
			uint32_t l3i : 7, l2i : 7, l1i : 6, pgi : 12;
		};
		uint32_t addr;
	} addr;

	addr.addr = (uint32_t)virt;

	pml2e_t *pml2 = (void *)(pml3[addr.l3i].addr << 4);
	if (!verify((uintptr_t)pml3, (uintptr_t)pml2)) {
		printf("PML2 bad: is %p\n", pml2);
		exit(1);
	}
	pml1e_t *pml1 = (void *)(pml2[addr.l2i].addr << 4);
	if (!verify((uintptr_t)pml3, (uintptr_t)pml1)) {
		printf("PML1 bad: is %p\n", pml1);
		exit(1);
	}
	pml1e_t *pml1e = &pml1[addr.l1i];
	if (!verify((uintptr_t)pml3, (uintptr_t)pml1e)) {
		printf("PML1E bad: is %p\n", pml1e);
		exit(1);
	}
	return (void *)(pml1e->pfn << 12);
}

static inline void
load_crp(uintptr_t pml3)
{
	asm volatile("movec %0,srp" : : "r"(pml3));
	asm volatile("movec %0,urp" : : "r"(pml3));
}

#if 0
static void
mmusr_query(uintptr_t pml3, void *addr)
{
	void *sstack = SuperState();
	asm volatile("ptestw (%0)" : : "a"(addr));
	unsigned long mmusr;
	asm volatile("movec %%mmusr,%0" : "=d"(mmusr));
	UserState(sstack);

	union {
		struct {
			uint32_t phys : 20, b : 1, g : 1, u1 : 1, u0 : 1, s : 1,
			    cm : 2, m : 1, o : 1, w : 1, t : 1, r : 1;
		};
		uint32_t val;
	} stuff;

	stuff.val = mmusr;

	printf("MMUSR: PHYS %d/B:%d/T:%d\n", stuff.phys, stuff.b, stuff.t);
	if (addr >= 0x80000000)
		printf("MANUAL TRANS: %p\n", pmap_trans(pml3, addr));
}
#endif

static void
enable_mmu(uintptr_t pml3)
{
	void	*sstack;
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

	sstack = SuperState();
	uint32_t nott = 0;
	asm("movec %0, %%dtt1" ::"d"(nott));
	asm("movec %0, %%itt1" ::"d"(nott));
	asm("movec %0, %%dtt0" ::"d"(dtt));
	asm("movec %0, %%itt0" ::"d"(dtt));
	load_crp(pml3);
	asm("movec %0, %%tcr" ::"d"(tcr));
	UserState(sstack);

#if 0
	mmusr_query(pml3, (void*)0x1000);
	mmusr_query(pml3, (void *)0x80001000);
	mmusr_query(pml3, (void *)0x80000000);
	mmusr_query(pml3, (void *)0x80030000);
	mmusr_query(pml3, 0x15938 + 0x80000000);
#endif
}

static void
setup_pagetables(void)
{
	void *bstables = AllocMem(BS_PAGETABLES_TOTAL + PGSIZE * 2, MEMF_FAST);
	assert(bstables != NULL);
	bstables = (void *)ROUNDUP(bstables, PGSIZE);

	pml3e_t *pml3 = bstables;
	pml2_t	*pml2s = bstables + sizeof(pml3_t);
	pml1_t	*pml1s = (void *)(((char *)pml2s) + sizeof(pml2_t) * NPML2);

	printf("%u bytes for pagetables\n", BS_PAGETABLES_TOTAL);

	for (int l3 = 64; l3 < 64 + NPML2; l3++) {
		pml2e_t *pml2 = pml2s[l3 - 64];

		pml3[l3].writeable = false;
		pml3[l3].type = 3;
		pml3[l3].used = 0;
		pml3[l3].addr = (uintptr_t)pml2 >> 4;

		pml1_t *l2_pml1s = &pml1s[(l3 - 64) * 128];
		memset(l2_pml1s, 0x0, sizeof(pml1_t) * 128);

		for (int l2 = 0; l2 < 128; l2++) {
			pml2[l2].writeable = false;
			pml2[l2].type = 3;
			pml2[l2].used = 0;
			pml2[l2].addr = (uintptr_t)l2_pml1s[l2] >> 4;
		}
	}

	for (unsigned i = 0; i < (1024 * 1024 * 256); i += PGSIZE) {
		pmap_enter(pml3, (void *)(0x80000000 + i), (void *)i);
	}

	enable_mmu((uintptr_t)pml3);

	bspml3 = pml3;
}

int
main(int argc, char *argv[])
{
	FILE *file;
	void *fileaddr;
	void (*init)(void *, void *);

	file = fopen("vmscalux", "rb");
	if (file == NULL)
		err("failed to open vmscalux");

	fseek(file, 0, SEEK_END);
	long fsize = ftell(file);
	fseek(file, 0, SEEK_SET); /* same as rewind(f); */

	fileaddr = malloc(fsize);
	fread(fileaddr, fsize, 1, file);

	init = image_load(fileaddr);
	if (!init) {
		fprintf(stderr, "failed to find start function\n");
		return -1;
	}

	void *mem = AllocMem(80 * 34 * 15, MEMF_CHIP);
	void *mem2 = AllocMem(2 * 13 * 4, MEMF_CHIP);

	printf("CHIPMEM: %p/%p - NEED %u of PAGETABLES\n", mem, mem2,
	    BS_PAGETABLES_TOTAL);

	setup_pagetables();

	printf("loading Keyronex...\n");

	Forbid();
	Disable();
	SuperState();

	init(mem, mem2);
}
