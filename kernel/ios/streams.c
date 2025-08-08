/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Thu Feb 20 2025.
 */
/*!
 * @file streams.c
 * @brief STREAMS implementation.
 */

#include <kdk/kern.h>
#include <kdk/queue.h>
#include <kdk/stream.h>
#include <kdk/vm.h>
#include <stdint.h>

#include "ios/str_impl.h"

struct sd_reader_entry {
	TAILQ_ENTRY(sd_reader_entry) tqentry;
	kevent_t event;
};

int
str_readlock_thread(stdata_t *stp, ipl_t *pipl)
{
	ipl_t ipl;
	struct sd_reader_entry ll;

	ipl = ke_spinlock_acquire(&stp->sd_lock);

	if (!stp->sd_readlocked) {
		stp->sd_readlocked = true;
		*pipl = ipl;
		return true;
	}

	ke_event_init(&ll.event, false);
	TAILQ_INSERT_TAIL(&stp->sd_read_threads, &ll, tqentry);
	ke_spinlock_release(&stp->sd_lock, ipl);

	ke_wait(&ll.event, "socklock_lock_thread", 0, 0, -1);

	ipl = ke_spinlock_acquire(&stp->sd_lock);
	kassert_dbg(stp->sd_readlocked);

	return 0;
}

void
str_readunlock(stdata_t *stp, ipl_t ipl)
{
	kassert_dbg(ke_spinlock_held(&stp->sd_lock));

	if (!TAILQ_EMPTY(&stp->sd_read_threads)) {
		struct sd_reader_entry *ll = TAILQ_FIRST(&stp->sd_read_threads);
		TAILQ_REMOVE(&stp->sd_read_threads, ll, tqentry);
		ke_event_signal(&ll->event);
	} else if (!TAILQ_EMPTY(&stp->sd_read_iops)) {
		kfatal("socklock_release: resume waiting IOP\n");
	} else {
		stp->sd_readlocked = false;
		ke_spinlock_release(&stp->sd_lock, ipl);
	}
}
