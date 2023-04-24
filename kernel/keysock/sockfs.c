/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#include "executive/epoll.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "keysock/sockfs.h"

#define VNTOSON(VN) ((struct socknode *)(VN)->data)

static bool
sock_ready_for_read(struct socknode *sock)
{
	return ke_wait(&sock->read_evobj, "", false, false, 0) ==
	    kKernWaitStatusOK;
}

static struct socknodeops *
sock_ops(struct socknode *sonode)
{
	switch (sonode->domain) {
	case PF_LOCAL:
		return &unix_soops;

	default:
		kfatal("No sonodeops for domain %d\n", sonode->domain);
	}
}

int
sock_event_raise(struct socknode *sock, int events)
{
	struct pollhead *ph;

	LIST_FOREACH (ph, &sock->polllist.pollhead_list, polllist_entry) {
		pollhead_raise(ph, events);
	}

	return 0;
}

int
sock_create(int domain, int type, int protocol, vnode_t **out)
{
	switch (domain) {
	case PF_LOCAL:
		kprintf("Create a Unix socket, type %d proto %d\n", type,
		    protocol);
		return unix_soops.create(out, domain, type, protocol);

	default:
		kdprintf("Unsupported domain %d\n", domain);
		return -EAFNOSUPPORT;
	}
}

int
sock_bind(vnode_t *vn, const struct sockaddr *addr, socklen_t addrlen)
{
	struct socknode *sonode;
	kassert(vn->ops = &sock_vnops);
	sonode = VNTOSON(vn);
	return sock_ops(sonode)->bind(vn, addr, addrlen);
}

int
sock_connect(vnode_t *vn, const struct sockaddr *addr, socklen_t addrlen)
{
	struct socknode *sonode;
	kassert(vn->ops = &sock_vnops);
	sonode = VNTOSON(vn);
	return sock_ops(sonode)->connect(vn, addr, addrlen);
}

int
sock_listen(vnode_t *vn, uint8_t backlog)
{
	struct socknode *sonode;
	kassert(vn->ops = &sock_vnops);
	sonode = VNTOSON(vn);
	return sock_ops(sonode)->listen(vn, backlog);
}

int
sock_recvmsg(vnode_t *vn, struct msghdr *msg, int flags)
{
	struct socknode *sock = VNTOSON(vn);
	kwaitstatus_t w;
	ipl_t ipl;

	kassert(vn->ops = &sock_vnops);
	kassert(msg->msg_iovlen > 0);

again:
	/* modified for NONBLOCK - todo set/getsockopt and O_NONBLOCK */
	w = ke_wait(&sock->read_evobj,
		    "sock_read:sock->read_evobj", false, false, 0);
	if (w == kKernWaitStatusTimedOut)
		return -EAGAIN;
	kassert(w == kKernWaitStatusOK);

	ipl = ke_spinlock_acquire(&sock->lock);
	if (!sock_ready_for_read(sock)) {
		ke_spinlock_release(&sock->lock, ipl);
		goto again;
	}

	/* lmao */
	return sock->sockops->recv(vn, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, ipl);
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
		if (ph->event.events & EPOLLOUT) {
			/* HACK! for now just say yes. */
			ph->revents = EPOLLOUT;
			r = 1;
		}

		if (ph->event.events & EPOLLIN && sock_ready_for_read(sock)) {
			ph->revents |= EPOLLIN;
			r = 1;
		}
		if (r != 1)
			LIST_INSERT_HEAD(&sock->polllist.pollhead_list, ph,
			    polllist_entry);

		break;

	case kChPollChange:
		kfatal("Unimplemented");

	case kChPollRemove:
		LIST_REMOVE(ph, polllist_entry);
		break;
	}

	ke_spinlock_release(&sock->lock, ipl);

	return r;
}

int
sock_read(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	struct socknode *sock = VNTOSON(vn);
	kwaitstatus_t w;
	ipl_t ipl;

	kassert(vn->ops = &sock_vnops);

again:
	w = ke_wait(&sock->read_evobj,
		    "sock_read:sock->read_evobj", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	ipl = ke_spinlock_acquire(&sock->lock);
	if (!sock_ready_for_read(sock)) {
		ke_spinlock_release(&sock->lock, ipl);
		goto again;
	}

	return sock->sockops->recv(vn, buf, nbyte, ipl);
}

int
sock_write(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	struct socknode *sonode;

	kassert(vn->ops = &sock_vnops);
	sonode = VNTOSON(vn);

	return sonode->sockops->send(vn, buf, nbyte);
}

struct vnops sock_vnops = {
	.chpoll = sock_chpoll,
	.read = sock_read,
	.write = sock_write,
};