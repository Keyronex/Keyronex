/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#ifndef KRX_KEYSOCK_SOCKFS_H
#define KRX_KEYSOCK_SOCKFS_H

#include "kdk/vfs.h"

/*!
 * Base type of a node in the socket filesystem representing a KeySock socket. 
 *
 * Protocols extend this struct.
 */
struct socknode {
	/*! Associated vnode */
	vnode_t *vnode;
};

#endif /* KRX_KEYSOCK_SOCKFS_H */
