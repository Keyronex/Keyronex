/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

/*
 * note: Lots of shared logic with datagram sock_unix and will probably be even
 * more shared logic if/when we get a raw packet socket. Will be worthwhile to
 * de-duplicate this as much as possible since there is logic that ought to be
 * implemented in common.
 */

#include <sys/socket.h>

#include <stdint.h>

#include "abi-bits/errno.h"
#include "abi-bits/in.h"
#include "kdk/kmem.h"
#include "keysock/sockfs.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

#define VNTOUDP(VN) ((struct sock_udp *)vn->data)

struct udp_packet {
	/*! Linkage in receive queue. */
	STAILQ_ENTRY(udp_packet) stailq_entry;
	/*! Received from IP. */
	ip_addr_t addr;
	/*! Received from port. */
	uint16_t port;
	/*! Byte size of the main data body of the packet. */
	uint32_t size;
	/*! Bytes already read. */
	uint32_t offset;
	/*! Main data body. */
	uint8_t *data;
};

struct sock_udp {
	struct socknode socknode;
	struct udp_pcb *udp_pcb;

	/*! Receive queue. */
	STAILQ_HEAD(, udp_packet) rx_queue;
};

/*!
 * Packet received callback.
 */
static void
sock_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
    const ip_addr_t *addr, u16_t port)
{
	struct sock_udp *sock = arg;
	struct udp_packet *pkt;
	ipl_t ipl;

	kassert(p->ref == 1);

	pkt = kmem_alloc(sizeof(*pkt));
	pkt->addr = *addr;
	pkt->port = port;
	pkt->size = p->tot_len;
	pkt->offset = 0;
	pkt->data = kmem_alloc(p->tot_len);
	pbuf_copy_partial(p, pkt->data, p->tot_len, 0);
	pbuf_free(p);

	ipl = ke_spinlock_acquire(&sock->socknode.lock);
	STAILQ_INSERT_TAIL(&sock->rx_queue, pkt, stailq_entry);
	ke_event_signal(&sock->socknode.read_evobj);
	sock_event_raise(&sock->socknode, EPOLLIN);
	ke_spinlock_release(&sock->socknode.lock, ipl);
}

/*!
 * Create a new unbound UDP socket.
 */
int
sock_udp_create(krx_out vnode_t **vnode, int domain, int type, int protocol)
{
	struct sock_udp *sock;
	int r;

	if (protocol != IPPROTO_UDP && protocol != 0)
		return -EPROTONOSUPPORT;

	sock = kmem_alloc(sizeof(*sock));
	r = sock_init(&sock->socknode, &udp_soops);

	kassert(r == 0);

	sock->udp_pcb = udp_new_ip_type(
	    domain == AF_INET ? IPADDR_TYPE_V4 : IPADDR_TYPE_ANY);
	if (!sock->udp_pcb) {
		kmem_free(sock, sizeof(*sock));
		return -ENOBUFS;
	}

	STAILQ_INIT(&sock->rx_queue);

	udp_recv(sock->udp_pcb, sock_udp_recv_cb, sock);

	*vnode = sock->socknode.vnode;

	return 0;
}

/*!
 * Assign an address to a UDP socket.
 */
int
sock_udp_bind(vnode_t *vn, const struct sockaddr *nam, socklen_t namlen)
{
	err_t err;
	ip_addr_t ip_addr;
	uint16_t port;

	err = addr_unpack_ip(nam, namlen, &ip_addr, &port);
	if (err != 0)
		return err;

	kdprintf("SOCK_UDP_BIND to %x:%d\n", ip_addr.addr, port);

	LOCK_TCPIP_CORE();
	err = udp_bind(VNTOUDP(vn)->udp_pcb, &ip_addr, port);
	UNLOCK_TCPIP_CORE();

	return err_to_errno(err);
}

static int sock_udp_close(vnode_t *vn)
{
	struct sock_udp *sock = VNTOUDP(vn);

	LOCK_TCPIP_CORE();
	udp_remove(sock->udp_pcb);
	UNLOCK_TCPIP_CORE();

	return 0;
}


/*!
 * Connect a UDP socket to an address. (todo)
 */
int
sock_udp_connect(vnode_t *vn, const struct sockaddr *nam, socklen_t addr_len)
{
	err_t err;
	ip_addr_t ip_addr;
	uint16_t port;

	err = addr_unpack_ip(nam, addr_len, &ip_addr, &port);
	if (err != 0)
		return err;

	kdprintf("SOCK_UDP_CONNECT to %x:%d\n", ip_addr.addr, port);

	return -EOPNOTSUPP;
}

int
sock_udp_recv(vnode_t *vn, void *buf, size_t nbyte, int flags, ipl_t ipl)
{
	struct sock_udp *sock = VNTOUDP(vn);
	struct udp_packet *pkt;
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

	if (pkt->offset == pkt->size) {
		STAILQ_REMOVE_HEAD(&sock->rx_queue, stailq_entry);
		if (STAILQ_EMPTY(&sock->rx_queue))
			ke_event_clear(&sock->socknode.read_evobj);
			/* todo(low?): this is technically improper */
#if 0
		else
			kdprintf(" !!! There is more data remaining.\n");
#endif
	}

	ke_spinlock_release(&sock->socknode.lock, ipl);

	/*
	 * n.b. need to free packet eventually without lock. Can do that by
	 * having the packet store a refcount. When we unlock to do copy out,
	 * we increment refcnt on the packet so it won't go away on us while
	 * we are working with it.
	 */

#if DEBUG_UDS == 0
	kdprintf("Received %zu on a socket; pkt %p, data %p, offs %zu\n", nread,
	    pkt, pkt->data, pkt_offset);
#endif

	memcpy(buf, pkt->data + pkt_offset, nread);

	return nread;
}

/*!
 * Send on an UDP socket.
 */
int
sock_udp_sendmsg(vnode_t *vn, struct msghdr *msg, int flags)
{
	struct pbuf *p;
	err_t err;
	bool sendto = false;
	ip_addr_t ip_addr;
	uint16_t port;

	if (msg->msg_name != NULL) {
		kassert(msg->msg_namelen != 0);
		sendto = true;
		err = addr_unpack_ip(msg->msg_name, msg->msg_namelen, &ip_addr,
		    &port);
		kdprintf("UDP_SENDTO %x:%d : %d\n", ip_addr.addr, port, err);
		if (err != 0)
			return err;
	}

	void *data = kmem_alloc(msg->msg_iov[0].iov_len);
	memcpy(data, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len);

	LOCK_TCPIP_CORE();
	p = pbuf_alloc_reference(data, msg->msg_iov[0].iov_len, PBUF_ROM);
	if (!p)
		return -ENOBUFS;

	err = udp_sendto(VNTOUDP(vn)->udp_pcb, p, &ip_addr, port);
	UNLOCK_TCPIP_CORE();

	kdprintf("UDP_SENDTO RETURNED %d\n", err_to_errno(err));

	return err == ERR_OK ? msg->msg_iov[0].iov_len : err_to_errno(err);
}

struct socknodeops udp_soops = {
	.create = sock_udp_create,
	.close = sock_udp_close,
	.recv = sock_udp_recv,
	.sendmsg = sock_udp_sendmsg,
};
