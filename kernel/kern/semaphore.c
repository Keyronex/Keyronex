/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Jan 19 2023.
 */

#include <stdbool.h>

#include "ki.h"

void
ke_semaphore_init(ksemaphore_t *sem, unsigned count)
{
	sem->hdr.type = kDispatchSemaphore;
	sem->hdr.signalled = count;
	ke_spinlock_init(&sem->hdr.spinlock);
	TAILQ_INIT(&sem->hdr.waitblock_queue);
}

void
ke_semaphore_reset(ksemaphore_t *sem, unsigned count)
{
	ipl_t ipl = ke_spinlock_acquire(&sem->hdr.spinlock);
	sem->hdr.signalled = count;
	ke_spinlock_release(&sem->hdr.spinlock, ipl);
}

void
ke_semaphore_release(ksemaphore_t *sem, unsigned adjustment)
{
	kwaitblock_queue_t queue = TAILQ_HEAD_INITIALIZER(queue);
	ipl_t ipl = ke_spinlock_acquire(&sem->hdr.spinlock);

	/* low: test for overflow; implement semaphore limits? */
	sem->hdr.signalled += adjustment;
	ki_signal(&sem->hdr, &queue);
	ke_spinlock_release_nospl(&sem->hdr.spinlock);

	ki_wake_waiters(&queue);
	splx(ipl);
}

void
ke_semaphore_release_maxone(ksemaphore_t *sem)
{
	kwaitblock_queue_t queue = TAILQ_HEAD_INITIALIZER(queue);
	ipl_t ipl = ke_spinlock_acquire(&sem->hdr.spinlock);

	/* low: test for overflow; implement semaphore limits? */
	if (sem->hdr.signalled == 0)
		sem->hdr.signalled = 1;
	ki_signal(&sem->hdr, &queue);
	ke_spinlock_release_nospl(&sem->hdr.spinlock);

	ki_wake_waiters(&queue);
	splx(ipl);
}
