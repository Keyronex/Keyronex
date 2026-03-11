/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file epoll.c
 * @brief Event poll
 *
 * TODO:
 * - Convert to a device.
 * - Less spinlock?
 */

#include <sys/epoll.h>
#include <sys/errno.h>
#include <sys/k_thread.h>
#include <sys/krx_epoll.h>
#include <sys/krx_file.h>
#include <sys/krx_user.h>
#include <sys/libkern.h>
#include <sys/vnode.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include "sys/k_intr.h"

#define LIST_ENTRY_INIT(elm, field) do {	\
	(elm)->field.le_prev = NULL;		\
	(elm)->field.le_next = NULL;		\
} while (0)
#define LIST_ELEM_IS_INSERTED(elm, field) ((elm)->field.le_prev != NULL)
#define LIST_REMOVE_AND_ZERO(elm, field) do {	\
	LIST_REMOVE(elm, field);		\
	(elm)->field.le_prev = NULL;		\
	(elm)->field.le_next = NULL;		\
} while (0)

/*!
 * An epoll entry.
 * These are linked on to epoll::watches, to file::watches, and to the watched
 * object's pollhead.
 */
struct poll_entry {
	/*! owning epoll */
	struct epoll *epoll;

	LIST_ENTRY(poll_entry) epoll_watch_link;

	/*! links the entry to te epoll::ready list */
	LIST_ENTRY(poll_entry) ready_link;

	/*! links the entry to the file_t::epoll_watches */
	LIST_ENTRY(poll_entry) file_list_entry;

	/*! links the entry onto the waited object's pollhead */
	LIST_ENTRY(poll_entry) pollhead_entry;

	int desc;

	/*! A non-owning reference. */
	struct file *file;

	uint32_t generation;
	struct epoll_event event;
};

/*!
 * An epoll instance.
 *
 * The mutex guards the watches list and generally everything other than ready
 * list access and event setting.
 *
 * The ready_lock guards the ready list and event setting. It is separate
 * because a different locking protocol is needed for add/remove/modify watch
 * operations as opposed to signalling operations, viz.:
 *
 * - add/remove/modify: lock epoll -> lock pollhead of watched object
 * - signal: lock pollhead of object that became ready -> lock epoll
 *
 * As such it is necessary that the ready list be locked separately from the
 * rest of the data. The general lock ordering is then:
 *
 * - epoll::mutex -> pollhead::lock -> epoll::ready_lock
 *
 * The epoll_teardown_mutex synchronises teardown of epoll instances with the
 * automatic removal of watches of watched files that have been closed
 * elsewhere.
 *
 */
struct epoll {
	kmutex_t mutex;
	kspinlock_t ready_lock;
	LIST_HEAD(, poll_entry) watches;
	LIST_HEAD(, poll_entry) ready;
	kevent_t event;
	bool deleted;
};

static kmutex_t epoll_teardown_mutex;
static struct vnode_ops epoll_vnops;

static inline struct epoll *
VTOEP(struct vnode *vnode)
{
	kassert(vnode->ops == &epoll_vnops);
	return (struct epoll *)vnode->fsprivate_1;
}

static int
desc_to_epoll(int epdesc, struct epoll **out_ep, struct file **out_file)
{
	struct file *file;
	struct epoll *ep;

	file = uf_lookup(curproc()->finfo, epdesc);
	if (file == NULL)
		return -EBADF;

	ep = VTOEP(file->vnode);
	if (ep == NULL) {
		file_release(file);
		return -EINVAL;
	}

	*out_ep = ep;
	*out_file = file;

	return 0;
}

static int
watch_add(struct epoll *ep, int desc, struct file *watch_file,
    struct epoll_event *pevent)
{
	struct poll_entry *entry;
	struct epoll_event event;
	int r;

	r = memcpy_from_user(&event, pevent, sizeof(event));
	if (r != 0)
		return r;

	ke_mutex_enter(&ep->mutex, "epoll_watch_add:ep->mutex");

	LIST_FOREACH(entry, &ep->watches, epoll_watch_link) {
		if (entry->file == watch_file && entry->desc == desc) {
			ke_mutex_exit(&ep->mutex);
			return -EEXIST;
		}
	}

