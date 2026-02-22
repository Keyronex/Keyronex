/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file strsubr.h
 * @brief STREAMS implementation.
 */

#ifndef ECX_SYS_STRSUBR_H
#define ECX_SYS_STRSUBR_H

#include <sys/k_thread.h>
#include <sys/k_wait.h>
#include <sys/stropts.h>
#include <sys/stream.h>

struct req_waiter {
	TAILQ_ENTRY(req_waiter) link;
	kevent_t event;
};

enum {
	ST_FROZEN = (1 << 0),	/* stream is frozen */
	ST_QUEUED = (1 << 1),	/* on a CPU runq */
	ST_RUNNING = (1 << 2),	/* worker currently servicing */
	ST_NEEDRUN = (1 << 3),	/* some queue enabled while we can't run */
	ST_DEAD = (1 << 4),	/* stream is being closed */
};

enum str_head_kind {
	STR_HEAD_KIND_NONE,
	STR_HEAD_KIND_TTY,
	STR_HEAD_KIND_RPIPE,
	STR_HEAD_KIND_WPIPE,
	STR_HEAD_KIND_FIFO,
};

typedef struct stdata {
	kmutex_t integral_mutex;
	kmutex_t *mutex;

	struct streamtab *devtab;
	enum str_head_kind kind;

	bool req_locked;
	TAILQ_HEAD(, req_waiter) req_waiters;

	kspinlock_t ingress_lock;
	mblk_q_t ingress_head;

	atomic_uint flags; /* ST_FROZEN, ... */
	kcpunum_t home_cpu;

	queue_t *rq;
	queue_t *wq;
	queue_t *rq_bottom;

	TAILQ_ENTRY(stdata) sched_link;

	/* stream head personality */
	pollhead_t pollhead;
	bool hanged_up;
	kevent_t data_readable;
	enum str_read_mode read_mode;

	kevent_t ioctl_done_ev;

	union {
		struct {
			struct pgrp *tty_pgrp; /* foreground process group */
			struct session *tty_session; /* controlling session */
		};
		struct {
			struct stdata *pipe_peer;
			bool write_broken;
		};
	};
} stdata_t;

stdata_t *stropen(struct streamtab *devtab, void *dev);
int strpush(stdata_t *sh, struct streamtab *tab);
int strread(stdata_t *, void *buf, size_t len, int options);
int strwrite(stdata_t *, const void *buf, size_t len, int options);
int strioctl(vnode_t *, stdata_t *, unsigned long cmd, void *arg);
int strchpoll(stdata_t *, struct poll_entry *, enum chpoll_mode);

#endif /* ECX_SYS_STRSUBR_H */
