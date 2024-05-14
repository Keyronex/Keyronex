#include <errno.h>

#include "kdk/dev.h"
#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/nanokern.h"
#include "vm/vmp.h"

typedef struct futex {
	RB_ENTRY(futex) rb_entry;
	void *object;
	io_off_t offset;
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
	if (a->object < b->object)
		return -1;
	else if (a->object > b->object)
		return 1;
	else if (a->offset < b->offset)
		return -1;
	else if (a->offset > b->offset)
		return 1;
	else
		return 0;
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
futex_find(void *object, io_off_t offset, bool create)
{
	futex_t *found, key;

	key.object = object;
	key.offset = offset;
	found = RB_FIND(futex_rb, &futex_rbtree, &key);

	if (!found && create) {
		found = kmem_alloc(sizeof(futex_t));
		found->object = object;
		found->offset = offset;
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
		kmem_free(futex, sizeof(futex_t));
	}
}

static int
get_object_and_offset(uintptr_t uaddr, void **object, io_off_t *offset)
{
	vm_map_entry_t *entry;

	ke_wait(&ex_curproc()->vm->mutex, "futex get object and offset", false,
	    false, -1);

	entry = vmp_ps_vad_find(ex_curproc()->vm, uaddr);
	if (entry == NULL) {
		ke_mutex_release(&ex_curproc()->vm->mutex);
		return -1;
	}

	if (entry->object == NULL || !entry->flags.inherit_shared) {
		/* private mapping. */
		*object = ex_curproc();
		*offset = uaddr;
	} else {
		/* shared mapping .*/
		*object = entry->object;
		*offset = (uaddr - entry->start) + entry->flags.offset * PGSIZE;
	}

	ke_mutex_release(&ex_curproc()->vm->mutex);
	return 0;
}

int
krx_futex_wait(int *u_pointer, int expected, nanosecs_t ns)
{
	ipl_t ipl;
	futex_t *futex;
	void *object;
	io_off_t offset;
	kwaitresult_t w;
	int r = 0;

	get_object_and_offset((uintptr_t)u_pointer, &object, &offset);

	ipl = ke_spinlock_acquire(&futex_lock);
	futex = futex_find(object, offset, true);
	ke_spinlock_release_nospl(&futex_lock);

	if (*u_pointer != expected) {
		ke_spinlock_release(&futex_lock, ipl);
		return -EAGAIN;
	}

	w = ke_wait(&futex->semaphore, "sys_futex_wait", true, true, ns);
	switch (w) {
	case kKernWaitStatusOK:
		r = 0;
		break;

	case kKernWaitStatusTimedOut:
		r = -ETIMEDOUT;
		break;

	case kKernWaitStatusSignalled:
		kprintf("note: mlibc may not handle EINTR here\n");
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
krx_futex_wake(int *u_pointer)
{
	ipl_t ipl;
	futex_t *futex;
	void *object;
	io_off_t offset;

	get_object_and_offset((uintptr_t)u_pointer, &object, &offset);

	ipl = ke_spinlock_acquire(&futex_lock);
	futex = futex_find(object, offset, true);
	ke_spinlock_release(&futex_lock, ipl);

	if (futex == NULL)
		return 0;
	else
		ke_semaphore_release_maxone(&futex->semaphore);

	ipl = ke_spinlock_acquire(&futex_lock);
	futex_release(futex);
	ke_spinlock_release(&futex_lock, ipl);

	return 0;
}
