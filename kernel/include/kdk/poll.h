/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Tue Sep 03 2024.
 */

#ifndef KRX_KDK_POLL_H
#define KRX_KDK_POLL_H

#include <sys/epoll.h>

#include <kdk/executive.h>
#include <kdk/vfs.h>

struct file;

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

ex_desc_ret_t ex_service_epoll_create(eprocess_t *proc, int flags);
ex_err_ret_t ex_service_epoll_ctl(eprocess_t *proc, descnum_t epdesc, int op,
    descnum_t desc, struct epoll_event *event);
ex_size_ret_t ex_service_epoll_wait(eprocess_t *proc, descnum_t epdesc,
    struct epoll_event *events, int maxevents, int millisecs);

#endif /* KRX_KDK_POLL_H */
