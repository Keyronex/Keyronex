/*!
 * @file executive/epoll.c
 * @brief Event poll objects.
 *
 * Locking orders being complicated, this is not currently the most efficiently
 * implemented thing.
 * There is an epoll->lock and there is a polllist->lock, acquired in that
 * order. To eliminate the need for any tricks, an event is raised simply by
 * signalling an event object in the epoll. More complicated schemes might be
 * necessary for nested epolls and suchlike.
 *
 */

#include <sys/select.h>

#include "epoll.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vfs.h"

#define LOCK_REQUIRES(lock)
#define LOCK_RELEASE(lock)

/*!
 * (l) => epoll::lock
 */
struct epoll {
	/*! mutex */
	kspinlock_t lock;

	/*! is it waiting? */
	bool waiting;

	/*! list of all epoll watches */
	LIST_HEAD(, pollhead) watches;

	/*! event to signal when a watch completes */
	kevent_t ev;
};

static struct vnops epoll_vnops;

/* borrowed from mlibc */
void
__FD_CLR(int fd, fd_set *set)
{
	kassert(fd < FD_SETSIZE);
	set->__mlibc_elems[fd / 8] &= ~(1 << (fd % 8));
}
int
__FD_ISSET(int fd, fd_set *set)
{
	kassert(fd < FD_SETSIZE);
	return set->__mlibc_elems[fd / 8] & (1 << (fd % 8));
}
void
__FD_SET(int fd, fd_set *set)
{
	kassert(fd < FD_SETSIZE);
	set->__mlibc_elems[fd / 8] |= 1 << (fd % 8);
}
void
__FD_ZERO(fd_set *set)
{
	memset(set->__mlibc_elems, 0, sizeof(fd_set));
}

struct epoll *
epoll_do_new(void)
{
	struct epoll *epoll = kmem_alloc(sizeof(*epoll));
	epoll->waiting = false;
	ke_spinlock_init(&epoll->lock);
	ke_event_init(&epoll->ev, false);
	LIST_INIT(&epoll->watches);
	return epoll;
}

static int
epoll_insert(struct epoll *epoll, struct pollhead *watch)
{
	LIST_INSERT_HEAD(&epoll->watches, watch, watch_link);
	/* todo: if poll is live, register pollhead? */
	return 0;
}

/*!
 * Destroy an epoll. unlocks the mutex and destroys the epoll altgoether.
 * no reference to the epoll should remain, because it gets unlocked and then
 * destroyed.
 */
void
epoll_do_destroy(struct epoll *epoll)
{
	struct pollhead *watch, *tmp;
	ipl_t ipl;

	/* todo: make sure poll isn't live! if it is, do the needful */

	ipl = ke_spinlock_acquire(&epoll->lock);

	LIST_FOREACH_SAFE (watch, &epoll->watches, watch_link, tmp) {
		/*
		 * One solution to the locking problem:
		 *
		 * We could set a "watch being destroyed" flag in the watch
		 * (and in this case also on the epoll itself) and spin on the
		 * object's polllist lock. If the object sees that flag, it
		 * backs off and moves on to the next task.
		 */
		if (watch->live)
			VOP_CHPOLL(watch->vnode, watch, kChPollRemove);
		kmem_free(watch, sizeof(*watch));
	}

	ke_spinlock_release(&epoll->lock, ipl);
	kmem_free(epoll, sizeof(*epoll));
}

/*!
 * do a poll
 * @param nanosecs -1 for infinite wait, 0 for immediate (only polls), positive
 * for a timeout
 */
int
poll_wait_locked(struct epoll *epoll, struct epoll_event *events, int nevents,
    nanosecs_t nanosecs, ipl_t ipl) LOCK_RELEASE(epoll->lock)
{
	struct pollhead *watch;
	int r = 0;

	if (nevents == 0)
		goto wait;

	if (epoll->waiting)
		kfatal("Multiply waiting on epoll not yet implemented.");

	epoll->waiting = true;

	/* for proper epoll support, this shouldn't happen*/
	ke_event_clear(&epoll->ev);

	LIST_FOREACH (watch, &epoll->watches, watch_link) {
		/*
		 * NOTE FOR PROPER EPOLL SUPPORT:
		 * Check if watch is already live
		 */
		r = VOP_CHPOLL(watch->vnode, watch, kChPollAdd);
		if (r != 0) {
			ke_spinlock_release(&epoll->lock, ipl);
			goto process;
		}
		watch->live = true;
	}

wait:
	ke_spinlock_release(&epoll->lock, ipl);

	kwaitstatus_t ws = ke_wait(&epoll->ev, "poll_wait_locked: epoll->ev",
	    true, true, nanosecs);
	kassert(ws == kKernWaitStatusOK || ws == kKernWaitStatusTimedOut);

process:
	ipl = ke_spinlock_acquire(&epoll->lock);
	r = 0;

	if (nevents == 0)
		goto finish;

	LIST_FOREACH (watch, &epoll->watches, watch_link) {
		if (r != nevents && watch->revents != 0) {
			/* revents must be a subset of watch events */
			kassert((watch->event.events | watch->revents) ==
			    watch->event.events);

			events[r].data.u64 = watch->event.data.u64;
			events[r].events = watch->revents;

			/*
			 * NOTE FOR PROPER EPOLL SUPPORT:
			 * We shouldn't set revents to 0.
			 */
			watch->revents = 0;

			/* increment r for each event we get */
			r++;
		}
		if (watch->live) {
			watch->live = false;
			VOP_CHPOLL(watch->vnode, watch, kChPollRemove);
		}
	}

	epoll->waiting = false;

finish:
	ke_spinlock_release(&epoll->lock, ipl);

	return r;
}

int
epoll_do_add(struct epoll *epoll, struct file *file, vnode_t *vnode,
    struct epoll_event *event)
{
	struct pollhead *watch;
	ipl_t ipl;
	int r;

	watch = kmem_alloc(sizeof(*watch));
	watch->file = file;
	watch->vnode = vnode;
	watch->event = *event;
	watch->epoll = epoll;
	watch->live = false;
	watch->revents = 0;

	ipl = ke_spinlock_acquire(&epoll->lock);
	r = epoll_insert(epoll, watch);
	ke_spinlock_release(&epoll->lock, ipl);

	return r;
}

int
epoll_do_ctl(struct epoll *epoll, int op, int fd, struct epoll_event *event)
{
	switch (op) {
	case EPOLL_CTL_ADD: {
		struct file *file;

		file = ps_getfile(ps_curproc(), fd);
		kassert(file != NULL);

		return epoll_do_add(epoll, file, file->vn, event);

	default:
		kfatal("Unimplemented\n");
	}
	}
}

int
epoll_do_wait(struct epoll *epoll, struct epoll_event *events, int nevents,
    nanosecs_t nanosecs)
{
	ipl_t ipl = ke_spinlock_acquire(&epoll->lock);
	return poll_wait_locked(epoll, events, nevents, nanosecs, ipl);
}

int
pollhead_raise(struct pollhead *ph, int events)
{
	if (ph->event.events & events) {
		ph->revents = ph->event.events & events;
		ke_event_signal(&ph->epoll->ev);
	}
	return 0;
}

vnode_t *
epoll_new(void)
{
	struct epoll *epoll = epoll_do_new();
	return devfs_create_unnamed(epoll, &epoll_vnops);
}

struct epoll *
epoll_from_vnode(vnode_t *vn)
{
	if (vn->rdeviceops != &epoll_vnops) {
		kfatal("Funny rdeviceops\n");
		return NULL;
	}
	return (struct epoll *)vn->rdevice;
}

static struct vnops epoll_vnops = {};