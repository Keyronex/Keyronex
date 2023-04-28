/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Jan 19 2023.
 */

#include <stdbool.h>

#include "kernel/ke_internal.h"

void
ke_semaphore_init(ksemaphore_t *sem, unsigned count)
{
	sem->hdr.type = kDispatchSemaphore;
	sem->hdr.signalled = count;
	TAILQ_INIT(&sem->hdr.waitblock_queue);
}

void
ke_semaphore_release(ksemaphore_t *sem, unsigned adjustment)
{
	ipl_t ipl = ke_acquire_dispatcher_lock();

	/* low: test for overflow; implement semaphore limits? */
	sem->hdr.signalled += adjustment;

	if (sem->hdr.signalled > 0) {
		kwaitblock_t *block = TAILQ_FIRST(&sem->hdr.waitblock_queue);
		while ((block) != NULL && ((sem->hdr.signalled) > 0)) {
			kwaitblock_t *next = TAILQ_NEXT(block, queue_entry);
			int i = ki_waiter_maybe_wakeup(block->thread,
			    &sem->hdr);
			(void)i;
			block = next;
		}
	}

	ke_release_dispatcher_lock(ipl);
}

void
ke_semaphore_release_maxone(ksemaphore_t *sem)
{
	ipl_t ipl = ke_acquire_dispatcher_lock();

	if (sem->hdr.signalled == 0) {
		kwaitblock_t *block = TAILQ_FIRST(&sem->hdr.waitblock_queue);
		sem->hdr.signalled = 1;
		if (block != NULL)
			ki_waiter_maybe_wakeup(block->thread, &sem->hdr);
	} else
		kassert(sem->hdr.signalled == 1);

	ke_release_dispatcher_lock(ipl);
}
