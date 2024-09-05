/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Tue Sep 03 2024.
 */
/*!
 * @file epoll.c
 * @brief Executive Poll implementation.
 *
 * This is a simplistic implementation of epoll that only supports
 * "level-triggered" waits, doesn't support edge, exclusive, or oneshot waits,
 * and doesn't support watching another epoll.
 *
 * In the future it might be enhanced to support all these.
 *
 * Please see the locking notes around the struct epoll and in process_ready().
 */

#include <sys/epoll.h>
#include <sys/errno.h>

#include <kdk/executive.h>
#include <kdk/file.h>
#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include <kdk/object.h>
#include <kdk/poll.h>
#include <kdk/vfs.h>

#define LIST_REMOVE_AND_ZERO(elm, field) do {	\
	LIST_REMOVE(elm, field);		\
	(elm)->field.le_prev = NULL;		\
	(elm)->field.le_next = NULL;		\
} while (0)


#define LIST_ELEM_IS_INSERTED(elm, field) ((elm)->field.le_next != NULL)

static inline int
memcpy_from_user(void *dst, const void *src, size_t len)
{
	memcpy(dst, src, len);
	return 0;
}

static inline int
memcpy_to_user(void *dst, const void *src, size_t len)
{
	memcpy(dst, src, len);
	return 0;
}

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

	descnum_t desc;
	/*! A non-owning reference. */
	struct file *file;

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

static kmutex_t epoll_teardown_mutex = KMUTEX_INITIALIZER(epoll_teardown_mutex);
static struct vnode_ops epoll_vnops;

static inline struct epoll *
VTOEP(struct vnode *vnode)
{
	kassert(vnode->ops == &epoll_vnops);
	return (struct epoll *)vnode->fs_data;
}

static int
desc_to_epoll(eprocess_t *proc, descnum_t epdesc, struct epoll **out_ep,
    struct file **out_file)
{
	struct file *file;
	struct epoll *ep;

	file = ex_object_space_lookup(proc->objspace, epdesc);
	if (file == NULL)
		return -EBADF;

	ep = VTOEP(file->vnode);
	if (ep == NULL) {
		obj_release(file);
		return -EINVAL;
	}

	*out_ep = ep;
	*out_file = file;

	return 0;
}

static int
watch_add(struct epoll *ep, descnum_t desc, struct file *watch_file,
    struct epoll_event *pevent)
{
	struct poll_entry *entry;
	struct epoll_event event;
	int r;

	r = memcpy_from_user(&event, pevent, sizeof(event));
	if (r != 0)
		return r;

	ke_wait(&ep->mutex, "epoll_watch_add:ep->mutex", 0, 0, -1);

	LIST_FOREACH (entry, &ep->watches, epoll_watch_link) {
		if (entry->file == watch_file && entry->desc == desc) {
			ke_mutex_release(&ep->mutex);
			return -EEXIST;
		}
	}

	entry = kmem_alloc(sizeof(*entry));
	if (entry == NULL)
		return -ENOMEM;

	entry->epoll = ep;
	entry->desc = desc;
	entry->file = watch_file;
	entry->event = event;

	LIST_INSERT_HEAD(&ep->watches, entry, epoll_watch_link);

	r = watch_file->vnode->ops->chpoll(watch_file->vnode, entry,
	    kChpollPoll);
	/*
	 * could be done from chpoll if we export an appropriate API?
	 * maybe better here to avoid more complicated call stacks and simplify
	 * chpoll implementations.
	 */
	if (r & event.events) {
		ipl_t ipl = ke_spinlock_acquire(&ep->ready_lock);
		if (!LIST_ELEM_IS_INSERTED(entry, ready_link))
			LIST_INSERT_HEAD(&ep->ready, entry, ready_link);
		ke_event_signal(&ep->event);
		ke_spinlock_release(&ep->ready_lock, ipl);
	}

	ke_spinlock_acquire_nospl(&entry->file->epoll_lock);
	LIST_INSERT_HEAD(&watch_file->epoll_watches, entry, file_list_entry);
	ke_spinlock_release_nospl(&entry->file->epoll_lock);

	ke_mutex_release(&ep->mutex);

	return 0;
}

