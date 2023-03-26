/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#include <sys/socket.h>

#include <elf.h>
#include <stdint.h>

#include "abi-bits/errno.h"
#include "abi-bits/in.h"
#include "kdk/kerndefs.h"
#include "kdk/kmem.h"
#include "keysock/sockfs.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/tcp.h"

#define VNTOTCP(VN) ((struct sock_tcp *)(VN)->data)

struct sock_tcp {
	struct socknode socknode;

	struct tcp_pcb *tcp_pcb;

	kevent_t accept_evobj;
	STAILQ_HEAD(, sock_tcp) accept_stailq;
	STAILQ_ENTRY(sock_tcp) accept_stailq_entry;
};

static int
sock_tcp_common_alloc(krx_out vnode_t **vnode)
{
	struct sock_tcp *sock;
	vnode_t *vn;

	sock = kmem_alloc(sizeof(*sock));
	ke_event_init(&sock->accept_evobj, false);
	STAILQ_INIT(&sock->accept_stailq);

	vn = kmem_alloc(sizeof(*vn));
	vn->type = VSOCK;
	vn->data = (uintptr_t)sock;
	sock->socknode.vnode = vn;

	*vnode = vn;

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

	r = sock_tcp_common_alloc(&newvn);
	if (r != 0)
		return ERR_MEM;

	newsock = VNTOTCP(newvn);
	newsock->tcp_pcb = newpcb;
	STAILQ_INSERT_TAIL(&sock->accept_stailq, newsock, accept_stailq_entry);

	tcp_backlog_delayed(newpcb);
	ke_event_signal(&sock->accept_evobj);

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

	sock->tcp_pcb = tcp_new_ip_type(
	    domain == AF_INET ? IPADDR_TYPE_V4 : IPADDR_TYPE_ANY);
	if (!sock->tcp_pcb) {
		kmem_free(vn, sizeof(*vn));
		kmem_free(sock, sizeof(*sock));
		return -ENOBUFS;
	}

	tcp_recv(sock->tcp_pcb, sock_tcp_recv_cb);

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

	err = tcp_bind(VNTOTCP(vn)->tcp_pcb, &ip_addr, port);

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

	new_pcb = tcp_listen_with_backlog_and_err(sock->tcp_pcb, backlog, &err);
	if (new_pcb == NULL) {
		return err_to_errno(err);
	}

	sock->tcp_pcb = new_pcb;

	tcp_arg(sock->tcp_pcb, sock);
	tcp_accept(sock->tcp_pcb, sock_tcp_cb_accept);

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

	err = tcp_write(sock->tcp_pcb, buf, len, 0);

	return err_to_errno(err);
}

int
sock_tcp_accept(vnode_t *vn, krx_out vnode_t **new_vn)
{
	struct sock_tcp *sock = VNTOTCP(vn), *newsock;

again:
	newsock = STAILQ_FIRST(&sock->accept_stailq);

	if (newsock == NULL) {
		ke_wait(&sock->accept_evobj,
		    "sock_tcp_accept:sock->accept_evobj", false, false, -1);
		goto again;
	}

	STAILQ_REMOVE_HEAD(&sock->accept_stailq, accept_stailq_entry);

	*new_vn = newsock->socknode.vnode;

	return 0;
}

int
test_tcpserver(void)
{
	struct sockaddr_in nam;
	vnode_t *vnode, *peer_vn;
	int r;

	nam.sin_family = AF_INET;
	nam.sin_port = htons(8080);
	nam.sin_addr.s_addr = htonl(INADDR_ANY);

	r = sock_tcp_create(AF_INET, IPPROTO_TCP, &vnode);
	if (r != 0)
		kfatal("sock_tcp_created failed: %d\n", r);

	r = sock_tcp_bind(vnode, &nam, sizeof(nam));
	if (r != 0)
		kfatal("sock_tcp_bind failed: %d\n", r);

	r = sock_tcp_listen(vnode, 5);
	if (r != 0)
		kfatal("sock_tcp_listen failed: %d\n", r);

	kdprintf("listening on socket...\n");

	r = sock_tcp_accept(vnode, &peer_vn);
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