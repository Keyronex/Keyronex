/*
 * Copyright (c) 2022-2023 NetaScale Object Solutions.
 * Created on Wed Mar 22 2023.
 */

#include <sys/auxv.h>

#include <elf.h>
#include <errno.h>

#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "pxp.h"

#define ELFMAG "\177ELF"
#define STRV_FOREACH(PSTR, STRV) \
	for ((PSTR) = (STRV); (PSTR) && *(PSTR); (PSTR)++)

typedef struct exec_package {
	vm_map_t *map;	  /* map to load into */
	vaddr_t stack;	  /* bottom of stack */
	vaddr_t sp;	  /* initial stack pointer to execute with */
	vaddr_t entry;	  /* entry IP */
	vaddr_t phaddr;	  /* address of phdr */
	size_t phentsize; /* size of a phdr */
	size_t phnum;	  /* count of phdrs */
} exec_package_t;

typedef char **strv_t;

static size_t
strv_length(strv_t strv)
{
	size_t n;

	for (n = 0; *strv != NULL; strv++)
		n++;

	return n;
}

/*
 * Prepend a simple array of string pointers (no terminating NULL) to an strv.
 * The string pointers are copied as is and not duplicated to yield new strings.
 */
static int
strv_precat_num_consume(strv_t *a, char **b, size_t n)
{
	char **new_strv = kmem_alloc(
	    sizeof(char *) * (strv_length(*a) + n + 1));

	memcpy(new_strv, b, sizeof(char *) * n);
	/* include trailing null */
	memcpy(&new_strv[n], *a, sizeof(char *) * (strv_length(*a) + 1));

	*a = new_strv;

	return 0;
}

void
parse_shebang(const char *shebang, char **interp_out, char ***args_out,
    size_t *n_args_out)
{
	const char *start = shebang;

	/* skip leading whitespace */
	while (isspace(*start)) {
		start++;
	}

	/* get end of interpreter path; don't support spaces in that yet. */
	const char *end = strchr(start, ' ');
	if (end == NULL) {
		/* no arguments present */
		end = start + strlen(start);
	}

	/* get length of the interpreter path */
	size_t length = end - start;

	*interp_out = kmem_alloc((length + 1) * sizeof(char));
	strncpy(*interp_out, start, length);
	(*interp_out)[length] = '\0';

	/* count number of arguments */
	*n_args_out = 1;
	*args_out = kmem_alloc(sizeof(char *));
	(*args_out)[0] = kmem_alloc((length + 1) * sizeof(char));
	strncpy((*args_out)[0], *interp_out, length);
	((*args_out)[0])[length] = '\0';

	if (*end != '\0') {
		const char *arg = end + 1;
		const char *arg_end = strchr(arg, ' ');

		while (arg != NULL) {
			if (arg_end != NULL)
				length = arg_end - arg;
			else
				length = strlen(arg);

			*args_out = kmem_realloc(*args_out,
			    (*n_args_out) * sizeof(char *),
			    (*n_args_out + 1) * sizeof(char *));
			(*args_out)[*n_args_out] = kmem_alloc(
			    (length + 1) * sizeof(char));
			strncpy((*args_out)[*n_args_out], arg, length);
			((*args_out)[*n_args_out])[length] = '\0';

			(*n_args_out)++;

			if (arg_end != NULL) {
				arg = arg_end + 1;
				arg_end = strchr(arg, ' ');
			} else {
				arg = NULL;
			}
		}
	}
}

