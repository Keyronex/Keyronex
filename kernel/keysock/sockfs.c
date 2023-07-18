/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#include "executive/epoll.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/process.h"
#include "keysock/sockfs.h"

#define VNTOSON(VN) ((struct socknode *)(VN)->data)

extern struct socknodeops udp_soops;
extern struct socknodeops tcp_soops;

const char *sockstate_str[] = {
	[kSockInitialised] = "init",
	[kSockListening] = "listen",
	[kSockConnected] = "conn",
};

static bool
sock_ready_for_read(struct socknode *sock)
{
	return ke_wait(&sock->read_evobj, "", false, false, 0) ==
	    kKernWaitStatusOK;
}

int
sock_event_raise(struct socknode *sock, int events)
{
	struct pollhead *ph, *tmp;

	LIST_FOREACH_SAFE (ph, &sock->polllist.pollhead_list, polllist_entry,
	    tmp) {
		pollhead_raise(ph, events);
	}

	return 0;
}

int
sock_create(int domain, int type, int protocol, vnode_t **out)
{
	switch (domain) {
	case PF_LOCAL:
		kprintf("[SockFS] Create a Unix socket, type %d proto %d\n",
		    type, protocol);
		return unix_soops.create(out, domain, type, protocol);

	case PF_INET:
		switch (type) {
		case SOCK_STREAM:
			kprintf("[SockFS] Create an internet stream socket\n");
			return tcp_soops.create(out, domain, type, protocol);

		case SOCK_DGRAM:
			kprintf(
			    "[SockFS] Create an internet datagram socket\n");

			return udp_soops.create(out, domain, type, protocol);

		case SOCK_RAW:
			kprintf("[SockFS] Create an internet raw socket\n");

			return raw_soops.create(out, domain, type, protocol);

		default:
			return -EPROTONOSUPPORT;
		}

	case PF_PACKET:
		kprintf("[SockFS] Create a packet socket, type %d proto %d\n",
		    type, protocol);
		return packet_soops.create(out, domain, type, protocol);

	default:
		kprintf("[SockFS] Unsupported domain %d (type %d, proto %d)\n",
		    domain, type, protocol);
		return -EAFNOSUPPORT;
	}
}

int
sock_bind(vnode_t *vn, const struct sockaddr *addr, socklen_t addrlen)
{
	struct socknode *sonode;
	kassert(vn->ops == &sock_vnops);
	sonode = VNTOSON(vn);
	return sonode->sockops->bind(vn, addr, addrlen);
}

int
sock_connect(vnode_t *vn, const struct sockaddr *addr, socklen_t addrlen)
{
	struct socknode *sonode;
	int r;
	kassert(vn->ops == &sock_vnops);
	sonode = VNTOSON(vn);
	r = sonode->sockops->connect(vn, addr, addrlen);
	if (r == 0)
		sonode->state = kSockConnected;
	return r;
}

int
sock_listen(vnode_t *vn, uint8_t backlog)
{
	struct socknode *sonode;
	int r;
	kassert(vn->ops == &sock_vnops);
	sonode = VNTOSON(vn);
	r = sonode->sockops->listen(vn, backlog);
	if (r == 0)
		sonode->state = kSockListening;
	return r;
}

int sock_pair(vnode_t *vn1, vnode_t *vn2)
{
	struct socknode *sock = VNTOSON(vn1);

	if (sock->sockops->pair == NULL)
		return -ENOSYS;

	return sock->sockops->pair(vn1, vn2);
}

int
sock_sendmsg(vnode_t *vn, struct msghdr *msg, int flags)
{
	struct socknode *sock = VNTOSON(vn);

	kassert(vn->ops == &sock_vnops);
	kassert(msg->msg_iovlen == 1);

	return sock->sockops->sendmsg(vn, msg, flags);
}

int
sock_recvmsg(vnode_t *vn, struct msghdr *msg, int flags)
{
	struct socknode *sock = VNTOSON(vn);
	kwaitstatus_t w;
	ipl_t ipl;

	kassert(vn->ops == &sock_vnops);
	kassert(msg->msg_iovlen > 0);

again:
#if DEBUG_SOCKFS == 1
	kdprintf("Attempt to receive (max %lu bytes)\n",
	    msg->msg_iov[0].iov_len);
#endif

	w = ke_wait(&sock->read_evobj, "sock_read:sock->read_evobj", false,
	    false, flags & MSG_DONTWAIT ? 0 : -1);
	if (w == kKernWaitStatusTimedOut)
		return -EAGAIN;
	kassert(w == kKernWaitStatusOK);

	ipl = ke_spinlock_acquire(&sock->lock);
	if (!sock_ready_for_read(sock)) {
		ke_spinlock_release(&sock->lock, ipl);
		goto again;
	}

	kassert(msg->msg_iovlen == 1);

	/* lmao */
	return sock->sockops->recv(vn, msg->msg_iov[0].iov_base,
	    msg->msg_iov[0].iov_len, flags, ipl);
}

/* review the below! these were made ages ago */

