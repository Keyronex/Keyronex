/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Jan 14 2023.
 */

#include "kdk/kmem.h"
#include "kdk/nanokern.h"
#include "kdk/queue.h"
#include "ki.h"

void
ke_event_init(kevent_t *ev, bool signalled)
{
	ev->hdr.type = kDispatchEvent;
	ev->hdr.signalled = signalled;
	ke_spinlock_init(&ev->hdr.spinlock);
	TAILQ_INIT(&ev->hdr.waitblock_queue);
}

void
ke_event_signal(kevent_t *ev)
{
	kwaitblock_queue_t queue = TAILQ_HEAD_INITIALIZER(queue);
	ipl_t ipl = ke_spinlock_acquire(&ev->hdr.spinlock);
	ev->hdr.signalled = 1;
	ki_signal(&ev->hdr, &queue);
	ke_spinlock_release_nospl(&ev->hdr.spinlock);

	ke_acquire_scheduler_lock();
	ki_wake_waiters(&queue);
	ke_release_scheduler_lock(ipl);
}

bool
ke_event_clear(kevent_t *ev)
{
	ipl_t ipl = ke_spinlock_acquire(&ev->hdr.spinlock);
	bool signalled;
	signalled = ev->hdr.signalled;
	ev->hdr.signalled = false;
	ke_spinlock_release(&ev->hdr.spinlock, ipl);
	return signalled;
}

#if 0
void
ke_msgqueue_init(kmsgqueue_t *msgq, unsigned count)
{
	msgq->hdr.type = kDispatchMsgQueue;
	msgq->hdr.signalled = 0;
	TAILQ_INIT(&msgq->hdr.waitblock_queue);
	msgq->size = count;
	msgq->readhead = 0;
	msgq->writehead = 0;
	msgq->messages = kmem_alloc(sizeof(void *) * count);
	ke_semaphore_init(&msgq->sem, count);
}

void
ke_msgq_post(kmsgqueue_t *queue, void *msg)
{
	kwaitresult_t wait = ke_wait(&queue->sem, "msgqueue_wait", false, false,
	    -1);
	kassert(wait == kKernWaitStatusOK);

	ipl_t ipl = ke_acquire_dispatcher_lock();

	queue->messages[queue->writehead++] = msg;
	if (queue->writehead == queue->size)
		queue->writehead = 0;

	kassert(queue->writehead != queue->readhead);
	queue->hdr.signalled = true;

	kwaitblock_t *block = TAILQ_FIRST(&queue->hdr.waitblock_queue);
	while ((block) != NULL) {
		kwaitblock_t *next = TAILQ_NEXT(block, queue_entry);
		if (ki_waiter_maybe_wakeup(block->thread, &queue->hdr))
			break;
		block = next;
	}

	ke_release_dispatcher_lock(ipl);
}

int
ke_msgq_read(kmsgqueue_t *queue, void **msg)
{
	ipl_t ipl = ke_acquire_dispatcher_lock();

	if (queue->writehead == queue->readhead) {
		ke_release_dispatcher_lock(ipl);
		return -1;
	}

	*msg = queue->messages[queue->readhead++];
	if (queue->readhead == queue->size)
		queue->readhead = 0;

	if (queue->writehead == queue->readhead) {
		queue->hdr.signalled = false;
	}

	ke_release_dispatcher_lock(ipl);

	ke_semaphore_release(&queue->sem, 1);

	return 0;
}
#endif
