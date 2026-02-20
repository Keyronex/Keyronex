/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file exec.c
 * @brief Executable loading.
 */

#include <sys/vm.h>
#include <sys/vnode.h>
#include <sys/kmem.h>
#include <sys/errno.h>

#include <libkern/lib.h>

#include <elf.h>
#include "sys/krx_vfs.h"
#include "sys/proc.h"

struct exec_package {
	vaddr_t stack;	  /* bottom of stack */
	vaddr_t sp;	  /* initial stack pointer to execute with */
	vaddr_t entry;	  /* entry IP */
	vaddr_t phaddr;	  /* address of phdr */
	size_t phentsize; /* size of a phdr */
	size_t phnum;	  /* count of phdrs */
};

typedef char **strv_t;

#if BITS == 32
typedef Elf32_Ehdr Elf_Ehdr;
typedef Elf32_Phdr Elf_Phdr;
#else
typedef Elf64_Ehdr Elf_Ehdr;
typedef Elf64_Phdr Elf_Phdr;
#endif

#define USER_STACK_SIZE PGSIZE * 32

void ke_md_enter_usermode(uintptr_t ip, uintptr_t sp);
void pmap_activate(struct vm_map *map);

static int
copyin_strv(char *const user_strv[], char ***out)
{
	size_t cnt = 0;
	char **strv;

	while (true) {
		if (user_strv[cnt++] == NULL)
			break;
	}

	strv = kmem_alloc((sizeof *user_strv) * cnt);
	memcpy(strv, user_strv, (sizeof *user_strv) * cnt);

	cnt = 0;
	while (strv[cnt] != NULL) {
		strv[cnt] = kmem_strdup(strv[cnt]);
		cnt++;
	}

	*out = strv;

	return 0;
}

static void
strv_free(strv_t strv)
{
	size_t cnt = 1;

	if (*strv == NULL)
		return;

	for (char **ptr = strv; *ptr != NULL; ptr++) {
		kmem_strfree(*ptr);
		cnt++;
	}

	/* cnt mismatches that in copyin_strv!! needs to be fixed */

	kmem_free(strv, sizeof(*strv) * cnt);
}

int
load_elf(vnode_t *vnode, vaddr_t base, struct exec_package *pkg)
{
	int r;
	Elf_Ehdr ehdr;
	Elf_Phdr *phdrs;
	vm_map_t *map = thread_vm_map(curthread());

	r = viewcache_io(vnode, 0, sizeof(ehdr), false, &ehdr);
	if (r < 0)
		kfatal("ubc_io: %d\n", r);

	if (memcmp(ehdr.e_ident, ELFMAG, 4) != 0)
		kfatal("load_elf: bad e_ident\n");

	phdrs = kmem_alloc(ehdr.e_phnum * ehdr.e_phentsize);
	if (!phdrs)
		return -ENOMEM;

	r = viewcache_io(vnode, ehdr.e_phoff, ehdr.e_phnum * ehdr.e_phentsize,
	    false, phdrs);
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
		vm_prot_t prot = VM_READ;

		if (phdr->p_type == PT_PHDR) {
			pkg->phaddr = base + phdr->p_vaddr;
			continue;
		} else if (phdr->p_type != PT_LOAD) {
			continue;
		}

		vaddr = rounddown2(phdr->p_vaddr, PGSIZE);
		vaddr_based = vaddr + base;
		file_offset_aligned = rounddown2(phdr->p_offset, PGSIZE);
		mapped_length = phdr->p_vaddr + phdr->p_filesz - vaddr;
		mapped_length_aligned = roundup2(mapped_length, PGSIZE);
		full_length = roundup2(phdr->p_vaddr + phdr->p_memsz, PGSIZE) -
		    vaddr;

		/* should really remap properly after doing initial memseting */
		prot |= VM_WRITE;

		if (phdr->p_flags & PF_W)
			prot |= VM_WRITE;

		if (phdr->p_flags & PF_X)
			prot |= VM_EXEC;

#if 0
		if ((prot & kVMExec) && (prot & kVMWrite))
			kprintf("note: riteable and executable segment\n");
#endif
		r = vm_map(map, vnode->file.vmobj,
		    &vaddr_based, mapped_length_aligned, file_offset_aligned,
		    prot, prot, false, true, true);
		kassert(r == 0);

		if (mapped_length_aligned > mapped_length) {
			memset((void *)(vaddr_based + mapped_length), 0,
			    mapped_length_aligned - mapped_length);
		}

		if (full_length > mapped_length_aligned) {
			vaddr_t anon_addr = vaddr_based + mapped_length_aligned;
			r = vm_allocate(map, prot,
			    &anon_addr, full_length - mapped_length_aligned,
			    true);
			kassert(r == 0);
		}
	}

	kmem_free(phdrs, ehdr.e_phnum * ehdr.e_phentsize);

	return 0;
}

