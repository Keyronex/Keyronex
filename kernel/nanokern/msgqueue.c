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
ke_msgq_post(kmsgqueue_t *msgq, void *msg)
{
	kwaitblock_queue_t queue = TAILQ_HEAD_INITIALIZER(queue);
	kwaitresult_t wait;
	ipl_t ipl;

	wait = ke_wait(&msgq->sem, "msgqueue_wait", false, false, -1);
	kassert(wait == kKernWaitStatusOK);

	ipl = ke_spinlock_acquire(&msgq->hdr.spinlock);

	msgq->messages[msgq->writehead++] = msg;
	if (msgq->writehead == msgq->size)
		msgq->writehead = 0;

	kassert(msgq->writehead != msgq->readhead);
	msgq->hdr.signalled = true;
	ki_signal(&msgq->hdr, &queue);
	ke_spinlock_release_nospl(&msgq->hdr.spinlock);

	ke_acquire_scheduler_lock();
	ki_wake_waiters(&queue);
	ke_release_scheduler_lock(ipl);
}

int
ke_msgq_read(kmsgqueue_t *msgq, void **msg)
{
	ipl_t ipl = ke_spinlock_acquire(&msgq->hdr.spinlock);

	if (msgq->writehead == msgq->readhead) {
		ke_spinlock_release(&msgq->hdr.spinlock, ipl);
		return -1;
	}

	*msg = msgq->messages[msgq->readhead++];

	if (msgq->readhead == msgq->size)
		msgq->readhead = 0;

	if (msgq->writehead == msgq->readhead)
		msgq->hdr.signalled = false;

	ke_spinlock_release(&msgq->hdr.spinlock, ipl);

	ke_semaphore_release(&msgq->sem, 1);

	return 0;
}
