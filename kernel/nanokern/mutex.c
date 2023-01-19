#include <nanokern/kernmisc.h>
#include <nanokern/thread.h>

void
nk_mutex_init(kmutex_t *mtx)
{
	mtx->hdr.type = kDispatchMutex;
	mtx->hdr.signalled = 1;
	TAILQ_INIT(&mtx->hdr.waitblock_queue);
}

void
nk_mutex_release(kmutex_t *mtx)
{
	ipl_t ipl = nk_spinlock_acquire(&nk_lock);

	nk_assert(mtx->owner == curcpu()->running_thread);
	mtx->hdr.signalled++;
	nk_assert(mtx->hdr.signalled <= 1);

	if (mtx->hdr.signalled == 1) {
		kwaitblock_t *block = TAILQ_FIRST(&mtx->hdr.waitblock_queue);
		while ((block) != NULL) {
			kwaitblock_t *next = TAILQ_NEXT(block, queue_entry);
			if (nkx_waiter_maybe_wakeup(block->thread, &mtx->hdr))
				break;
			block = next;
		}
	}

	nk_spinlock_release(&nk_lock, ipl);
}