static int
loadscript(char **path, strv_t *argp)
{
	vnode_t *vn;
	char first[2];
	char *line, *nl;
	char *interpreter;
	char **interp_args;
	size_t n_args;
	int r;

	r = vfs_lookup(root_vnode, &vn, *path, 0);
	if (r < 0) {
		kdprintf("exec: failed to lookup %s (errno %d)\n", *path, -r);
		return r;
	}

	r = VOP_READ(vn, first, sizeof(first), 0, 0);
	if (r < 0) {
		kdprintf("exec: failed to read %s (errno %d)\n", *path, -r);
		return r;
	}

	if (memcmp(first, "#!", 2) != 0) {
		kdprintf("exec: not a script in %s\n", *path);
		return -ENOEXEC;
	}

	line = kmem_alloc(256);
	memset(line, 0x0, 256);

	r = VOP_READ(vn, line, 256, 2, 255);
	if (r < 0) {
		kdprintf("exec script: failed to read %s (errno %d)\n", *path,
		    -r);
		kmem_free(line, 256);
		return r;
	}

	nl = strchr(line, '\n');
	if (nl)
		*nl = '\0';

	parse_shebang(line, &interpreter, &interp_args, &n_args);

	strv_precat_num_consume(argp, interp_args, n_args);

	kmem_free(interp_args, sizeof(char *) * n_args);
	kmem_free(line, 256);

	kmem_strfree(*path);
	*path = interpreter;

	return 0;
}

static int
loadelf(const char *path, vaddr_t base, exec_package_t *pkg)
{
	vnode_t *vn;
	Elf64_Ehdr ehdr;
	Elf64_Phdr *phdrs;
	int r;

	r = vfs_lookup(root_vnode, &vn, path, 0);
	if (r < 0) {
		kdprintf("exec: failed to lookup %s (errno %d)\n", path, -r);
		return r;
	}

	r = VOP_READ(vn, &ehdr, sizeof ehdr, 0, 0);
	if (r < 0) {
		kdprintf("exec: failed to read %s (errno %d)\n", path, -r);
		return r;
	}

	if (memcmp(ehdr.e_ident, ELFMAG, 4) != 0) {
		kdprintf("exec: bad e_ident in %s\n", path);
		return -ENOEXEC;
	}

	phdrs = kmem_alloc(ehdr.e_phnum * ehdr.e_phentsize);
	if (!phdrs)
		return -ENOMEM;

	r = VOP_READ(vn, phdrs, ehdr.e_phnum * ehdr.e_phentsize, ehdr.e_phoff,
	    0);
	if (r < 0)
		return r;

	pkg->entry = base + ehdr.e_entry;
	pkg->phentsize = ehdr.e_phentsize;
	pkg->phnum = ehdr.e_phnum;
	pkg->phaddr = 0x0;

	for (int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr *phdr = &phdrs[i];
		size_t pageoff;
		size_t size;
		vaddr_t segbase;
		vaddr_t mapaddr;

		if (phdr->p_type == PT_PHDR) {
			pkg->phaddr = base + phdr->p_vaddr;
			continue;
		} else if (phdr->p_type != PT_LOAD)
			continue;

		segbase = (vaddr_t)PGROUNDDOWN(phdr->p_vaddr);
		pageoff = phdr->p_vaddr - (uintptr_t)segbase;
		size = PGROUNDUP(pageoff + phdr->p_memsz);
		segbase += (uintptr_t)base;
		mapaddr = segbase + pageoff;

#if 0
		r = vm_map_object(pkg->map, vn->vmobj, &mapaddr,
		    PGROUNDUP(phdr->p_filesz), phdr->p_offset, kVMAll, kVMAll,
		    kVMInheritCopy, true, true);
#else
		(void)mapaddr;
		r = vm_map_allocate(pkg->map, &segbase, size, true);
		kassert(r == 0);

		r = VOP_READ(vn, (void *)(segbase + pageoff), phdr->p_filesz,
		    phdr->p_offset, 0);
#endif
		if (r < 0)
			return r; /* TODO: this won't work anymore */
	}

	kmem_free(phdrs, ehdr.e_phnum * ehdr.e_phentsize);

	return 0;
}