/*! @brief Copy out auxvals to a newly created user stack. */
int
copyout_args(struct exec_package *pkg, const char *const argp[],
    const char *const envp[])
{
	size_t narg = 0, nenv = 0;
	char *stackp = (char *)pkg->stack;
	uintptr_t *stackpuptr;

	for (const char *const *env = envp; env && *env; env++, nenv++) {
		stackp -= (strlen(*env) + 1);
		strcpy(stackp, *env);
	}

	for (const char *const *arg = argp; arg && *arg; arg++, narg++) {
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

int
load_init(vnode_t *server_vnode, vnode_t *ld_vnode)
{
	int r = 0;
	struct exec_package pkg, rtldpkg;
	const char *argp[] = { "init", NULL },
		   *envp[] = { NULL };

	/* assume it's not PIE */
	r = load_elf(server_vnode, (vaddr_t)0x0, &pkg);
	if (r < 0)
		kfatal("failed to load init\n");
	r = load_elf(ld_vnode, (vaddr_t)0x40000000, &rtldpkg);
	if (r < 0)
		kfatal("failed to load rtld for init\n");

	pkg.stack = -1;
	r = vm_allocate(thread_vm_map(curthread()), VM_READ | VM_WRITE,
	    &pkg.stack, USER_STACK_SIZE, false);
	kassert(r == 0);

	pkg.stack += USER_STACK_SIZE;
	r = copyout_args(&pkg, argp, envp);
	kassert(r == 0);

	/*
	 * leaks init nch?
	 * we want proc struct to keep an nch reference on what it opened anyway
	 */

	ke_md_enter_usermode(rtldpkg.entry, pkg.sp);

	return r;
}

int
sys_execve(proc_t *proc, const char *upath, char *const uarpg[],
    char *const uenvp[])
{
	int r;
	char *path = NULL, **argp = NULL, **envp = NULL;
	namecache_handle_t exe_nch = { 0 }, ld_nch = { 0 };
	vm_map_t *newmap = NULL, *oldmap = proc->vm_map;
	struct exec_package pkg, rtldpkg;
	ipl_t ipl;

	r = strldup_user(&path, upath, strlen(upath) + 1);
	if (r < 0)
		return r;

#if TRACE_SYSCALLS
	kprintf_dbg("sys_execve: path='%s'\n", path);
#endif

	r = copyin_strv(uarpg, &argp);
	if (r < 0)
		goto error;

	r = copyin_strv(uenvp, &envp);
	if (r < 0)
		goto error;

	r = vfs_lookup_simple(root_nch, &exe_nch, upath, 0);
	if (r < 0)
		goto error;

	r = vfs_lookup_simple(root_nch, &ld_nch, "/usr/lib/ld.so", 0);
	if (r < 0)
		goto error;

	newmap = vm_map_create();
	if (newmap == NULL) {
		r = -ENOMEM;
		goto error;
	}

	ipl = spldisp();
	curthread()->vm_map = newmap;
	pmap_activate(newmap);
	splx(ipl);

	r = load_elf(exe_nch.nc->vp, (vaddr_t)0x0, &pkg);
	if (r < 0)
		kfatal("failed to load init\n");

	r = load_elf(ld_nch.nc->vp, (vaddr_t)0x40000000, &rtldpkg);
	if (r < 0)
		kfatal("failed to load rtld for init\n");

	pkg.stack = -1;
	r = vm_allocate(newmap, VM_READ | VM_WRITE,
	    &pkg.stack, USER_STACK_SIZE, false);
	if (r != 0)
		goto error;

	pkg.stack += USER_STACK_SIZE;
	r = copyout_args(&pkg, (const char *const *)argp,
	    (const char *const *)envp);
	kassert(r == 0);

	ipl = spldisp();
	curproc()->vm_map = newmap;
	curthread()->vm_map = NULL; /* stop overriding */
	splx(ipl);
	vm_unmap(oldmap, LOWER_HALF, LOWER_HALF + LOWER_HALF_SIZE);
	vm_map_release(oldmap);

	strncpy(curproc()->comm, argp[0], sizeof(curproc()->comm) - 1);

	ke_md_enter_usermode(rtldpkg.entry, pkg.sp);

	kfatal("unreachable\n");

error:
	/* FIXME: (or in nchandle_release()) - if unset, null deref... */
	nchandle_release(exe_nch);
	nchandle_release(ld_nch);
	kmem_free(path, strlen(path) + 1);
	strv_free(argp);
	strv_free(envp);
	if (newmap != NULL) {
		ipl = spldisp();
		curthread()->vm_map = NULL;
		pmap_activate(oldmap);
		splx(ipl);
		vm_unmap(newmap, LOWER_HALF, LOWER_HALF + LOWER_HALF_SIZE);
		vm_map_release(newmap);
	}

	return r;
}