	entry = kmem_alloc(sizeof(*entry));
	if (entry == NULL) {
		ke_mutex_exit(&ep->mutex);
		return -ENOMEM;
	}

	entry->epoll = ep;
	entry->desc = desc;
	entry->file = watch_file;
	entry->event = event;
	LIST_ENTRY_INIT(entry, ready_link);

	LIST_INSERT_HEAD(&ep->watches, entry, epoll_watch_link);

	kassert(entry->file->vnode->ops->chpoll != NULL);
	r =  VOP_CHPOLL(watch_file->vnode, entry, CHPOLL_POLL);

	/*
	 * could be done from chpoll if we export an appropriate API?
	 * maybe better here to avoid more complicated call stacks and simplify
	 * chpoll implementations.
	 */
	if (r & event.events) {
		ipl_t ipl = ke_spinlock_enter(&ep->ready_lock);
		if (!LIST_ELEM_IS_INSERTED(entry, ready_link))
			LIST_INSERT_HEAD(&ep->ready, entry, ready_link);
		ke_event_set_signalled(&ep->event, true);
		ke_spinlock_exit(&ep->ready_lock, ipl);
	}

	ke_spinlock_enter_nospl(&entry->file->epoll_lock);
	LIST_INSERT_HEAD(&watch_file->epoll_watches, entry, file_list_entry);
	ke_spinlock_exit_nospl(&entry->file->epoll_lock);

	ke_mutex_exit(&ep->mutex);

	return 0;
}

static void
watch_del(struct epoll *ep, struct poll_entry *entry)
{
	ipl_t ipl;

	entry->file->vnode->ops->chpoll(entry->file->vnode, entry,
	    CHPOLL_UNPOLL);

	ipl = spldisp();

	ke_spinlock_enter_nospl(&entry->file->epoll_lock);
	kassert(LIST_ELEM_IS_INSERTED(entry, file_list_entry));
	LIST_REMOVE_AND_ZERO(entry, file_list_entry);
	ke_spinlock_exit_nospl(&entry->file->epoll_lock);

	ke_spinlock_enter_nospl(&ep->ready_lock);
	if (LIST_ELEM_IS_INSERTED(entry, ready_link))
		LIST_REMOVE_AND_ZERO(entry, ready_link);
	ke_spinlock_exit_nospl(&ep->ready_lock);

	splx(ipl);

	LIST_REMOVE_AND_ZERO(entry, epoll_watch_link);

	kmem_free(entry, sizeof(struct poll_entry));
}

void
pollhead_init(pollhead_t *ph)
{
	ke_spinlock_init(&ph->lock);
	LIST_INIT(&ph->pollers);
}

void
pollhead_deliver_events(pollhead_t *ph, int revents)
{
	struct poll_entry *entry;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&ph->lock);
	LIST_FOREACH(entry, &ph->pollers, pollhead_entry) {
		struct epoll *ep;

		if ((entry->event.events & revents) == 0)
			continue;

		ep = entry->epoll;

		ke_spinlock_enter_nospl(&ep->ready_lock);
		entry->generation++;
		if (!LIST_ELEM_IS_INSERTED(entry, ready_link))
			LIST_INSERT_HEAD(&ep->ready, entry, ready_link);
		ke_event_set_signalled(&ep->event, true);
		ke_spinlock_exit_nospl(&ep->ready_lock);
	}
	ke_spinlock_exit(&ph->lock, ipl);
}

void
pollhead_register(pollhead_t *ph, struct poll_entry *pe)
{
	ipl_t ipl = ke_spinlock_enter(&ph->lock);
	LIST_INSERT_HEAD(&ph->pollers, pe, pollhead_entry);
	ke_spinlock_exit(&ph->lock, ipl);
}

void
pollhead_unregister(pollhead_t *ph, struct poll_entry *pe)
{
	ipl_t ipl = ke_spinlock_enter(&ph->lock);
	LIST_REMOVE_AND_ZERO(pe, pollhead_entry);
	ke_spinlock_exit(&ph->lock, ipl);
}

