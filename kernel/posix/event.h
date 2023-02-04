#ifndef EVENT_H_
#define EVENT_H_

#include <sys/epoll.h>

#include <nanokern/queue.h>

#include <stdbool.h>

/*!
 * which kind of chpoll call?
 */
typedef enum chpoll_kind {
	/*! poll is being added; the pollhead should NOT exist in the list */
	kChPollAdd,
	/*! poll is being removed; the pollhead MUST exist in the list */
	kChPollRemove,
	/*! poll conditions are being changed */
	kChPollChange
} chpoll_kind_t;

/*!
 * entry in a pollable object's poll-list, representing a current poll on the
 * object
 *
 * (~) = invariant
 * (e) = epoll->lock
 */
typedef struct epollhead {
	/*! linkage in struct epoll::entries */
	LIST_ENTRY(epollhead) watch_link;
	/*! which file is being epolled? */
	struct file *file;
	/*! which event to poll + userdata */
	struct epoll_event event;

	/*! linkage in vnode's polllist */
	LIST_ENTRY(epollhead) pollhead_entry;
	/*! to which epoll does it belong? */
	struct epoll *epoll;
	/*! (e) what events did we get? */
	uint32_t revents;
	/*! is the pollhead inserted and live in the vnode's pollist? */
	bool live;
} epollhead_t;

#endif /* EVENT_H_ */
