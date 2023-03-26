#include <sys/select.h>

#include "epoll.h"
#include "kdk/kmem.h"
#include "kdk/process.h"
#include "kdk/libkern.h"

#define LOCK_REQUIRES(lock)
#define LOCK_RELEASE(lock)

/*!
 * (l) => epoll::lock
 */
struct epoll {
	/*! mutex */
	kspinlock_t lock;

	/*! (l) list of epoll watches */
	LIST_HEAD(, epollhead) watches;

	/*! (l) event to signal to wake up the poller */
	kevent_t ev;
};

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
epoll_new(void)
{
	struct epoll *epoll = kmem_alloc(sizeof(*epoll));
	ke_spinlock_init(&epoll->lock);
	LIST_INIT(&epoll->watches);
	return epoll;
}

/*!
 * Add a watch to an epoll.
 */
int
epoll_add(struct epoll *epoll, struct epollhead *watch)
{
	LIST_INSERT_HEAD(&epoll->watches, watch, watch_link);
	/* todo: if poll is live, register pollhead? */
	return 0;
}

#if 0
void epoll_del(struct epoll *epoll, int fd) {
	struct epollhead *watch;

		kwaitstatus_t ret = ke_wait(&epoll->mutex,
	    "epoll_del: acquire epoll->mutex", false, false, 0);
	kassert(ret == kKernWaitStatusOK);

	LIST_FOREACH(watch, )
}
#endif

/*!
 * destroy a locked epoll. unlocks the mutex and destroys the epoll altgoether.
 * no reference to the epoll should remain, because it gets unlocked and then
 * destroyed.
 */
void
epoll_destroy(struct epoll *epoll, ipl_t ipl) LOCK_REQUIRES(epoll->lock)
{
	struct epollhead *watch;
	/* todo: make sure poll isn't live! if it is, do the needful */

	LIST_FOREACH (watch, &epoll->watches, watch_link) {
		/* one solution to the locking problem
		 * when we are in the middle of trying to remove something from
		 * an object's polllist but the object is trying to lock us so
		 * it can wake us
		 *
		 * we can trylock the watch's object's lock on its watch list.
		 * if we fail to acquire the lock, we unlock everything and
		 * retry later?? is this reasonable to do? i think it is
		 *
		 * alternatively can the object do a trylock? i fear that's
		 * non-viable because of the lengthy delay that might entail.
		 * plus the object is probably locked in its own way.
		 *
		 * my best solution so far: we can instead perhaps set a "watch
		 * being destroyed" flag in the watch, (and in this case also on
		 * the epoll itself??) and spin on the object's polllist lock.
		 * if the object sees that flag, it backs off and moves on to
		 * the next task. (!!! i raelly think this can actually work!)
		 */
		// VOP_POLL(watch->file->vn, watch, kChPollChange);
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
	struct epollhead *watch;
	int r = 0;

	if (nevents == 0)
		goto wait;

	/* NOTE FOR PROPER EPOLL SUPPORT
	 * instead: test if it's set, if so immediately return with events?
	 * do we need to create separate pollheads for each poll? my thinking:
	 * we need to remove all the pollheads after an iteration....
	 * or can we simply check if there's a live poll_wait going on??
	 */
	ke_event_clear(&epoll->ev);

	LIST_FOREACH (watch, &epoll->watches, watch_link) {
		/*
		 * NOTE FOR PROPER EPOLL SUPPORT:
		 * todo: check if watch is already live (someone else did it, or
		 * maybe it's edge-triggered)
		 */
		// VOP_POLL(watch->file->vn, watch, kChPollAdd);
	}

wait:
	ke_spinlock_release(&epoll->lock, ipl);

	kwaitstatus_t ws = ke_wait(&epoll->ev, "poll_wait_locked: epoll->ev",
	    true, true, nanosecs);
	kassert(ws == kKernWaitStatusOK || ws == kKernWaitStatusTimedOut);

	ipl = ke_spinlock_acquire(&epoll->lock);

	if (nevents == 0)
		goto finish;

	LIST_FOREACH (watch, &epoll->watches, watch_link) {
		if (watch->revents != 0) {
			/* revents must be a subset of watch events */
			kassert((watch->event.events | watch->revents) ==
			    watch->event.events);

			events[r].data.u64 = watch->event.data.u64;
			events[r].events = watch->revents;

			/*
			 * NOTE FOR PROPER EPOLL SUPPORT
			 * can we really set revents to 0 here? what if there
			 * are multiple people polling this epoll? it seems to
			 * me we need to get revents from the vnode.
			 *
			 * we MAY want to reset state if EPOLLET is set?
			 *
			 * chatgpt said: To support multiple pollers, you would
			 * need to maintain a separate revents value for each
			 * poller and reset the value only for the specific
			 * poller that retrieved the events.
			 * can we trust chatgpt? lmao
			 */
			watch->revents = 0;

			/* increment r for each event we get*/
			if (++r == nevents)
				break;
		}
	}

finish:
	ke_spinlock_release(&epoll->lock, ipl);

	return r;
}

uintptr_t
sys_pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
    const struct timespec *timeout, const sigset_t *sigmask)
{
	int r;
	int nevents = 0;
	struct epoll *epoll;
	ipl_t ipl;

	kassert(nfds > 0);
	kassert(nevents <= 25);
	epoll = epoll_new();

	ipl = ke_spinlock_acquire(&epoll->lock);

	for (size_t i = 0; i < nfds; i++) {
		uint32_t events = 0;

		if (FD_ISSET(i, readfds))
			events |= EPOLLIN;
		if (FD_ISSET(i, writefds))
			events |= EPOLLOUT;
		if (FD_ISSET(i, exceptfds))
			events |= EPOLLERR;

		if (events != 0) {
			struct epollhead *watch = kmem_alloc(sizeof(*watch));
			watch->file = ps_curproc()->files[i];
			watch->event.events = events;
			watch->epoll = epoll;
			watch->live = false;
			watch->revents = 0;
			r = epoll_add(epoll, watch);
			if (r != 0)
				goto finish;
			nevents++;
		}
	}

	struct epoll_event *events = NULL;

	if (nevents)
		events = kmem_alloc(sizeof(struct epoll_event) * nevents);

	nanosecs_t nanosecs;
	if (!timeout)
		nanosecs = -1;
	else if (timeout->tv_nsec <= 100000 && timeout->tv_nsec == 0)
		nanosecs = 0;
	else
		nanosecs = (nanosecs_t)timeout->tv_sec * NS_PER_S +
		    (nanosecs_t)timeout->tv_nsec;

	r = poll_wait_locked(epoll, events, nevents, nanosecs, ipl);

finish:
	epoll_destroy(epoll, ipl);

	return r;
}