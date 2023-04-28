/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Apr 24 2023.
 */

#include <sys/errno.h>
#include <sys/time.h>

#include <kdk/kernel.h>
#include <kdk/kmem.h>
#include <kdk/process.h>
#include <kdk/vm.h>

typedef struct futex {
	RB_ENTRY(futex) rb_entry;
	vm_page_t *page;
	paddr_t paddr;
	ksemaphore_t semaphore;
	size_t refcnt;
} futex_t;

RB_HEAD(futex_rb, futex);

static intptr_t futex_cmp(futex_t *a, futex_t *b);
RB_GENERATE(futex_rb, futex, rb_entry, futex_cmp);

static kspinlock_t futex_lock = KSPINLOCK_INITIALISER;
static struct futex_rb futex_rbtree = RB_INITIALIZER(futex_rbtree);

static intptr_t
futex_cmp(futex_t *a, futex_t *b)
{
	return a->paddr - b->paddr;
}

/*!
 * @brief Find or optionally create a futex for a physical address.
 *
 * The futex's reference count is incremented. If the futex is created, it wires
 * the page.
 *
 * @pre futex_lock held
 *
 * @return Pointer to futex object if found or created, NULL if not found and
 * creation not requested.
 */
static futex_t *
futex_find(vm_page_t *page, paddr_t paddr, bool create)
{
	futex_t *found, key;

	key.paddr = paddr;
	found = RB_FIND(futex_rb, &futex_rbtree, &key);

	if (!found && create) {
		found = kmem_alloc(sizeof(futex_t));
		/* futex object takes over the page wiring */
		vm_page_wire(page);
		found->page = page;
		found->paddr = paddr;
		found->refcnt = 1;
		ke_semaphore_init(&found->semaphore, 0);
		RB_INSERT(futex_rb, &futex_rbtree, found);
	} else if (found)
		found->refcnt++;

	return found;
}

/*!
 * @brief Release a reference to a futex.
 *
 * @pre futex_lock held
 */
static void
futex_release(futex_t *futex)
{
	if (--futex->refcnt == 0) {
		RB_REMOVE(futex_rb, &futex_rbtree, futex);
		kassert(TAILQ_EMPTY(&futex->semaphore.hdr.waitblock_queue));
		vm_page_unwire(futex->page);
		kmem_free(futex, sizeof(futex_t));
	}
}

static void *
to_hhdm(vm_page_t *page, void *ptr)
{
	char *base = (char *)VM_PAGE_PADDR(page);
	kassert((uintptr_t)base % PGSIZE == 0);
	base += ((uintptr_t)ptr) % PGSIZE;
	return P2V(base);
}

int
sys_futex_wait(int *u_pointer, int expected, const struct timespec *u_time)
{
	nanosecs_t ns = -1;
	vm_page_t *page;
	int *pointer;
	ipl_t ipl;
	futex_t *futex;
	vm_fault_return_t fret;
	int r = 0;
	kwaitstatus_t w;

	if (u_time)
		ns = u_time->tv_sec * NS_PER_S + u_time->tv_nsec;

	fret = vm_fault(ps_curproc()->map, PGROUNDDOWN(u_pointer), 0, &page);
	kassert(fret == kVMFaultRetOK);

	pointer = to_hhdm(page, u_pointer);

	ipl = ke_spinlock_acquire(&futex_lock);
	if (*pointer != expected) {
		vm_page_unwire(page);
		ke_spinlock_release(&futex_lock, ipl);
		return -EAGAIN;
	}
	futex = futex_find(page, (paddr_t)pointer, true);
	ke_spinlock_release_nospl(&futex_lock);

	/* futex keeps it wired, we can drop our extra wire */
	vm_page_unwire(page);

	w = ke_wait(&futex->semaphore, "sys_futex_wait", true, true, ns);
	switch (w) {
	case kKernWaitStatusOK:
		r = 0;
		break;

	case kKernWaitStatusTimedOut:
		r = -ETIMEDOUT;
		break;

	case kKernWaitStatusSignalled:
		kdprintf("note: mlibc may not handle EINTR here\n");
		r = -EINTR;
		break;

	default:
		kfatal("unexpected ke_wait return %d\n", w);
	}

	ipl = ke_spinlock_acquire(&futex_lock);
	futex_release(futex);
	ke_spinlock_release(&futex_lock, ipl);

	return r;
}

int
sys_futex_wake(int *u_pointer)
{
	vm_page_t *page;
	ipl_t ipl;
	futex_t *futex;
	int *pointer;
	vm_fault_return_t fret;

	fret = vm_fault(ps_curproc()->map, PGROUNDDOWN(u_pointer), 0, &page);
	kassert(fret == kVMFaultRetOK);

	pointer = to_hhdm(page, u_pointer);

	ipl = ke_spinlock_acquire(&futex_lock);
	futex = futex_find(page, (paddr_t)pointer, true);
	ke_spinlock_release(&futex_lock, ipl);

	vm_page_unwire(page);

	if (futex == NULL)
		return 0;
	else
		ke_semaphore_release_maxone(&futex->semaphore);

	ipl = ke_spinlock_acquire(&futex_lock);
	futex_release(futex);
	ke_spinlock_release(&futex_lock, ipl);

	return 0;
}