void
epoll_delete(struct epoll *ep)
{
	struct poll_entry *entry, *tmp;

	ke_mutex_enter(&epoll_teardown_mutex,
	    "epoll_delete:epoll_teardown_mutex");

	LIST_FOREACH_SAFE(entry, &ep->watches, epoll_watch_link, tmp) {
		ipl_t ipl;

		entry->file->vnode->ops->chpoll(entry->file->vnode, entry,
		    CHPOLL_UNPOLL);

		ipl = spldisp();

		ke_spinlock_enter_nospl(&entry->file->epoll_lock);
		kassert(LIST_ELEM_IS_INSERTED(entry, file_list_entry));
		LIST_REMOVE_AND_ZERO(entry, file_list_entry);
		ke_spinlock_exit_nospl(&entry->file->epoll_lock);

		ke_spinlock_enter_nospl(&ep->ready_lock);
		if (LIST_ELEM_IS_INSERTED(entry, ready_link))
			LIST_REMOVE_AND_ZERO(entry, ready_link);
		ke_spinlock_exit_nospl(&ep->ready_lock);

		splx(ipl);

		LIST_REMOVE_AND_ZERO(entry, epoll_watch_link);

		kmem_free(entry, sizeof(struct poll_entry));
	}

	ke_mutex_exit(&epoll_teardown_mutex);
}

void
poll_watched_file_did_close(file_t *file)
{
	ipl_t ipl;

	ke_mutex_enter(&epoll_teardown_mutex,
	    "epoll_watched_file_did_close:epoll_teardown_mutex");


	for  (!LIST_EMPTY(&file->epoll_watches)) {
		struct epoll *ep;
		struct poll_entry *entry;

		ipl = ke_spinlock_enter(&file->epoll_lock);

		if (LIST_EMPTY(&file->epoll_watches)) {
			ke_spinlock_exit(&file->epoll_lock, ipl);
			break;
		}

		entry = LIST_FIRST(&file->epoll_watches);
		ke_spinlock_exit(&file->epoll_lock, ipl);

		ep = entry->epoll;

		ke_mutex_enter(&ep->mutex,
		    "epoll_watched_file_did_close:ep->mutex");
		watch_del(ep, entry);
		ke_mutex_exit(&ep->mutex);
	}

	ke_mutex_exit(&epoll_teardown_mutex);


}

int
sys_epoll_ctl(int epdesc, int op, int desc,
    struct epoll_event *event)
{
	struct file *ep_file;
	struct epoll *ep;

	struct file *watch_file;
	int r;

	if (event != NULL &&
	    event->events &
		~(EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLPRI)) {
		kfatal("unexpected epoll flags\n");
		return -EINVAL;
	}

	r = desc_to_epoll(epdesc, &ep, &ep_file);
	if (r != 0)
		return r;

	watch_file = uf_lookup(curproc()->finfo, desc);
	if (watch_file == NULL) {
		file_release(ep_file);
		return -EBADF;
	}

	switch (op) {
	case EPOLL_CTL_ADD:
		r = watch_add(ep, desc, watch_file, event);
		file_release(watch_file);
		break;

	case EPOLL_CTL_MOD:
		kfatal("implement me!\n");

	case EPOLL_CTL_DEL: {
		struct poll_entry *entry, *found = NULL;

		ke_mutex_enter(&ep->mutex, "epoll_ctl_del:ep->mutex");

		LIST_FOREACH(entry, &ep->watches, epoll_watch_link) {
			if (entry->file == watch_file && entry->desc == desc) {
				found = entry;
				break;
			}
		}

		if (found == NULL) {
			ke_mutex_exit(&ep->mutex);
			r = -ENOENT;
		} else {
			watch_del(ep, found);
			ke_mutex_exit(&ep->mutex);
			r = 0;
		}

		file_release(watch_file);
		break;
	}
	}

	file_release(ep_file);

	return r;
}

