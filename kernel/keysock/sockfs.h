/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#ifndef KRX_KEYSOCK_SOCKFS_H
#define KRX_KEYSOCK_SOCKFS_H

#include <sys/socket.h>

#include <abi-bits/in.h>
#include <stdint.h>

#include "executive/epoll.h"
#include "kdk/vfs.h"
#include "lwip/inet.h"

enum chpoll_kind;

/*!
 * Base type of a node in the socket filesystem representing a KeySock socket.
 *
 * Protocols extend this struct.
 *
 * The lifetime of the socket is by default the lifetime of the vnode describing
 * it. The #vnode pointer is weak. The exception is when a socket is on an
 * accept queue. In that case the reference to the socket is considered to also
 * keep alive the reference to the vnode.
 *
 * (l) => #lock
 * (p) => #polllist->lock
 * (~) => invariant from creation
 */
struct socknode {
	/*! (per-socket) Associated vnode; weak unless socket on accept queue */
	vnode_t *vnode;
	/*! (~) Socket operations vector. */
	struct socknodeops *sockops;
	int domain;   /* (~) */
	int type;     /* (~) */
	int protocol; /* (~) */

	/*! Lock. */
	kspinlock_t lock;

	/*! () Poll list. */
	struct polllist polllist;

	/*! (l to set) Event to wait on for accept() */
	kevent_t accept_evobj;
	/*! (l) Queue of sockets awaiting accept() */
	STAILQ_HEAD(, socknode) accept_stailq;
	/* Linkage in another socket's #accept_stailq */
	STAILQ_ENTRY(socknode) accept_stailq_entry;
};

/*!
 * Socket node operations. They are all entered unlocked for the time being.
 */
struct socknodeops {
	int (*create)(krx_out vnode_t **out_vn, int domain, int type,
	    int protocol);
	int (*accept)(vnode_t *vn, krx_out struct sockaddr *addr,
	    krx_inout socklen_t *addrlen);
	int (*bind)(vnode_t *vn, const struct sockaddr *nam, socklen_t namlen);
	int (*listen)(vnode_t *vn, uint8_t backlog);
	int (*connect)(vnode_t *vn, const struct sockaddr *nam,
	    socklen_t addr_len);
	int (*chpoll)(vnode_t *vn, struct pollhead *, enum chpoll_kind);
};

int sock_create(int domain, int type, int protocol, vnode_t **out);
int sock_bind(vnode_t *vn, const struct sockaddr *addr, socklen_t addrlen);
int sock_listen(vnode_t *vn, uint8_t backlog);
int sock_pair(int domain, int type, int protocol, vnode_t *out[2]);
int sock_accept(vnode_t *vn, krx_out struct sockaddr *addr,
    krx_inout socklen_t *addrlen, krx_out vnode_t **out);

/* internal functions */
/*! @brief Unack an IP/port from a sockaddr. */
int addr_unpack_ip(const struct sockaddr *nam, socklen_t namlen,
    krx_out ip_addr_t *ip_out, krx_out uint16_t *port_out);
/*! @brief Pack an IP/port into a sockaddr. */
int addr_pack_ip(krx_out struct sockaddr *addr, krx_inout socklen_t *addrlen,
    ip_addr_t *ip, uint16_t port);

/*! @brief Initialise a new socket node.*/
int sock_init(struct socknode *sock, struct socknodeops *ops);
/*! @brief Raise an event with a socket. */
int sock_event_raise(struct socknode *sock, int events);

extern struct socknodeops unix_soops;
extern struct vnops sock_vnops;

#endif /* KRX_KEYSOCK_SOCKFS_H */
