/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file krx_epoll.h
 * @brief Event poll.
 */

#ifndef ECX_SYS_KRX_EPOLL_H
#define ECX_SYS_KRX_EPOLL_H

#include <sys/epoll.h>
#include <sys/k_intr.h>

struct file;
struct proc;
struct poll_entry;

/* What kind of chpoll operation is being done. */
enum chpoll_mode {
	CHPOLL_POLL,
	CHPOLL_UNPOLL,
};

typedef struct pollhead {
	kspinlock_t lock;
	LIST_HEAD(, poll_entry) pollers;
} pollhead_t;

/*! @brief Initialise a pollhead. */
void pollhead_init(pollhead_t *ph);
/*! @brief Deliver events to waiters on a pollhead. */
void pollhead_deliver_events(pollhead_t *ph, int revents);
/*! @brief Register a poller with a pollhead. */
void pollhead_register(pollhead_t *ph, struct poll_entry *pe);
/*! @brief Deregister a poller with a pollhead. */
void pollhead_unregister(pollhead_t *ph, struct poll_entry *pe);

/*! @brief Called when a poll-watched file is closed. */
void poll_watched_file_did_close(struct file *file);

int sys_epoll_create(int flags);
int sys_epoll_ctl(int epdesc, int op, int desc, struct epoll_event *event);
int sys_epoll_wait(int epdesc, struct epoll_event *events, int maxevents,
    int millisecs);

#endif /* ECX_SYS_KRX_EPOLL_H */
