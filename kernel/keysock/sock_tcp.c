/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#include <sys/socket.h>

#include <elf.h>
#include <stdint.h>

#include "abi-bits/errno.h"
#include "abi-bits/in.h"
#include "executive/epoll.h"
#include "kdk/kerndefs.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "keysock/sockfs.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"

#define VNTOTCP(VN) ((struct sock_tcp *)(VN)->data)

struct sock_tcp {
	struct socknode socknode;

	struct tcp_pcb *tcp_pcb;
};

static struct socknodeops tcp_soops;

static int
sock_tcp_common_alloc(krx_out vnode_t **vnode)
{
	struct sock_tcp *sock;
	int r;

	sock = kmem_alloc(sizeof(*sock));
	r = sock_init(&sock->socknode, &tcp_soops);
	kassert(r == 0);

	*vnode = sock->socknode.vnode;

	return 0;
}

/*!
 * Packet received callback.
 */
static err_t
sock_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *pbuf, err_t err)
{
	if (err != ERR_OK)
		kfatal("bad err: %d\n", err);

	kfatal("implement\n");
}

/*!
 * New connection callback.
 */
static err_t
sock_tcp_cb_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	struct sock_tcp *sock = (struct sock_tcp *)arg, *newsock;
	vnode_t *newvn;
	int r;
	ipl_t ipl;

	/* This is called with the TCP/IP core lock locked. */

	r = sock_tcp_common_alloc(&newvn);
	if (r != 0)
		return ERR_MEM;

	newsock = VNTOTCP(newvn);
	newsock->tcp_pcb = newpcb;

	tcp_backlog_delayed(newpcb);

	ipl = ke_spinlock_acquire(&sock->socknode.lock);
	STAILQ_INSERT_TAIL(&sock->socknode.accept_stailq, &newsock->socknode,
	    accept_stailq_entry);
	ke_event_signal(&sock->socknode.accept_evobj);
	sock_event_raise(&sock->socknode, EPOLLIN);
	ke_spinlock_release(&sock->socknode.lock, ipl);

	return ERR_OK;
}

/*!
 * Create a new unbound TCP socket.
 */
int
sock_tcp_create(int domain, int protocol, krx_out vnode_t **vnode)
{
	struct sock_tcp *sock;
	vnode_t *vn;
	int r;

	if (protocol != IPPROTO_TCP && protocol != 0)
		return -EPROTONOSUPPORT;

	r = sock_tcp_common_alloc(&vn);
	if (r != 0)
		return r;

	sock = VNTOTCP(vn);

	LOCK_TCPIP_CORE();
	sock->tcp_pcb = tcp_new_ip_type(
	    domain == AF_INET ? IPADDR_TYPE_V4 : IPADDR_TYPE_ANY);
	if (!sock->tcp_pcb) {
		UNLOCK_TCPIP_CORE();
		kmem_free(vn, sizeof(*vn));
		kmem_free(sock, sizeof(*sock));
		return -ENOBUFS;
	}

	tcp_recv(sock->tcp_pcb, sock_tcp_recv_cb);
	UNLOCK_TCPIP_CORE();

	*vnode = vn;

	return 0;
}

/*!
 * Assign an address to a TCP socket.
 */
int
sock_tcp_bind(vnode_t *vn, const struct sockaddr *nam, socklen_t addr_len)
{
	err_t err;
	ip_addr_t ip_addr;
	uint16_t port;
	struct sockaddr_in *sin = (struct sockaddr_in *)nam;

	inet_addr_to_ip4addr(ip_2_ip4(&ip_addr), &(sin->sin_addr));
	port = lwip_ntohs((sin)->sin_port);

	kdprintf("Binding to %x:%d\n", ip_addr.addr, port);

	LOCK_TCPIP_CORE();
	err = tcp_bind(VNTOTCP(vn)->tcp_pcb, &ip_addr, port);
	UNLOCK_TCPIP_CORE();

	return err_to_errno(err);
}

/*!
 * Connect a TCP socket to an address. (todo)
 */
