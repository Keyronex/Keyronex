/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 27 2023.
 */

#include <sys/socket.h>

#include "abi-bits/errno.h"
#include "abi-bits/socket.h"
#include "kdk/kmem.h"
#include "keysock/sockfs.h"

/*
 * We need to devise a way of doing the refcounting properly in this case.
 * Perhaps we can refcount references to the peer or linked socket separately
 * from references to that socket (in such a way as to enable listening on it.)
 */

/*!
 * Messages on both datagram and stream sockets in the Unix domain are
 * represented with these packets. SCM_CREDENTIALS and SCM_RIGHTS control
 * messages are re-synthesised at delivery time.
 */
struct unix_packet {
	/*! Linkage in receive queue. (There is no send queue) */
	STAILQ_ENTRY(unix_packet) stailq_entry;
	/*! Credentials of sender. */
	struct ucred ucred;
	/*! Whether credentials were explicitly attached. */
	bool has_ucred : 1, has_rights : 1;
	/*! Number of rights sent with the messsage. */
	uint32_t num_rights : 30;
	/*! Byte size of the main data body of the packet. */
	uint32_t size;
	/*! Bytes already read. */
	uint32_t offset;
	/*! Main data body. */
	uint8_t *data;
	/*! Rights. */
	struct file *rights;
};

struct sock_unix {
	struct socknode socknode;

	/*! Receive queue. */
	STAILQ_HEAD(, unix_packet) rx_queue;

	/*! Connected peer or linked (waiting for accept) socket*/
	struct sock_unix *remote;
};

static struct socknodeops unix_soops;

static int
sock_unix_common_alloc(krx_out vnode_t **vnode)
{
	struct sock_unix *sock;
	int r;

	sock = kmem_alloc(sizeof(*sock));
	r = sock_init(&sock->socknode, &unix_soops);
	kassert(r == 0);

	*vnode = sock->socknode.vnode;

	return 0;
}

int
sock_unix_create(krx_out vnode_t **out_vn, int domain, int type, int protocol)
{
	struct sock_unix *sock;
	vnode_t *vn;
	int r;

	kassert(domain == PF_UNIX);

	if (protocol != 0)
		return EPROTONOSUPPORT;

	switch (type) {
	case SOCK_STREAM:
	case SOCK_DGRAM:
		break;

	default:
		return EPROTOTYPE;
	}

	sock = kmem_alloc(sizeof(*sock));
}