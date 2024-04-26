#include "kdk/executive.h"
#include "kdk/object.h"
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
	*(unsigned int**)addr = 0xdeadbeef;
	*(unsigned int**)(addr + PGSIZE) = 0xdeadbeef;
}

void
ex_init(void *)
{
	void ddk_init(void), ddk_autoconf(void);
	ddk_init();
#if 0
	pe_load(module_request.response->modules[3]->path,module_request.response->modules[3]->address);
#endif
	ddk_autoconf();

	vmp_pages_dump();
	obj_dump();

	test_anon();

	ps_exit_this_thread();
}
