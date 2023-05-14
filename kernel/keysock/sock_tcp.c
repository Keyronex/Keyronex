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

struct tcp_packet {
	/*! Linkage in receive queue. */
	STAILQ_ENTRY(tcp_packet) stailq_entry;
	/*! Byte size of the main data body of the packet. */
	uint32_t size;
	/*! Bytes already read. */
	uint32_t offset;
	/*! Main data body. */
	uint8_t *data;
};

struct sock_tcp {
	struct socknode socknode;
	struct tcp_pcb *tcp_pcb;
	struct kevent connect_ev;

	/*! Receive queue. */
	STAILQ_HEAD(, tcp_packet) rx_queue;
};

extern struct socknodeops tcp_soops;

static int
sock_tcp_common_alloc(krx_out vnode_t **vnode)
{
	struct sock_tcp *sock;
	int r;

	sock = kmem_alloc(sizeof(*sock));
	r = sock_init(&sock->socknode, &tcp_soops);
	kassert(r == 0);

	ke_event_init(&sock->connect_ev, false);
	STAILQ_INIT(&sock->rx_queue);

	*vnode = sock->socknode.vnode;

	return 0;
}

/*!
 * Packet received callback.
 */
static err_t
sock_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct sock_tcp *sock = arg;
	struct tcp_packet *pkt;
	size_t len;
	ipl_t ipl;

	if (p == NULL) {
		kdprintf("TCP Socket %p peer disconnected\n", arg);
		return ERR_OK;
	}

	len = p->tot_len;

	kassert(p->ref == 1);
	kassert(len > 0);
	kassert(err == ERR_OK);

	pkt = kmem_alloc(sizeof(*pkt));
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

	tcp_recved(pcb, len);

	return ERR_OK;
}

/*!
 * Socket connected callback.
 */
static err_t
tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
	struct sock_tcp *sock = arg;

	kassert(err == ERR_OK);
	ke_event_signal(&sock->connect_ev);

	return ERR_OK;
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

	kfatal("SOCK_TCP_ACCEPT!\n");

	r = sock_tcp_common_alloc(&newvn);
	if (r != 0)
		return ERR_MEM;

	newsock = VNTOTCP(newvn);
	newsock->tcp_pcb = newpcb;

	tcp_backlog_delayed(newpcb);
	tcp_arg(newpcb, newsock);
	tcp_recv(newpcb, sock_tcp_recv_cb);

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
static int
sock_tcp_create(krx_out vnode_t **vnode, int domain, int type, int protocol)
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

	tcp_arg(sock->tcp_pcb, sock);
	tcp_recv(sock->tcp_pcb, sock_tcp_recv_cb);
	UNLOCK_TCPIP_CORE();

	*vnode = vn;

	return 0;
}

/*!
 * Assign an address to a TCP socket.
 */
static int
sock_tcp_bind(vnode_t *vn, const struct sockaddr *nam, socklen_t addr_len)
{
	err_t err;
	ip_addr_t ip_addr;
	uint16_t port;
	struct sockaddr_in *sin = (struct sockaddr_in *)nam;

	inet_addr_to_ip4addr(ip_2_ip4(&ip_addr), &(sin->sin_addr));
	port = lwip_ntohs((sin)->sin_port);

	kdprintf("TCP: Binding to %x:%d\n", ip_addr.addr, port);

	LOCK_TCPIP_CORE();
	err = tcp_bind(VNTOTCP(vn)->tcp_pcb, &ip_addr, port);
	UNLOCK_TCPIP_CORE();

	return err_to_errno(err);
}

static int
sock_tcp_close(vnode_t *vn)
{
	struct sock_tcp *sock = VNTOTCP(vn);

	LOCK_TCPIP_CORE();
	tcp_close(sock->tcp_pcb);
	UNLOCK_TCPIP_CORE();

	return 0;
}

/*!
 * Connect a TCP socket to an address. (todo)
 */
static int
sock_tcp_connect(vnode_t *vn, const struct sockaddr *nam, socklen_t addr_len)
{
	err_t err;
	ip_addr_t ip_addr;
	uint16_t port;
	struct sock_tcp *sock = VNTOTCP(vn);

	err = addr_unpack_ip(nam, addr_len, &ip_addr, &port);
	if (err != ERR_OK) {
		return -err_to_errno(err);
	}

	LOCK_TCPIP_CORE();
	err = tcp_connect(sock->tcp_pcb, &ip_addr, port, tcp_connected_cb);
	UNLOCK_TCPIP_CORE();
	if (err != ERR_OK)
		return err_to_errno(err);

	ke_wait(&sock->connect_ev, "tcp_connect:sock_>connect_ev", false, false,
	    -1);

	return 0;
}

static int
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

static int
sock_tcp_recv(vnode_t *vn, void *buf, size_t nbyte, int flags, ipl_t ipl)
{
	struct sock_tcp *sock = VNTOTCP(vn);
	struct tcp_packet *pkt;
	size_t nread;
	size_t pkt_offset;

	/* note: entered spinlocked */

	kassert(!STAILQ_EMPTY(&sock->rx_queue));
	pkt = STAILQ_FIRST(&sock->rx_queue);

	nread = MIN2(pkt->size - pkt->offset, nbyte);
	kassert(nread > 0);

	pkt_offset = pkt->offset;

	if (flags & MSG_PEEK)
		goto do_read;

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

do_read:
	ke_spinlock_release(&sock->socknode.lock, ipl);

	/*
	 * n.b. need to free packet eventually without lock. Can do that by
	 * having the packet store a refcount. When we unlock to do copy out,
	 * we increment refcnt on the packet so it won't go away on us while
	 * we are working with it.
	 */

#if DEBUG_UDS == 1
	kdprintf("%s %zu on a socket; pkt %p, data %p, offs %zu\n",
	    flags & MSG_PEEK ? "Peeked" : "Received", nread, pkt, pkt->data,
	    pkt_offset);
#endif

	memcpy(buf, pkt->data + pkt_offset, nread);

	return nread;
}

/*!
 * Send on an TCP socket.
 */
int
sock_tcp_send(vnode_t *vn, void *buf, size_t len)
{
	err_t err;
	struct sock_tcp *sock = VNTOTCP(vn);

	void *cpy = kmem_alloc(len);
	memcpy(cpy, buf, len);

	LOCK_TCPIP_CORE();
	err = tcp_write(sock->tcp_pcb, cpy, len, 0);
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

	r = sock_tcp_create(&vnode, AF_INET, SOCK_STREAM, IPPROTO_TCP);
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
	r = sock_tcp_send(peer_vn, (void *)resp, sizeof(resp));
	if (r != 0)
		kfatal("sock_tcp_sendto failed: %d\n", r);

	return 0;
}

struct socknodeops tcp_soops = {
	.create = sock_tcp_create,
	.accept = sock_tcp_accept,
	.connect = sock_tcp_connect,
	.close = sock_tcp_close,
	.recv = sock_tcp_recv,
	.send = sock_tcp_send,
};
