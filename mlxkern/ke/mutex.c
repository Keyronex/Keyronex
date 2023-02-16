#include "ke/ke_internal.h"

void
ke_mutex_init(kmutex_t *mtx)
{
	mtx->hdr.type = kDispatchMutex;
	mtx->hdr.signalled = 1;
	TAILQ_INIT(&mtx->hdr.waitblock_queue);
}

void
ke_mutex_release(kmutex_t *mtx)
{
	ipl_t ipl = ke_acquire_dispatcher_lock();

	kassert(mtx->owner == ke_curthread());
	mtx->hdr.signalled++;
	kassert(mtx->hdr.signalled <= 1);

	if (mtx->hdr.signalled == 1) {
		kwaitblock_t *block = TAILQ_FIRST(&mtx->hdr.waitblock_queue);
		while ((block) != NULL) {
			kwaitblock_t *next = TAILQ_NEXT(block, queue_entry);
			if (ki_waiter_maybe_wakeup(block->thread, &mtx->hdr))
				break;
			block = next;
		}
	}

	ke_release_dispatcher_lock(ipl);
}
