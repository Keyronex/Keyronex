/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Jan 19 2023.
 */

#include "kdk/kern.h"
#include "ki.h"

void
ke_mutex_init(kmutex_t *mtx)
{
	mtx->hdr.type = kDispatchMutex;
	mtx->hdr.signalled = 1;
	ke_spinlock_init(&mtx->hdr.spinlock);
	TAILQ_INIT(&mtx->hdr.waitblock_queue);
}

void
ke_mutex_release(kmutex_t *mtx)
{
	kwaitblock_queue_t queue = TAILQ_HEAD_INITIALIZER(queue);
	ipl_t ipl = ke_spinlock_acquire(&mtx->hdr.spinlock);

	kassert(mtx->owner == curthread());
	mtx->owner = NULL;
	mtx->hdr.signalled++;
	kassert(mtx->hdr.signalled <= 1);

	ki_signal(&mtx->hdr, &queue);
	ke_spinlock_release_nospl(&mtx->hdr.spinlock);

	ke_acquire_scheduler_lock();
	ki_wake_waiters(&queue);
	ke_release_scheduler_lock(ipl);
}
