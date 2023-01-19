#include <nanokern/thread.h>
#include <libkern/libkern.h>
#include <vm/vm.h>

#include <string.h>

#define PGQ_INITIALIZER(PGQ)                                             \
	{                                                                \
		.queue = TAILQ_HEAD_INITIALIZER(PGQ.queue), .npages = 0, \
	}

vm_pagequeue_t vm_pgfreeq = PGQ_INITIALIZER(vm_pgfreeq),
	       vm_pgkmemq = PGQ_INITIALIZER(vm_pgkmemq),
	       vm_pgwiredq = PGQ_INITIALIZER(vm_pgwiredq),
	       vm_pgactiveq = PGQ_INITIALIZER(vm_pgactiveq),
	       vm_pginactiveq = PGQ_INITIALIZER(vm_pginactiveq),
	       vm_pgpmapq = PGQ_INITIALIZER(vm_pgpmapq);

vm_pregion_queue_t vm_pregion_queue = TAILQ_HEAD_INITIALIZER(vm_pregion_queue);

vm_page_t *
vm_page_from_paddr(paddr_t paddr)
{
	vm_pregion_t *preg;

	TAILQ_FOREACH (preg, &vm_pregion_queue, queue) {
		if (preg->base <= paddr &&
		    (preg->base + PGSIZE * preg->npages) > paddr) {
			return &preg->pages[(paddr - preg->base) / PGSIZE];
		}
	}

	return NULL;
}

vm_pagequeue_t *
vm_page_queue(vm_page_t *page)
{
	switch (page->queue) {
	case kVMPageFree:
		return &vm_pgfreeq;
	case kVMPageKMem:
		return &vm_pgkmemq;
	case kVMPageWired:
		return &vm_pgactiveq;
	case kVMPageActive:
		return &vm_pgactiveq;
	case kVMPageInactive:
		return &vm_pginactiveq;
	default:
		kfatal("unreached\n");
	}
}

void
vm_page_changequeue(vm_page_t *page, kx_nullable vm_pagequeue_t *from,
    vm_pagequeue_t *to)
{
	kassert(page != NULL);
	kassert(to != NULL);

	if (!from) {
		from = vm_page_queue(page);
	}

	TAILQ_REMOVE(&from->queue, page, pagequeue);
	from->npages--;

	TAILQ_INSERT_HEAD(&to->queue, page, pagequeue);
	to->npages++;
}

vm_page_t *
vm_pagealloc(bool sleep, vm_pagequeue_t *queue)
{
	vm_page_t *page;

	// ipl_t ipl = nk_spinlock_acquire(&pages_lock)
	page = TAILQ_FIRST(&vm_pgfreeq.queue);
	if (!page) {
		kfatal("vm_allocpage: oom not yet handled\n");
	}
	vm_page_changequeue(page, &vm_pgfreeq, queue);

	memset(P2V(page->paddr), 0x0, PGSIZE);

	return page;
}

void
vm_page_free(vm_page_t *page)
{
	kassert(page != NULL);
	vm_page_changequeue(page, NULL, &vm_pgfreeq);
}


void
vm_pagedump(void)
{
	kprintf("\033[7m%-9s%-9s%-9s%-9s%-9s%-9s\033[m\n", "free", "kmem",
	    "wired", "active", "inactive", "pmap");

	kprintf("%-9zu%-9zu%-9zu%-9zu%-9zu%-9zu\n", vm_pgfreeq.npages,
	    vm_pgkmemq.npages, vm_pgwiredq.npages, vm_pgactiveq.npages,
	    vm_pginactiveq.npages, vm_pgpmapq.npages);
}
