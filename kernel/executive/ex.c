#include "kdk/executive.h"
#include "kdk/object.h"
#include "kdk/vfs.h"
#include "ntcompat/ntcompat.h"
#include "vm/vmp.h"

kthread_t ex_init_thread;

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

	namecache_handle_t hdl = nchandle_retain(root_nch), out;

	kprintf("Before any lookups\n");
	nc_dump();

	int r = vfs_lookup(hdl, &out, "A/B/C/D.txt", 0);
	kprintf("\nAfter looking up A/B/C/D.txt (ret %d)...\n", r);
	nc_dump();

	r = vfs_lookup(hdl, &out, "E/F/G.TXT", 0);
	kprintf("\nAfter looking up E/F/G.TXT (ret %d)...\n", r);
	nc_dump();

	kfatal("R: %d, hdl.nc: %p\n", r, out.nc);

	ps_exit_this_thread();
}
