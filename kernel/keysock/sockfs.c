/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#include "executive/epoll.h"
#include "kdk/kmem.h"
#include "keysock/sockfs.h"

#define VNTOSON(VN) ((struct socknode *)(VN)->data)

int
sock_init(struct socknode *sock, struct socknodeops *ops)
{
	vnode_t *vn;
	sock->sockops = ops;
	LIST_INIT(&sock->polllist.pollhead_list);
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