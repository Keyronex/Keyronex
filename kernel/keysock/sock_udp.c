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
sock_udp_create(int domain, int protocol, krx_out vnode_t **vnode)
{
	struct sock_udp *sock;
	vnode_t *vn;

	if (protocol != IPPROTO_UDP && protocol != 0)
		return -EPROTONOSUPPORT;

	sock = kmem_alloc(sizeof(*sock));

	sock->udp_pcb = udp_new_ip_type(
	    domain == AF_INET ? IPADDR_TYPE_V4 : IPADDR_TYPE_ANY);
	if (!sock->udp_pcb) {
		kmem_free(sock, sizeof(*sock));
		return -ENOBUFS;
	}

	udp_recv(sock->udp_pcb, sock_udp_recv_cb, sock);

	vn = kmem_alloc(sizeof(*vn));
	vn->type = VSOCK;
	vn->data = (uintptr_t)sock;

	*vnode = vn;

	return 0;
}

/*!
 * Assign an address to a UDP socket.
 */
int
sock_udp_bind(vnode_t *vn, const struct sockaddr *nam, socklen_t addr_len)
{
	err_t err;
	ip_addr_t ip_addr;
	uint16_t port;
	struct sockaddr_in *sin = (struct sockaddr_in *)nam;

	inet_addr_to_ip4addr(ip_2_ip4(&ip_addr), &(sin->sin_addr));
	port = lwip_ntohs((sin)->sin_port);

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
int sock_udp_sendto(vnode_t *vn, void *buf, size_t len, const struct sockaddr *nam, socklen_t addr_len) {
	struct pbuf *p;
	err_t err;
	ip_addr_t ip_addr;
	uint16_t port;
	struct sockaddr_in *sin = (struct sockaddr_in *)nam;

	p = pbuf_alloc_reference(buf, len, PBUF_ROM);
	if (!p)
		return -ENOBUFS;
	
	inet_addr_to_ip4addr(ip_2_ip4(&ip_addr), &(sin->sin_addr));
	port = lwip_ntohs((sin)->sin_port);

	err = udp_sendto(VNTOUDP(vn)->udp_pcb, p, &ip_addr, port);

	return err_to_errno(err);
}