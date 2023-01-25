#include <kern/kmem.h>

#include <nanokern/kernmisc.h>
#include <nanokern/thread.h>
#include <stdbool.h>

void
nk_msgqueue_init(kmsgqueue_t *msgq, unsigned count)
{
	msgq->hdr.type = kDispatchMsgQueue;
	msgq->hdr.signalled = 0;
	TAILQ_INIT(&msgq->hdr.waitblock_queue);
	msgq->size = count;
	msgq->readhead = 0;
	msgq->writehead = 0;
	msgq->messages = kmem_alloc(sizeof(void *) * count);
	nk_semaphore_init(&msgq->sem, count);
}

void
nk_msgq_post(kmsgqueue_t *queue, void *msg)
{
	kwaitstatus_t wait = nk_wait(&queue->sem, "msgqueue_wait", false, false,
	    -1);
	nk_assert(wait == kKernWaitStatusOK);

	ipl_t ipl = nk_spinlock_acquire(&nk_lock);

	queue->messages[queue->writehead++] = msg;
	if (queue->writehead == queue->size)
		queue->writehead = 0;

	nk_assert(queue->writehead != queue->readhead);
	queue->hdr.signalled = true;

	kwaitblock_t *block = TAILQ_FIRST(&queue->hdr.waitblock_queue);
	while ((block) != NULL) {
		kwaitblock_t *next = TAILQ_NEXT(block, queue_entry);
		if (nkx_waiter_maybe_wakeup(block->thread, &queue->hdr))
			break;
		block = next;
	}

	nk_spinlock_release(&nk_lock, ipl);
}

int
nk_msgq_read(kmsgqueue_t *queue, void **msg)
{
	ipl_t ipl = nk_spinlock_acquire(&nk_lock);

	if (queue->writehead == queue->readhead) {
		nk_spinlock_release(&nk_lock, ipl);
		return -1;
	}

	*msg = queue->messages[queue->readhead++];
	if (queue->readhead == queue->size)
		queue->readhead = 0;

	if (queue->writehead == queue->readhead) {
		queue->hdr.signalled = false;
	}

	nk_spinlock_release(&nk_lock, ipl);

	nk_semaphore_release(&queue->sem, 1);

	return 0;
}