static void
watch_del(struct epoll *ep, struct poll_entry *entry)
{
	ipl_t ipl;

	entry->file->vnode->ops->chpoll(entry->file->vnode, entry,
	    kChpollRemove);

	ipl = spldpc();

	ke_spinlock_acquire_nospl(&entry->file->epoll_lock);
	kassert(LIST_ELEM_IS_INSERTED(entry, file_list_entry));
	LIST_REMOVE_AND_ZERO(entry, file_list_entry);
	ke_spinlock_release_nospl(&entry->file->epoll_lock);

	ke_spinlock_acquire_nospl(&ep->ready_lock);
	if (LIST_ELEM_IS_INSERTED(entry, ready_link))
		LIST_REMOVE_AND_ZERO(entry, ready_link);
	ke_spinlock_release_nospl(&ep->ready_lock);

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

	ipl = ke_spinlock_acquire(&ph->lock);
	LIST_FOREACH (entry, &ph->pollers, pollhead_entry) {
		struct epoll *ep;

		if ((entry->event.events & revents) == 0)
			continue;

		ep = entry->epoll;

		ke_spinlock_acquire_nospl(&ep->ready_lock);
		if (!LIST_ELEM_IS_INSERTED(entry, ready_link))
			LIST_INSERT_HEAD(&ep->ready, entry, ready_link);
		ke_event_signal(&ep->event);
		ke_spinlock_release_nospl(&ep->ready_lock);
	}
	ke_spinlock_release(&ph->lock, ipl);
}

void
pollhead_register(pollhead_t *ph, struct poll_entry *pe)
{
	ipl_t ipl = ke_spinlock_acquire(&ph->lock);
	LIST_INSERT_HEAD(&ph->pollers, pe, pollhead_entry);
	ke_spinlock_release(&ph->lock, ipl);
}

void
pollhead_unregister(pollhead_t *ph, struct poll_entry *pe)
{
	ipl_t ipl = ke_spinlock_acquire(&ph->lock);
	LIST_REMOVE_AND_ZERO(pe, pollhead_entry);
	ke_spinlock_release(&ph->lock, ipl);
}

void
epoll_delete(struct epoll *ep)
{
	struct poll_entry *entry, *tmp;

	ke_wait(&epoll_teardown_mutex, "epoll_delete:epoll_teardown_mutex", 0,
	    0, -1);

	LIST_FOREACH_SAFE (entry, &ep->watches, epoll_watch_link, tmp) {
		ipl_t ipl;

		entry->file->vnode->ops->chpoll(entry->file->vnode, entry,
		    kChpollRemove);

		ipl = spldpc();

		ke_spinlock_acquire_nospl(&entry->file->epoll_lock);
		kassert(LIST_ELEM_IS_INSERTED(entry, file_list_entry));
		LIST_REMOVE_AND_ZERO(entry, file_list_entry);
		ke_spinlock_release_nospl(&entry->file->epoll_lock);

		ke_spinlock_acquire_nospl(&ep->ready_lock);
		if (LIST_ELEM_IS_INSERTED(entry, ready_link))
			LIST_REMOVE_AND_ZERO(entry, ready_link);
		ke_spinlock_release_nospl(&ep->ready_lock);

		splx(ipl);

		LIST_REMOVE_AND_ZERO(entry, epoll_watch_link);

		kmem_free(entry, sizeof(struct poll_entry));
	}

	ke_mutex_release(&epoll_teardown_mutex);
}

void
poll_watched_file_did_close(file_t *file)
{
	ipl_t ipl;

	ke_wait(&epoll_teardown_mutex,
	    "epoll_watched_file_did_close:epoll_teardown_mutex", 0, 0, -1);

	ipl = ke_spinlock_acquire(&file->epoll_lock);
	while (!LIST_EMPTY(&file->epoll_watches)) {
		struct epoll *ep;
		struct poll_entry *entry;

		entry = LIST_FIRST(&file->epoll_watches);
		ke_spinlock_release(&file->epoll_lock, ipl);

		ep = entry->epoll;

		ke_wait(&ep->mutex, "epoll_watched_file_did_close:ep->mutex", 0,
		    0, -1);
		watch_del(ep, entry);
		ke_mutex_release(&ep->mutex);
	}
	ke_spinlock_release(&file->epoll_lock, ipl);
}

ex_err_ret_t
ex_service_epoll_ctl(eprocess_t *proc, descnum_t epdesc, int op, descnum_t desc,
    struct epoll_event *event)
{
	struct file *ep_file;
	struct epoll *ep;

	struct file *watch_file;
	int r;

	if (event->events &
	    ~(EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLPRI)) {
		kfatal("unexpected epoll flags\n");
		return -EINVAL;
	}

	r = desc_to_epoll(proc, epdesc, &ep, &ep_file);
	if (r != 0)
		return r;

