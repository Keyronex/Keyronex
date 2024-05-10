#include <keyronex/syscall.h>

#include "exp.h"
#include "kdk/executive.h"
#include "kdk/nanokern.h"
#include "kdk/object.h"
#include "kdk/vfs.h"
#include "ntcompat/ntcompat.h"
#include "vm/vmp.h"

/* exec.c */
int load_server(vnode_t *server_vnode, vnode_t *ld_vnode);

kthread_t *ex_init_thread;
obj_class_t process_class;

static void
test_anon(void)
{
	vaddr_t addr;
	extern vm_procstate_t kernel_procstate;
	int r = vm_ps_allocate(&kernel_procstate, &addr, PGSIZE * 4, false);
	kassert(r == 0);
	kprintf("Allocated at 0x%zx\n", addr);
	*(unsigned int*)addr = 0xdeadbeef;
	*(unsigned int*)(addr + PGSIZE) = 0xbeefdead;
	*(unsigned int*)(addr + PGSIZE * 2) = 0xfeedbeef;
	*(unsigned int*)(addr + PGSIZE * 3) = 0xbeeffeed;

	*(unsigned int*)addr = 0xdeadbeef;
	*(unsigned int*)(addr + PGSIZE) = 0xbeefdead;

	kprintf("After touching 4 pages:\n");
	vmp_wsl_dump(&kernel_procstate);
	vmp_pages_dump();
}

void
user_init(void *arg)
{
	int r;
	namecache_handle_t ld, posix;

	r = vfs_lookup(root_nch, &ld, "/usr/lib/ld.so", 0);
	if (r != 0)
		kfatal("Failed to look up RTDL\n");

	r = vfs_lookup(root_nch, &posix, "/usr/bin/posix_server", 0);
	if (r != 0)
		kfatal("Failed to look up POSIX Server\n");

	r = load_server(posix.nc->vp, ld.nc->vp);
	if (r != 0)
		kfatal("Failed to load POSIX server\n");

	for (;;) ;

}

void
ex_init(void *)
{
	void ddk_init(void), ddk_autoconf(void), ubc_init(void);

	vmp_paging_init();
	ubc_init();
	ddk_init();
#if 0
	pe_load(module_request.response->modules[3]->path,module_request.response->modules[3]->address);
#endif
	ddk_autoconf();

#if 0
	vmp_pages_dump();
	obj_dump();

	test_anon();
#endif

	process_class = obj_new_type("process");

	namecache_handle_t hdl = nchandle_retain(root_nch), out;

	kprintf("Before any lookups\n");
	nc_dump();

	int r = vfs_lookup(hdl, &out, "A/B/C/D.txt", 0);
	kprintf("\nAfter looking up A/B/C/D.txt (ret %d)...\n", r);
	nc_dump();

	r = vfs_lookup(hdl, &out, "E/F/G.TXT", 0);
	kprintf("\nAfter looking up E/F/G.TXT (ret %d)...\n", r);
	nc_dump();

	eprocess_t *initps;
	kthread_t *initthread;
	r = ps_process_create(&initps, false);
	kassert(r == 0);

	r = ps_thread_create(&initthread, "user init thread 0", user_init, NULL, initps);
	kassert(r == 0);

	ke_thread_resume(initthread);

	ps_exit_this_thread();
}

int
krx_vm_allocate(size_t size, vaddr_t *out)
{
	int r;
	vaddr_t vaddr;
	r = vm_ps_allocate(ex_curproc()->vm, &vaddr, size, false);
	*out = vaddr;
	return r;
}

uintptr_t
ex_syscall_dispatch(enum krx_syscall syscall, uintptr_t arg1, uintptr_t arg2,
    uintptr_t arg3, uintptr_t arg4, uintptr_t arg5, uintptr_t arg6,
    uintptr_t *out1)
{
	switch (syscall) {
	case kKrxDebugMessage:
		kprintf("[libc]: %s\n", (const char *)arg1);
		return 0;

	case kKrxVmAllocate:
		return krx_vm_allocate(arg1, out1);

	default:
		kfatal("unhandled syscall %d\n", syscall);
	}
}
