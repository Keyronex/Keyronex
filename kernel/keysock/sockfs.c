/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#include "executive/epoll.h"
#include "kdk/kmem.h"
#include "keysock/sockfs.h"

#define VNTOSON(VN) ((struct socknode *)(VN)->data)

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
	ke_event_init(&sock->accept_evobj, false);
	STAILQ_INIT(&sock->accept_stailq);

	vn = kmem_alloc(sizeof(*vn));
	vn->type = VSOCK;
	vn->data = (uintptr_t)sock;
	vn->ops = &sock_vnops;
	sock->vnode = vn;
	return 0;
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

static int
sock_chpoll(vnode_t *vn, struct pollhead *ph, enum chpoll_kind kind)
{
	struct socknode *sock = VNTOSON(vn);

	if (kind == kChPollAdd) {
		LIST_INSERT_HEAD(&sock->polllist.pollhead_list, ph,
		    polllist_entry);
		return 0;
	} else if (kind == kChPollChange) {
		kfatal("Unimplemented");
	} else if (kind == kChPollRemove) {
		LIST_REMOVE(ph, polllist_entry);
		return 0;
	}

	kfatal("unreached");
}

struct vnops sock_vnops = {
	.chpoll = sock_chpoll,
};