	watch_file = ex_object_space_lookup(proc->objspace, desc);
	if (watch_file == NULL)
		return -EBADF;

	switch (op) {
	case EPOLL_CTL_ADD:
		r = watch_add(ep, desc, watch_file, event);
		obj_release(watch_file);
		break;

	case EPOLL_CTL_MOD:
	case EPOLL_CTL_DEL:
		kfatal("implement me!\n");
	}

	obj_release(ep_file);

	return r;
}

static int
process_ready(struct epoll *ep, int maxevents, struct epoll_event *revents)
{
	int nrevents = 0;
	int r;

	if (!LIST_EMPTY(&ep->ready)) {
		struct poll_entry *entry, *tmp;

		LIST_FOREACH_SAFE (entry, &ep->ready, ready_link, tmp) {
			if (nrevents == maxevents)
				break;

			/*
			 * Locking Note:
			 *
			 * It is here assumed that the chpol() operation does
			 * NOT acquire the pollhead lock of the watched object,
			 * if it does then because this function runs with the
			 * epoll::ready_lock held, it would be a lock order
			 * violation (the ready lock must be acquired AFTER
			 * pollhead locks).
			 *
			 * As chpoll() operations do presumably need to lock
			 * *something* in the watched object, it is also assumed
			 * that whatever lock that is, is NOT held when the
			 * object is signalling its pollhead.
			 *
			 */
			r = entry->file->vnode->ops->chpoll(entry->file->vnode,
			    NULL, kChpollPoll);
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
		ke_event_clear(&ep->event);

	return nrevents;
}

ex_size_ret_t
ex_service_epoll_wait(eprocess_t *proc, descnum_t epdesc,
    struct epoll_event *events, int maxevents, int millisecs)
{
	struct file *ep_file;
	struct epoll *ep;
	struct epoll_event revents[4];
	nanosecs_t timeout;
	ipl_t ipl;
	int r;

	if (maxevents > 4)
		maxevents = 4;
	else if (maxevents < 0)
		return -EINVAL;
	else if (maxevents == 0)
		kfatal("Todo: is this sensible?\n");

	r = desc_to_epoll(proc, epdesc, &ep, &ep_file);
	if (r != 0)
		return r;

	ipl = ke_spinlock_acquire(&ep->ready_lock);
	r = process_ready(ep, maxevents, revents);
	ke_spinlock_release(&ep->ready_lock, ipl);

	if (r != 0) {
		memcpy_to_user(events, revents, sizeof(struct epoll_event) * r);
		obj_release(ep_file);
		return r;
	}

	timeout = millisecs == -1 ? -1 : millisecs * NS_PER_MS;
	r = ke_wait(&ep->event, "epoll_wait:ep->event", 0, 0, timeout);
	if (r == kKernWaitStatusTimedOut) {
		obj_release(ep_file);
		return 0;
	}

	ipl = ke_spinlock_acquire(&ep->ready_lock);
	r = process_ready(ep, maxevents, revents);
	ke_spinlock_release(&ep->ready_lock, ipl);
	if (r != 0)
		memcpy_to_user(events, revents, sizeof(struct epoll_event) * r);

	obj_release(ep_file);
	return r;
}

ex_desc_ret_t
ex_service_epoll_create(eprocess_t *proc, int flags)
{
	descnum_t descnum;
	file_t *file;
	vnode_t *vnode;
	struct epoll *ep;

	ep = kmem_alloc(sizeof(struct epoll));
	if (ep == NULL)
		return -ENOMEM;

	file = ex_file_new();
	if (file == NULL) {
		kmem_free(ep, sizeof(struct epoll));
		return -ENOMEM;
	}

	vnode = vnode_new(NULL, VCHR, &epoll_vnops, NULL, NULL, (uintptr_t)ep);
	if (vnode == NULL) {
		kmem_free(ep, sizeof(struct epoll));
		obj_release(file);
		return -ENOMEM;
	}

	descnum = ex_object_space_reserve(proc->objspace, false);
	if (descnum == DESCNUM_NULL) {
		kmem_free(ep, sizeof(struct epoll));
		obj_release(file);
		vn_release(vnode);
		return -EMFILE;
	}

	file->vnode = vnode;

	ke_mutex_init(&ep->mutex);
	ke_spinlock_init(&ep->ready_lock);
	LIST_INIT(&ep->watches);
	LIST_INIT(&ep->ready);
	ke_event_init(&ep->event, false);
	ep->deleted = false;

	ex_object_space_reserved_insert(proc->objspace, descnum, file);

	return descnum;
}
