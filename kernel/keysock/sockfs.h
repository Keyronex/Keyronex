/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#ifndef KRX_KEYSOCK_SOCKFS_H
#define KRX_KEYSOCK_SOCKFS_H

#include <sys/socket.h>

#include <stdint.h>

#include "abi-bits/in.h"
#include "executive/epoll.h"
#include "kdk/vfs.h"

enum chpoll_kind;

/*!
 * Base type of a node in the socket filesystem representing a KeySock socket.
 *
 * Protocols extend this struct.
 *
 * The lifetime of the socket is the lifetime of the vnode describing it. The
 * #vnode pointer is weak. The exception is when a socket is on an accept queue.
 * In that case the reference to the socket is considered to also keep alive the
 * reference to the vnode.
 */
struct socknode {
	/*! Associated vnode; weak unless socket on accept queue */
	vnode_t *vnode;
	/*! Socket operations vector. */
	struct socknodeops *sockops;
	/*! Poll list. */
	struct polllist polllist;
};

/*!
 * Socket node operations.
 */
struct socknodeops {
	int (*create)(krx_out vnode_t **out_vn, int domain, int protocol);
	int (*accept)(vnode_t *vn, krx_out vnode_t **out_vn);
	int (*bind)(vnode_t *vn, const struct sockaddr *nam, socklen_t namlen);
	int (*listen)(vnode_t *vn, uint8_t backlog);
	int (*connect)(vnode_t *vn, const struct sockaddr *nam,
	    socklen_t addr_len);
	int (*chpoll)(vnode_t *vn, struct pollhead *, enum chpoll_kind);
};

/* internal functions */
int addr_unpack_ip(const struct sockaddr *nam, socklen_t namlen,
    ip_addr_t *ip_out, uint16_t *port_out);

/*! @brief Initialise a new socket node.*/
int sock_init(struct socknode *sock, struct socknodeops *ops);
/*! @brief Raise an event with a socket. */
int sock_event_raise(struct socknode *sock, int events);

extern struct vnops sock_vnops;

#endif /* KRX_KEYSOCK_SOCKFS_H */
