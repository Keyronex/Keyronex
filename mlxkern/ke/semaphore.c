#include <stdbool.h>

#include "ke/ke_internal.h"

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
