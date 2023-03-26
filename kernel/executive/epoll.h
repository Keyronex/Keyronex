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
typedef struct epollhead {
	/*! linkage in struct epoll::entries */
	LIST_ENTRY(epollhead) watch_link;
	/*!
	 * Which open file is being epolled?
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
	/*! which events to poll + userdata */
	struct epoll_event event;

	/*! linkage in vnode's polllist */
	LIST_ENTRY(epollhead) vnode_list_entry;
	/*! linkage in #file polllist */
	LIST_ENTRY(epollhead) file_list_entry;
	/*! to which epoll does it belong? */
	struct epoll *epoll;
	/*! (e) what events did we get? */
	uint32_t revents;
	/*! is the pollhead inserted and live in the vnode's pollist? */
	bool live;
} epollhead_t;

#endif /* KRX_EXECUTIVE_EPOLL_H */
