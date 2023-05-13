/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#include <sys/socket.h>

#include <stdint.h>

#include "abi-bits/errno.h"
#include "abi-bits/in.h"
#include "kdk/kmem.h"
#include "keysock/sockfs.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/udp.h"

#define VNTOUDP(VN) ((struct sock_udp *)vn->data)

struct sock_udp {
	struct socknode socknode;
	struct udp_pcb *udp_pcb;
};

/*!
 * Packet received callback.
 */
static void
sock_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
    const ip_addr_t *addr, u16_t port)
{
	kfatal("implement\n");
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
	r = sock_init(&sock->socknode, &unix_soops);

	kassert(r == 0);

	sock->udp_pcb = udp_new_ip_type(
	    domain == AF_INET ? IPADDR_TYPE_V4 : IPADDR_TYPE_ANY);
	if (!sock->udp_pcb) {
		kmem_free(sock, sizeof(*sock));
		return -ENOBUFS;
	}

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

	err = udp_bind(VNTOUDP(vn)->udp_pcb, &ip_addr, port);

	return err_to_errno(err);
}

/*!
 * Connect a UDP socket to an address. (todo)
 */
int
sock_udp_connect(vnode_t *vn, const struct sockaddr *nam, socklen_t addr_len)
{
	return -EOPNOTSUPP;
}

/*!
 * Send on an UDP socket.
 */
int
sock_udp_sendto(vnode_t *vn, void *buf, size_t len, const struct sockaddr *nam,
    socklen_t namlen)
{
	struct pbuf *p;
	err_t err;
	ip_addr_t ip_addr;
	uint16_t port;

	err = addr_unpack_ip(nam, namlen, &ip_addr, &port);
	if (err != 0)
		return err;

	p = pbuf_alloc_reference(buf, len, PBUF_ROM);
	if (!p)
		return -ENOBUFS;

	err = udp_sendto(VNTOUDP(vn)->udp_pcb, p, &ip_addr, port);

	return err_to_errno(err);
}

struct socknodeops udp_soops = {
	.create = sock_udp_create,
};
