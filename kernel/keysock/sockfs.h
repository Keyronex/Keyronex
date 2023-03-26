/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#ifndef KRX_KEYSOCK_SOCKFS_H
#define KRX_KEYSOCK_SOCKFS_H

#include <sys/socket.h>

#include <stdint.h>

#include "abi-bits/in.h"
#include "kdk/vfs.h"

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
};

/*!
 * Socket node operations.
 */
struct socknodeops {
	int (*create)(krx_out vnode_t **out_vn, int domain, int protocol);
	int (*accept)(vnode_t *vn, krx_out vnode_t **out_vn);
};

/* internal functions */
int addr_unpack_ip(const struct sockaddr *nam, socklen_t namlen,
    ip_addr_t *ip_out, uint16_t *port_out);

#endif /* KRX_KEYSOCK_SOCKFS_H */