/*! @brief Copy out auxvals to a newly created user stack. */
static int
copyout_args(exec_package_t *pkg, const char *argp[], const char *envp[])
{
	size_t narg = 0, nenv = 0;
	char *stackp = (char *)pkg->stack;
	uint64_t *stackpu64;

	for (const char **env = envp; *env; env++, nenv++) {
		stackp -= (strlen(*env) + 1);
		strcpy(stackp, *env);
	}

	for (const char **arg = argp; *arg; arg++, narg++) {
		stackp -= (strlen(*arg) + 1);
		strcpy(stackp, *arg);
	}

	/* align to 16 bytes */
	stackpu64 = (size_t *)(stackp - ((uintptr_t)stackp & 0xf));
	/* account for args/env */
	if ((narg + nenv + 3) % 2)
		--stackpu64;

/* populate the auxv */
#define AUXV(TAG, VALUE)                \
	*--stackpu64 = (uint64_t)VALUE; \
	*--stackpu64 = TAG
	AUXV(0x0, 0x0);
	AUXV(AT_ENTRY, pkg->entry);
	AUXV(AT_PHDR, pkg->phaddr);
	AUXV(AT_PHENT, pkg->phentsize);
	AUXV(AT_PHNUM, pkg->phnum);

	*(--stackpu64) = 0;
	stackpu64 -= nenv;
	stackp = (char *)pkg->stack;

	for (int i = 0; i < nenv; i++) {
		stackp -= strlen(envp[i]) + 1;
		stackpu64[i] = (uint64_t)stackp;
	}

	*(--stackpu64) = 0;
	stackpu64 -= narg;
	for (int i = 0; i < narg; i++) {
		stackp -= strlen(argp[i]) + 1;
		stackpu64[i] = (uint64_t)stackp;
	}

	*(--stackpu64) = narg;

	pkg->sp = (vaddr_t)stackpu64;

	return 0;
}

/*!
 * Copy in a string vector to kernel space.
 */
static int
copyin_strv(const char *user_strv[], char ***out)
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
		strv[cnt] = strdup(strv[cnt]);
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
sys_exec(posix_proc_t *proc, const char *u_path, const char *u_argp[],
    const char *u_envp[], hl_intr_frame_t *frame)
{
	int r = 0;
	exec_package_t pkg, rtldpkg;
	char *path = NULL, **argp = NULL, **envp = NULL;
	vm_map_t *oldmap = proc->eprocess->map;

	/* TODO: check for other threads, terminate them all */

	kassert(oldmap != kernel_process.map);
	pkg.map = rtldpkg.map = kmem_alloc(sizeof(*pkg.map));
	vm_map_init(pkg.map);
	kassert(pkg.map != NULL);

	path = strdup(u_path);
	r = copyin_strv(u_argp, &argp);
	kassert(r == 0);
	r = copyin_strv(u_envp, &envp);
	kassert(r == 0);

#if DEBUG_SYSCALLS == 1
	kdprintf("SYS_EXEC(%s)\n", path);
	for (const char **arg = u_argp; *arg != NULL; arg++)
		kdprintf("ARG: %s\n", *arg);
#endif

	proc->eprocess->map = pkg.map;
	vm_map_activate(pkg.map);

load:
	/* assume it's not PIE */
	r = loadelf(path, (vaddr_t)0x0, &pkg);
	if (r < 0) {
		r = loadscript(&path, &argp);
		if (r < 0)
			goto fail;
		else
			goto load;
	}

	r = loadelf("/usr/lib/ld.so", (vaddr_t)0x40000000, &rtldpkg);
	if (r < 0)
		goto fail;

	pkg.stack = -1;
	r = vm_map_allocate(pkg.map, &pkg.stack, USER_STACK_SIZE, false);
	kassert(r == 0);
	pkg.stack += USER_STACK_SIZE;
	r = copyout_args(&pkg, (const char **)argp, (const char **)envp);
	kassert(r == 0);

	vm_map_free(oldmap);

	/* todo(low): separate this machine-dependent stuff */
	memset(frame, 0x0, sizeof(*frame));
	frame->rip = (uint64_t)rtldpkg.entry;
	frame->rsp = (uint64_t)pkg.sp;
	frame->cs = 0x38 | 0x3;
	frame->ss = 0x40 | 0x3;
	frame->rflags = 0x202;
	r = 0;

	if (r != 0) {
	fail:
		vm_map_activate(oldmap);
		proc->eprocess->map = oldmap;
		if (pkg.map)
			vm_map_free(pkg.map);
	}

	kmem_strfree(path);
	strv_free(argp);
	strv_free(envp);
	return r;
}
