#include <sys/auxv.h>

#include <elf.h>
#include <errno.h>

#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "vm/vmp.h"

struct exec_package {
	vaddr_t stack;	  /* bottom of stack */
	vaddr_t sp;	  /* initial stack pointer to execute with */
	vaddr_t entry;	  /* entry IP */
	vaddr_t phaddr;	  /* address of phdr */
	size_t phentsize; /* size of a phdr */
	size_t phnum;	  /* count of phdrs */
};

#if BITS == 32
typedef Elf32_Ehdr Elf_Ehdr;
typedef Elf32_Phdr Elf_Phdr;
#elif BITS == 64
typedef Elf64_Ehdr Elf_Ehdr;
typedef Elf64_Phdr Elf_Phdr;
#endif

extern vm_procstate_t kernel_procstate;

int
load_elf(vnode_t *vnode, vaddr_t base, struct exec_package *pkg)
{
	int r;
	Elf_Ehdr ehdr;
	Elf_Phdr *phdrs;

	r = ubc_io(vnode, (vaddr_t)&ehdr, 0, sizeof(ehdr), false);
	if (r < 0)
		kfatal("ubc_io: %d\n", r);

	if (memcmp(ehdr.e_ident, ELFMAG, 4) != 0)
		kfatal("load_elf: bad e_ident\n");

	phdrs = kmem_alloc(ehdr.e_phnum * ehdr.e_phentsize);
	if (!phdrs)
		return -ENOMEM;

	r = ubc_io(vnode, (vaddr_t)phdrs, ehdr.e_phoff,
	    ehdr.e_phnum * ehdr.e_phentsize, false);
	if (r < 0)
		return r;

	pkg->entry = base + ehdr.e_entry;
	pkg->phentsize = ehdr.e_phentsize;
	pkg->phnum = ehdr.e_phnum;
	pkg->phaddr = 0x0;

	for (int i = 0; i < ehdr.e_phnum; i++) {
		Elf_Phdr *phdr = &phdrs[i];
		vaddr_t vaddr, vaddr_based;
		io_off_t file_offset_aligned;
		size_t mapped_length, mapped_length_aligned, full_length;

		if (phdr->p_type == PT_PHDR) {
			pkg->phaddr = base + phdr->p_vaddr;
			continue;
		} else if (phdr->p_type != PT_LOAD)
			continue;

		vaddr = PGROUNDDOWN(phdr->p_vaddr);
		vaddr_based = vaddr + base;
		file_offset_aligned = PGROUNDDOWN(phdr->p_offset);
		mapped_length = phdr->p_vaddr + phdr->p_filesz - vaddr;
		mapped_length_aligned = PGROUNDUP(mapped_length);
		full_length = PGROUNDUP(phdr->p_vaddr + phdr->p_memsz) - vaddr;

		r = vm_ps_map_object_view(&kernel_procstate, vnode->object,
		    &vaddr_based, mapped_length_aligned, file_offset_aligned,
		    kVMAll, kVMAll, false, true, true);
		kassert(r == 0);

		if (mapped_length_aligned > mapped_length) {
			memset((void *)(vaddr_based + mapped_length), 0,
			    mapped_length_aligned - mapped_length);
		}

		if (full_length > mapped_length_aligned) {
			vaddr_t anon_addr = vaddr_based + mapped_length_aligned;
			r = vm_ps_allocate(&kernel_procstate, &anon_addr,
			    full_length - mapped_length_aligned, true);
			kassert(r == 0);
		}
	}

	kmem_free(phdrs, ehdr.e_phnum * ehdr.e_phentsize);

	return 0;
}

/*! @brief Copy out auxvals to a newly created user stack. */
int
copyout_args(struct exec_package *pkg, const char *argp[], const char *envp[])
{
	size_t narg = 0, nenv = 0;
	char *stackp = (char *)pkg->stack;
	uintptr_t *stackpuptr;

	for (const char **env = envp; *env; env++, nenv++) {
		stackp -= (strlen(*env) + 1);
		strcpy(stackp, *env);
	}

	for (const char **arg = argp; *arg; arg++, narg++) {
		stackp -= (strlen(*arg) + 1);
		strcpy(stackp, *arg);
	}

	/* align to 16 bytes */
	stackpuptr = (size_t *)(stackp - ((uintptr_t)stackp & 0xf));
	/* account for args/env */
	if ((narg + nenv + 3) % 2)
		--stackpuptr;

/* populate the auxv */
#define AUXV(TAG, VALUE)                  \
	*--stackpuptr = (uintptr_t)VALUE; \
	*--stackpuptr = TAG
	AUXV(0x0, 0x0);
	AUXV(AT_ENTRY, pkg->entry);
	AUXV(AT_PHDR, pkg->phaddr);
	AUXV(AT_PHENT, pkg->phentsize);
	AUXV(AT_PHNUM, pkg->phnum);

	*(--stackpuptr) = 0;
	stackpuptr -= nenv;
	stackp = (char *)pkg->stack;

	for (int i = 0; i < nenv; i++) {
		stackp -= strlen(envp[i]) + 1;
		stackpuptr[i] = (uintptr_t)stackp;
	}

	*(--stackpuptr) = 0;
	stackpuptr -= narg;
	for (int i = 0; i < narg; i++) {
		stackp -= strlen(argp[i]) + 1;
		stackpuptr[i] = (uintptr_t)stackp;
	}

	*(--stackpuptr) = narg;

	pkg->sp = (vaddr_t)stackpuptr;

	return 0;
}
