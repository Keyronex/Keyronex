#include <kern/kmem.h>
#include <kern/ksrv.h>
#include <libkern/libkern.h>

#include <string.h>

#define ELFMAG "\177ELF"

struct kmod_head kmods = TAILQ_HEAD_INITIALIZER(kmods);

unsigned char ELF64_ST_BIND(unsigned char info) {
	return info >> 4;
}

static uint32_t
elf64_hash(const char *name)
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

static const Elf64_Sym *
elf64_hashlookup(const Elf64_Sym *symtab, const char *strtab,
    const uint32_t *hashtab, const char *symname)
{
	const uint32_t hash = elf64_hash(symname);
	const uint32_t nbucket = hashtab[0];

	for (uint32_t i = hashtab[2 + hash % nbucket]; i;
	     i = hashtab[2 + nbucket + i]) {
		if (strcmp(symname, strtab + symtab[i].st_name) == 0)
			return &symtab[i];
	}

	return NULL;
}

/*
 * Lookup a symbol, searching each kmod to check whether it contains it.
 *
 * assumes: no duplicate names
 */
static const void *
kmod_lookupsym(const char *name)
{
	kmod_t *kmod;

	TAILQ_FOREACH (kmod, &kmods, entries) {
		const Elf64_Sym *sym = NULL;
		int		 bind;

		if (kmod->hashtab)
			sym = elf64_hashlookup(kmod->symtab, kmod->strtab,
			    kmod->hashtab, name);
		else if (kmod->symtab)
			for (int i = 0; i < kmod->symtab_size; i++) {
				const char *cand = kmod->strtab +
				    kmod->symtab[i].st_name;
				if (strcmp(name, cand) == 0) {
					sym = &kmod->symtab[i];
					break;
				}
			}

		if (!sym)
			continue;

		bind = ELF64_ST_BIND(sym->st_info);
		if (bind != STB_GLOBAL && bind != STB_WEAK &&
		    bind != STB_GNU_UNIQUE) {
			kprintf("binding for %s is not global/weak/unique",
			    name);
			continue;
		}

		if (kmod->hashtab) /* meaning is a shared library */
			return (void *)(kmod->base + sym->st_value);
		else
			return (void *)
			    sym->st_value; /* kernel addrs are absolute */
	}

	return NULL;
}

int
ksrv_backtrace(vaddr_t vaddr, const char **name, size_t *offset)
{
	const char *cand_name;
	vaddr_t	    cand_addr = 0;
	size_t	    cand_offs;
	kmod_t	   *kmod;

	TAILQ_FOREACH (kmod, &kmods, entries) {
		for (int i = 0; i < kmod->symtab_size; i++) {
			const char *iName = kmod->strtab +
			    kmod->symtab[i].st_name;
			vaddr_t iAddr = (vaddr_t)kmod->symtab[i].st_value;

			if (iAddr <= vaddr && iAddr > cand_addr) {
				cand_name = iName;
				cand_addr = iAddr;
				cand_offs = vaddr - iAddr;
			}
		}
	}

	if (cand_addr == 0)
		return -1;

	*name = cand_name;
	*offset = cand_offs;
	return 0;
}

void
ksrv_parsekern(void *addr)
{
	Elf64_Ehdr *ehdr = addr;
	kmod_t	   *kmod = kmem_alloc(sizeof *kmod);

	kprintf("reading kernel: addr %p...\n", addr);

	TAILQ_INIT(&kmods);
	TAILQ_INSERT_HEAD(&kmods, kmod, entries);

	for (int x = 0; x < ehdr->e_shentsize * ehdr->e_shnum;
	     x += ehdr->e_shentsize) {
		Elf64_Shdr *shdr = (addr + ehdr->e_shoff + x);
		if (shdr->sh_type == SHT_SYMTAB) {
			Elf64_Shdr *strtabshdr;

			kassert(shdr->sh_entsize == sizeof(Elf64_Sym));

			kmod->symtab = addr + shdr->sh_offset;
			kmod->symtab_size = shdr->sh_size / sizeof(Elf64_Sym);

			strtabshdr = addr + ehdr->e_shoff +
			    ehdr->e_shentsize * shdr->sh_link;
			kmod->strtab = addr + strtabshdr->sh_offset;

			break;
		}
	}

	kassert(kmod->symtab != NULL);
	kmod->hashtab = NULL;
}