int
sock_accept(vnode_t *vn, struct sockaddr *addr, socklen_t *addrlen,
    krx_out vnode_t **out)
{
	struct socknode *sock = VNTOSON(vn), *newsock;
	ipl_t ipl;

again:
	ipl = ke_spinlock_acquire(&sock->lock);
	newsock = STAILQ_FIRST(&sock->accept_stailq);

	if (newsock == NULL) {
		ke_spinlock_release(&sock->lock, ipl);
		ke_wait(&sock->accept_evobj,
		    "sock_tcp_accept:sock->accept_evobj", false, false, -1);
		goto again;
	}

	STAILQ_REMOVE_HEAD(&sock->accept_stailq, accept_stailq_entry);
	if (STAILQ_EMPTY(&sock->accept_stailq))
		ke_event_clear(&sock->accept_evobj);
	ke_spinlock_release(&sock->lock, ipl);

	*out = newsock->vnode;

	return newsock->sockops->accept(vn, addr, addrlen);
}

int
sock_init(struct socknode *sock, struct socknodeops *ops)
{
	vnode_t *vn;

	sock->sockops = ops;
	sock->state = kSockInitialised;
	ke_spinlock_init(&sock->lock);
	LIST_INIT(&sock->polllist.pollhead_list);
	ke_event_init(&sock->read_evobj, false);
	ke_event_init(&sock->accept_evobj, false);
	STAILQ_INIT(&sock->accept_stailq);

	vn = kmem_alloc(sizeof(*vn));
	vn->type = VSOCK;
	vn->data = (uintptr_t)sock;
	vn->ops = &sock_vnops;
	sock->vnode = vn;
	return 0;
}

static int
sock_chpoll(vnode_t *vn, struct pollhead *ph, enum chpoll_kind kind)
{
	struct socknode *sock = VNTOSON(vn);
	int r = 0;
	ipl_t ipl = ke_spinlock_acquire(&sock->lock);

	switch (kind) {
	case kChPollAdd:
#if DEBUG_SOCKFS == 1
		kdprintf("%d: [%p(%s)] Chpoll Add %p (events: 0x%x).\n",
		    ps_curproc()->id, sock, sockstate_str[sock->state], ph,
		    ph->event.events);
#endif

		if (ph->event.events & EPOLLOUT) {
			/* HACK! for now just say yes. */
			ph->revents = EPOLLOUT;
			r = 1;
		}

		if (ph->event.events & EPOLLIN &&
		    (sock_ready_for_read(sock) ||
			!STAILQ_EMPTY(&sock->accept_stailq))) {
			ph->revents |= EPOLLIN;
			r = 1;
		}

		if (r != 1) {
			struct pollhead *aph;

			LIST_FOREACH (aph, &sock->polllist.pollhead_list,
			    polllist_entry) {
				kassert(aph != ph);
			}

			LIST_INSERT_HEAD(&sock->polllist.pollhead_list, ph,
			    polllist_entry);
		}

		break;

	case kChPollChange:
		kfatal("Unimplemented");

	case kChPollRemove:
#if DEBUG_SOCKFS == 1
		kdprintf("%d: [%p(%s)] Chpoll Rem %p (events: 0x%x).\n",
		    ps_curproc()->id, sock, sockstate_str[sock->state], ph,
		    ph->event.events);
#endif

		LIST_REMOVE(ph, polllist_entry);

		break;
	}

	ke_spinlock_release(&sock->lock, ipl);

	return r;
}

int
sock_read(vnode_t *vn, void *buf, size_t nbyte, off_t off, int flags)
{
	struct socknode *sock = VNTOSON(vn);
	kwaitstatus_t w;
	ipl_t ipl;

	kassert(vn->ops == &sock_vnops);

again:
#if DEBUG_SOCKFS == 1
	kdprintf("Attempt to read (max %lu bytes)\n", nbyte);
#endif

	/* modified for nonblock - fix as with recvmsg pls. */
	w = ke_wait(&sock->read_evobj, "sock_read:sock->read_evobj", false,
	    false, flags & O_NONBLOCK ? 0 : -1);
	if (w == kKernWaitStatusTimedOut)
		return -EAGAIN;
	kassert(w == kKernWaitStatusOK);

	ipl = ke_spinlock_acquire(&sock->lock);
	if (!sock_ready_for_read(sock)) {
		ke_spinlock_release(&sock->lock, ipl);
		goto again;
	}

	return sock->sockops->recv(vn, buf, nbyte, 0, ipl);
}

int
sock_write(vnode_t *vn, void *buf, size_t nbyte, off_t off, int flags)
{
	struct socknode *sonode;

	kassert(vn->ops == &sock_vnops);
	sonode = VNTOSON(vn);

	return sonode->sockops->send(vn, buf, nbyte);
}

int
sock_ioctl(vnode_t *vn, unsigned long op, void *arg)
{
	struct socknode *sonode;

	kassert(vn->ops == &sock_vnops);
	sonode = VNTOSON(vn);

	kassert(sonode->sockops->ioctl != NULL);
	return sonode->sockops->ioctl(vn, op, arg);
}

int
sock_close(vnode_t *vn)
{
	struct socknode *sonode;

	kassert(vn->ops == &sock_vnops);
	sonode = VNTOSON(vn);

	if (sonode->sockops->close != NULL)
		return sonode->sockops->close(vn);

	return 0;
}

struct vnops sock_vnops = {
	.read = sock_read,
	.write = sock_write,
	.ioctl = sock_ioctl,
	.close = sock_close,
	.chpoll = sock_chpoll,
};
