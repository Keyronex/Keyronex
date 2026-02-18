/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file event.c
 * @brief Event kernel waitable object.
 */

#include <sys/k_wait.h>

void
ke_event_init(kevent_t *ev, bool signalled)
{
	kep_dispatcher_obj_init(&ev->header, signalled, SYNCH_EVENT);
}

void
ke_event_set_signalled(kevent_t *event, bool signalled)
{
	ipl_t ipl;
	struct kwaitblock_queue wakeq = TAILQ_HEAD_INITIALIZER(wakeq);

	ipl = ke_spinlock_enter(&event->header.lock);

	if (event->header.signalled == 0 && signalled == 1) {
		event->header.signalled = signalled;
		kep_signal(&event->header, &wakeq);
	} else {
		event->header.signalled = signalled;
	}

	ke_spinlock_exit_nospl(&event->header.lock);
	kep_waiters_wake(&wakeq);
	splx(ipl);
}
