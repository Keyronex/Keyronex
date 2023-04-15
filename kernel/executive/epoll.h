/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Mar 26 2023.
 */
/*!
 * @file executive/epoll.h
 * @brief Event poll interface
 */

#ifndef KRX_EXECUTIVE_EPOLL_H
#define KRX_EXECUTIVE_EPOLL_H

#include <sys/epoll.h>

#include "kdk/kernel.h"

struct vnode;
struct epoll;

/*!
 * Entry in a pollable object's poll-list, representing a current poll on the
 * object
 *
 * TODO:
 * "my proposed locking scheme for epoll which i think will be robust is to
 * define a struct epollhead_list which contains its own spinlock and which
 * every object which can be epoll'd has. so the lock ordering will look like
 * this for 1. "add a watch to an epoll" v.s. 2. "object signals a watch has an
 * event for an epoll"
 * 1. lock struct epoll  -> lock struct epollhead_list lock of object -> install
 * watch on the list
 * 2. struct epollhead_list -> atomically increment a counter in the epoll ->
 * unlock struct epollhead_list -> lock struct epoll -> deliver event to epoll""
 *
 * (~) = invariant
 * (e) = #epoll lock
 * (o) = #vnode lock
 * .... figure this all out.
 */
struct pollhead {
	/*! linkage in struct epoll::entries */
	LIST_ENTRY(pollhead) watch_link;
	/*!
	 * Which open file is being epolled? Optional entry; can be absent.
	 *
	 * A non-owning reference. It is worth noting that when the file becomes
	 * inaccessible (due to refcount dropping sufficiently) this will be the
	 * last reference to it.
	 *
	 * The file in its teardown routine can therefore simply ask #epoll to
	 * drop this pollhead. No further locking is necessary.
	 */
	struct file *file;
	/*! Which vnode is being polled? */
	struct vnode *vnode;
	/*! which events to poll + userdata */
	struct epoll_event event;

	/*! linkage in object's polllist */
	LIST_ENTRY(pollhead) polllist_entry;
	/*! linkage in #file polllist */
	LIST_ENTRY(pollhead) file_list_entry;
	/*! to which epoll does it belong? */
	struct epoll *epoll;
	/*! (e) what events did we get? */
	uint32_t revents;
	/*! is the pollhead inserted and live in the vnode's pollist? */
	bool live;
};

/*!
 * A list of pollers on some object.
 */
struct polllist {
	kspinlock_t lock;
	LIST_HEAD(, pollhead) pollhead_list;
};

struct epoll *epoll_do_new(void);
int epoll_do_add(struct epoll *epoll, struct file *file, struct vnode *vnode,
    struct epoll_event *event);
int epoll_do_ctl(struct epoll *epoll, int op, int fd,
    struct epoll_event *event);
void epoll_do_destroy(struct epoll *epoll);
int epoll_do_wait(struct epoll *epoll, struct epoll_event *events, int nevents,
    nanosecs_t nanosecs);
struct vnode *epoll_new(void);
struct epoll *epoll_from_vnode(struct vnode *);

/*! @brief Initialise a polllist. */
void pollist_init(struct polllist *pl);
/*! @brief Raise events on a pollhead */
int pollhead_raise(struct pollhead *ph, int events);

#endif /* KRX_EXECUTIVE_EPOLL_H */