static int
process_ready(struct epoll *ep, int maxevents, struct epoll_event *revents)
{
	int nrevents = 0;
	int r;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&ep->ready_lock);

	if (!LIST_EMPTY(&ep->ready)) {
		struct poll_entry *entry, *tmp;

		LIST_FOREACH_SAFE(entry, &ep->ready, ready_link, tmp) {
			if (nrevents == maxevents)
				break;

			do {
				uint32_t gen = entry->generation;
				ke_spinlock_exit(&ep->ready_lock, ipl);

				r = VOP_CHPOLL(entry->file->vnode, NULL,
				    CHPOLL_POLL);

				ipl = ke_spinlock_enter(&ep->ready_lock);
				if ((r & entry->event.events) != 0 ||
				    entry->generation == gen)
					break;
			} while (true);

			if ((r & entry->event.events) != 0) {
				revents[nrevents] = entry->event;
				revents[nrevents++].events = r &
				    entry->event.events;
			} else {
				LIST_REMOVE_AND_ZERO(entry, ready_link);
			}
		}
	}

	if (LIST_EMPTY(&ep->ready))
		ke_event_set_signalled(&ep->event, false);

	ke_spinlock_exit(&ep->ready_lock, ipl);

	return nrevents;
}

int
sys_epoll_wait(int epdesc, struct epoll_event *events,
    int maxevents, int millisecs)
{
	struct file *ep_file;
	struct epoll *ep;
	struct epoll_event revents[4];
	knanosecs_t deadline;
	int r;

	if (maxevents > 4)
		maxevents = 4;
	else if (maxevents < 0)
		return -EINVAL;
	else if (maxevents == 0)
		kfatal("Todo: is this sensible?\n");

	r = desc_to_epoll(epdesc, &ep, &ep_file);
	if (r != 0)
		return r;

	ke_mutex_enter(&ep->mutex, "epoll_wait:ep->mutex 1");
	r = process_ready(ep, maxevents, revents);
	ke_mutex_exit(&ep->mutex);

	if (r != 0) {
		memcpy_to_user(events, revents, sizeof(struct epoll_event) * r);
		file_release(ep_file);
		return r;
	}

	if (millisecs == -1)
		deadline = ABSTIME_FOREVER;
	else
		deadline = ke_time() + (knanosecs_t)millisecs * NS_PER_MS;

	r = ke_wait1(&ep->event, "epoll_wait:ep->event", false, deadline);
	if (r == -ETIMEDOUT) {
		file_release(ep_file);
		return 0;
	}

	ke_mutex_enter(&ep->mutex, "epoll_wait:ep->mutex 2");
	r = process_ready(ep, maxevents, revents);
	ke_mutex_exit(&ep->mutex);
	if (r != 0)
		memcpy_to_user(events, revents, sizeof(struct epoll_event) * r);

	file_release(ep_file);

	return r;
}

int
sys_epoll_create(int flags)
{
	int r;
	file_t *file;
	vnode_t *vnode;
	struct epoll *ep;

	ep = kmem_alloc(sizeof(struct epoll));
	if (ep == NULL)
		return -ENOMEM;

	vnode = vn_alloc(NULL, VCHR, &epoll_vnops, (uintptr_t)ep, 0);
	if (vnode == NULL) {
		kmem_free(ep, sizeof(struct epoll));
		return -ENOMEM;
	}

	/* fixme: flags */
	file = file_new((namecache_handle_t) { NULL, NULL }, vnode, 0);
	if (file == NULL) {
		kmem_free(ep, sizeof(struct epoll));
		vn_release(vnode);
		return -ENOMEM;
	}

	file->nch = (namecache_handle_t) { NULL, NULL };
	file->vnode = vnode;

	ke_mutex_init(&ep->mutex);
	ke_spinlock_init(&ep->ready_lock);
	LIST_INIT(&ep->watches);
	LIST_INIT(&ep->ready);
	ke_event_init(&ep->event, false);
	ep->deleted = false;

	r = uf_reserve_fd(curproc()->finfo, 0,
	    (flags & EPOLL_CLOEXEC) ? O_CLOEXEC : 0);
	if (r < 0)
		file_release(file);
	else
		uf_install_reserved(curproc()->finfo, r, file);

	return r;
}

static int
epoll_inactive(vnode_t *vn)
{
	struct epoll *ep = VTOEP(vn);
	epoll_delete(ep);
	return 0;
}

static struct vnode_ops epoll_vnops = {
	.inactive = epoll_inactive,
};
