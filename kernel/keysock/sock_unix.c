/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 27 2023.
 */

#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <kdk/libkern.h>

#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/vfs.h"
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

#define VNTOUNP(VN) ((struct sock_unix *)(VN)->data)

static int
sock_unix_common_alloc(krx_out vnode_t **vnode)
{
	struct sock_unix *sock;
	int r;

	sock = kmem_alloc(sizeof(*sock));
	r = sock_init(&sock->socknode, &unix_soops);
	kassert(r == 0);

	sock->remote = NULL;
	STAILQ_INIT(&sock->rx_queue);

	*vnode = sock->socknode.vnode;

	return 0;
}

int
sock_unix_accept(vnode_t *vn, krx_out struct sockaddr *addr,
    krx_inout socklen_t *addrlen)
{
	(void)vn;
	(void)addr;
	(void)addrlen;
	/* most of the work done for us elsewhere */
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
		break;

	case SOCK_DGRAM:
	default:
		return EPROTOTYPE;
	}

	r = sock_unix_common_alloc(&vn);
	if (r != 0)
		return r;

	sock = VNTOUNP(vn);
	sock->socknode.domain = domain;
	sock->socknode.type = type;
	sock->socknode.protocol = protocol;

	*out_vn = vn;

	return 0;
}

int
sock_unix_bind(vnode_t *vn, const struct sockaddr *addr, socklen_t addrlen)
{
	struct sockaddr_un *sun;
	int r;
	vnode_t *filevn;
	struct sock_unix *sock = VNTOUNP(vn);
	struct vattr attr = { 0 };

	if (addr->sa_family != AF_UNIX)
		return -EINVAL;

	sun = (struct sockaddr_un *)addr;
	attr.type = VSOCK;
	attr.mode = S_IFSOCK | 0777;

	/*
	 * minor niggle: there is a period between lookup and the setting of the
	 * socket in which the socket vnode lacks its associated socket, which
	 * is inappropriate.
	 */

	r = vfs_lookup(root_vnode, &filevn, sun->sun_path, kLookupCreate,
	    &attr);
	if (r != 0)
		return r;

	filevn->sock = sock;

	return 0;
}

int
sock_unix_connect(vnode_t *vn, const struct sockaddr *addr, socklen_t addrlen)
{
	struct sockaddr_un *sun;
	int r;
	ipl_t ipl;
	vnode_t *filevn, *peervn;
	struct sock_unix *sock = VNTOUNP(vn), *listener, *peer;

	if (addr->sa_family != AF_UNIX) {
		kfatal("Unix connect bad\n");
		return -EINVAL;
	}

	sun = (struct sockaddr_un *)addr;

	r = vfs_lookup(root_vnode, &filevn, sun->sun_path, 0, NULL);
	if (r != 0) {
		kfatal("Failed to connect unix sock %d\n", r);
		return r;
	}

	if (filevn->type != VSOCK)
		return -ENOTSOCK;

	listener = filevn->sock;

	r = sock_unix_common_alloc(&peervn);
	kassert(r == 0);

	peer = VNTOUNP(peervn);
	peer->remote = sock;
	sock->remote = peer;

	/* todo: consider factoring this */
	ipl = ke_spinlock_acquire(&listener->socknode.lock);
	STAILQ_INSERT_TAIL(&listener->socknode.accept_stailq, &peer->socknode,
	    accept_stailq_entry);
	ke_event_signal(&listener->socknode.accept_evobj);
	sock_event_raise(&listener->socknode, EPOLLIN);
	ke_spinlock_release(&listener->socknode.lock, ipl);

	return 0;
}

int
sock_unix_listen(vnode_t *vn, uint8_t backlog)
{
	return 0;
}

int
sock_unix_recv(vnode_t *vn, void *buf, size_t nbyte, ipl_t ipl)
{
	struct sock_unix *sock = VNTOUNP(vn);
	struct unix_packet *pkt;
	size_t nread;
	size_t pkt_offset;

	/* note: entered spinlocked */

	kassert(!STAILQ_EMPTY(&sock->rx_queue));
	pkt = STAILQ_FIRST(&sock->rx_queue);

	nread = MIN2(pkt->size - pkt->offset, nbyte);
	kassert(nread > 0);

	pkt_offset = pkt->offset;
	pkt->offset += nread;
	kassert(pkt->offset <= pkt->size);

	kdprintf("Reading %zu, Didn't read %d bytes\n", nread,
	    pkt->size - pkt->offset);

	if (pkt->offset == pkt->size) {
		STAILQ_REMOVE_HEAD(&sock->rx_queue, stailq_entry);
		if (STAILQ_EMPTY(&sock->rx_queue))
			ke_event_clear(&sock->socknode.read_evobj);
		else
			kdprintf("There is more data remaining.\n");
	}

	ke_spinlock_release(&sock->socknode.lock, ipl);

	/*
	 * n.b. need to free packet eventually without lock. Can do that by
	 * having the packet store a refcount. When we unlock to do copy out,
	 * we increment refcnt on the packet so it won't go away on us while
	 * we are working with it.
	 */
	memcpy(buf, pkt->data + pkt_offset, nread);

	return nread;
}

int
sock_unix_send(vnode_t *vn, void *buf, size_t nbyte)
{
	struct sock_unix *sock = VNTOUNP(vn), *remote;
	struct unix_packet *pkt;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&sock->socknode.lock);
	remote = sock->remote;
	ke_spinlock_release(&sock->socknode.lock, ipl);

	kassert(remote != NULL);

	pkt = kmem_alloc(sizeof(*pkt));

	pkt->size = nbyte;
	pkt->offset = 0;
	pkt->data = kmem_alloc(nbyte);

	memcpy(pkt->data, buf, nbyte);

	ipl = ke_spinlock_acquire(&remote->socknode.lock);
	STAILQ_INSERT_TAIL(&remote->rx_queue, pkt, stailq_entry);
	ke_event_signal(&remote->socknode.read_evobj);
	sock_event_raise(&remote->socknode, EPOLLIN);
	kdprintf("Sent %d on a socket\n", nbyte);
	ke_spinlock_release(&remote->socknode.lock, ipl);

	return nbyte;
}

struct socknodeops unix_soops = {
	.accept = sock_unix_accept,
	.create = sock_unix_create,
	.bind = sock_unix_bind,
	.connect = sock_unix_connect,
	.listen = sock_unix_listen,
	.recv = sock_unix_recv,
	.send = sock_unix_send,
};