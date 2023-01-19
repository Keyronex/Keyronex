#include <nanokern/thread.h>

#include <stdbool.h>

void
nk_semaphore_init(ksemaphore_t *sem, unsigned count)
{
	sem->hdr.type = kDispatchSemaphore;
	sem->hdr.signalled = count;
	TAILQ_INIT(&sem->hdr.waitblock_queue);
}

void
nk_semaphore_release(ksemaphore_t *sem, unsigned adjustment)
{
	ipl_t ipl = nk_spinlock_acquire(&nk_lock);

	/* low: test for overflow; implement semaphore limits? */
	sem->hdr.signalled += adjustment;

	if (sem->hdr.signalled > 0) {
		kwaitblock_t *block = TAILQ_FIRST(&sem->hdr.waitblock_queue);
		while ((block) != NULL && ((sem->hdr.signalled - 1) > 0)) {
			kwaitblock_t *next = TAILQ_NEXT(block, queue_entry);
			nkx_waiter_maybe_wakeup(block->thread, &sem->hdr);
			block = next;
		}
	}

	nk_spinlock_release(&nk_lock, ipl);
}