int
sock_tcp_connect(vnode_t *vn, const struct sockaddr *nam, socklen_t addr_len)
{
	return -EOPNOTSUPP;
}

int
sock_tcp_listen(vnode_t *vn, uint8_t backlog)
{
	err_t err;
	struct sock_tcp *sock = VNTOTCP(vn);
	struct tcp_pcb *new_pcb;

	LOCK_TCPIP_CORE();
	new_pcb = tcp_listen_with_backlog_and_err(sock->tcp_pcb, backlog, &err);
	if (new_pcb == NULL) {
		UNLOCK_TCPIP_CORE();
		return err_to_errno(err);
	}

	sock->tcp_pcb = new_pcb;

	tcp_arg(sock->tcp_pcb, sock);
	tcp_accept(sock->tcp_pcb, sock_tcp_cb_accept);
	UNLOCK_TCPIP_CORE();

	return 0;
}

/*!
 * Send on an TCP socket.
 */
int
sock_tcp_sendto(vnode_t *vn, void *buf, size_t len,
    const struct sockaddr *nam __unused, socklen_t addr_len __unused)
{
	err_t err;
	struct sock_tcp *sock = VNTOTCP(vn);

	LOCK_TCPIP_CORE();
	err = tcp_write(sock->tcp_pcb, buf, len, 0);
	UNLOCK_TCPIP_CORE();

	return err_to_errno(err);
}

int
sock_tcp_accept(vnode_t *vn, krx_out struct sockaddr *addr,
    krx_inout socklen_t *addrlen)
{
	struct sock_tcp *sock = VNTOTCP(vn);
	ip_addr_t ip;
	uint16_t port;

	LOCK_TCPIP_CORE();
	tcp_backlog_accepted(newsock->tcp_pcb);
	tcp_tcp_get_tcp_addrinfo(sock->tcp_pcb, 0, &ip, &port);
	UNLOCK_TCPIP_CORE();

	addr_pack_ip(addr, addrlen, &ip, port);

	return 0;
}

int
test_tcpserver(void)
{
	struct sockaddr_in nam;
	socklen_t addrlen = sizeof(nam);
	vnode_t *vnode, *peer_vn;
	int r;

	nam.sin_family = AF_INET;
	nam.sin_port = htons(8080);
	nam.sin_addr.s_addr = htonl(INADDR_ANY);

	r = sock_tcp_create(AF_INET, IPPROTO_TCP, &vnode);
	if (r != 0)
		kfatal("sock_tcp_created failed: %d\n", r);

	r = sock_tcp_bind(vnode, (struct sockaddr *)&nam, sizeof(nam));
	if (r != 0)
		kfatal("sock_tcp_bind failed: %d\n", r);

	r = sock_tcp_listen(vnode, 5);
	if (r != 0)
		kfatal("sock_tcp_listen failed: %d\n", r);

	kdprintf("listening on socket...\n");

	struct epoll *epoll;
	struct epoll_event listen_ev, rev;

	epoll = epoll_do_new();
	kassert(epoll != NULL);

	listen_ev.events = EPOLLIN | EPOLLERR;

	r = epoll_do_add(epoll, NULL, vnode, &listen_ev);
	kassert(r == 0);

	r = epoll_do_wait(epoll, &rev, 1, -1);
	kdprintf("Epoll Do Wait returned: %d\n", r);

	r = sock_accept(vnode, (struct sockaddr *)&nam, &addrlen, &peer_vn);
	if (r != 0)
		kfatal("sock_tcp_accept failed: %d\n", r);

	kdprintf("peer connected\n");

	static const char resp[] =
	    "HTTP/1.1 200 OK\r\nContent-Length: 29\r\nConnection: close\r\n\r\n<h1>Hello from Keyronex!</h1>";
	r = sock_tcp_sendto(peer_vn, (void *)resp, sizeof(resp), NULL, 0);
	if (r != 0)
		kfatal("sock_tcp_sendto failed: %d\n", r);

	return 0;
}

static struct socknodeops tcp_soops = {
	.accept = sock_tcp_accept,
};