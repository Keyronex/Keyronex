#include <nanokern/thread.h>
#include <vm/vm.h>

static kmutex_t alloclock = KMUTEX_INITIALIZER(alloclock);

int
liballoc_lock()
{
	nk_wait(&alloclock, "kmem_liballoc_lock", false, false, -1);
	return 0;
}

int
liballoc_unlock()
{
	nk_mutex_release(&alloclock);
	return 0;
}

void *
liballoc_alloc(size_t pages)
{
	void *addr = (void*)vm_kalloc(pages, false);
	return addr;
}

int
liballoc_free(void *ptr, size_t pages)
{
	vm_kfree((vaddr_t)ptr, pages);
	return 0;
}
