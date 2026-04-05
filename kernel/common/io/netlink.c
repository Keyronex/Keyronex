/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Mar 06 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file netlink.c
 * @brief Transport provider for NetLink.
 */

#include <linux/netlink.h>
#include <sys/stream.h>
#include <sys/errno.h>
#include <sys/tihdr.h>
#include <sys/kmem.h>
#include <sys/netlinksubr.h>

typedef struct nl_endp {
	int		protocol;
	uint32_t	nl_pid;
	uint32_t	nl_groups;
	queue_t		*rq;
	bool		bound;
} nl_endp_t;

static nl_handler_fn nl_handlers[32];

static atomic_uint nl_next_pid = 1;

static int  nl_ropen(queue_t *rq, void *);
static void nl_rclose(queue_t *rq);
static void nl_rput(queue_t *rq, mblk_t *);
static void nl_wput(queue_t *wq, mblk_t *);

static struct qinit nl_rinit = {
	.qopen = nl_ropen,
	.qclose = nl_rclose,
	.putp = nl_rput,
};

static struct qinit nl_winit = {
	.putp = nl_wput,
};

struct streamtab nl_streamtab = {
	.rinit = &nl_rinit,
	.winit = &nl_winit,
};

void
nl_register_protocol(int protocol, nl_handler_fn handler)
{
	kassert(protocol >= 0 && protocol <= 32);
	kassert(nl_handlers[protocol] == NULL);
	nl_handlers[protocol] = handler;
}

void
nl_send_error(queue_t *wq, struct nlmsghdr *orig_nlh, int error)
{
	mblk_t *rmp;
	struct nlmsghdr *nlh;
	struct nlmsgerr *nlerr;
	size_t len = NLMSG_SPACE(sizeof(struct nlmsgerr));

	rmp = str_allocb(len);
	if (rmp == NULL)
		return;

	nlh = (struct nlmsghdr *)rmp->rptr;
	nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nlmsgerr));
	nlh->nlmsg_type = NLMSG_ERROR;
	nlh->nlmsg_flags = 0;
	nlh->nlmsg_seq = orig_nlh->nlmsg_seq;
	nlh->nlmsg_pid = orig_nlh->nlmsg_pid;

	nlerr = (struct nlmsgerr *)NLMSG_DATA(nlh);
	nlerr->error = error;
	nlerr->msg = *orig_nlh;

	rmp->wptr = rmp->rptr + len;
	rmp->db->type = M_DATA;

	str_qreply(wq, rmp);
}

void
nl_send_done(queue_t *wq, uint32_t seq, uint32_t pid)
{
	mblk_t *rmp;
	struct nlmsghdr *nlh;
	size_t len = NLMSG_SPACE(0);

	rmp = str_allocb(len);
	if (rmp == NULL)
		return;

	nlh = (struct nlmsghdr *)rmp->rptr;
	nlh->nlmsg_len = NLMSG_LENGTH(0);
	nlh->nlmsg_type = NLMSG_DONE;
	nlh->nlmsg_flags = NLM_F_MULTI;
	nlh->nlmsg_seq = seq;
	nlh->nlmsg_pid = pid;

	rmp->wptr = rmp->rptr + len;
	rmp->db->type = M_DATA;

	str_qreply(wq, rmp);
}

static void
nl_reply_ok_ack(queue_t *wq, mblk_t *mp, enum T_prim correct_prim)
{
	struct T_ok_ack *oa = (typeof(oa))mp->rptr;

	kassert(mp->wptr - mp->rptr >= (ptrdiff_t)sizeof(*oa));

	oa->PRIM_type = T_OK_ACK;
	oa->CORRECT_prim = correct_prim;
	mp->wptr = mp->rptr + sizeof(*oa);
	mp->db->type = M_PCPROTO;

	str_qreply(wq, mp);
}

static void
nl_reply_error_ack(queue_t *wq, mblk_t *mp, enum T_prim error_prim,
    int unix_error)
{
	struct T_error_ack *ea = (typeof(ea))mp->rptr;

	kassert(mp->wptr - mp->rptr >= (ptrdiff_t)sizeof(*ea));

	ea->PRIM_type = T_ERROR_ACK;
	ea->ERROR_prim = error_prim;
	ea->UNIX_error = unix_error;
	mp->wptr = mp->rptr + sizeof(*ea);
	mp->db->type = M_PCPROTO;

	str_qreply(wq, mp);
}

static void
nl_reply_bind_ack(queue_t *wq, mblk_t *mp, nl_endp_t *ep)
{
	struct T_bind_ack *ba = (typeof(ba))mp->rptr;
	struct sockaddr_nl *snl;

	ba->PRIM_type = T_BIND_ACK;
	ba->ADDR_length = sizeof(struct sockaddr_nl);
	ba->CONIND_number = 0;

	snl = (struct sockaddr_nl *)&ba->ADDR;
	snl->nl_family = AF_NETLINK;
	snl->nl_pad = 0;
	snl->nl_pid = ep->nl_pid;
	snl->nl_groups = ep->nl_groups;

	mp->wptr = mp->rptr + sizeof(struct T_bind_ack);
	mp->db->type = M_PCPROTO;

	str_qreply(wq, mp);
}

static int
nl_ropen(queue_t *rq, void *arg)
{
	nl_endp_t *ep;

	ep = kmem_alloc(sizeof(*ep));
	if (ep == NULL)
		return -ENOMEM;

	ep->protocol = (int)(uintptr_t)arg;
	ep->nl_pid = 0;
	ep->nl_groups = 0;
	ep->rq = rq;
	ep->bound = false;

	rq->ptr = rq->other->ptr = ep;

	return 0;
}

static void
nl_rclose(queue_t *rq)
{
	nl_endp_t *ep = rq->ptr;

	if (ep != NULL) {
		kmem_free(ep, sizeof(*ep));
		rq->ptr = rq->other->ptr = NULL;
	}
}

static void
nl_rput(queue_t *rq, mblk_t *mp)
{
	str_putnext(rq, mp);
}

static void
nl_wput_bind_req(queue_t *wq, mblk_t *mp)
{
	nl_endp_t *ep = wq->ptr;
	struct T_bind_req *br = (typeof(br))mp->rptr;
	struct sockaddr_nl *snl;

	if (ep->bound) {
		nl_reply_error_ack(wq, mp, T_BIND_REQ, EINVAL);
		return;
	}

	/*
	 * TODO: sockfs to lower sockaddr_nl for binds: need to make nl_pid =
	 * sys_getpid() initially, and to indicate to us that it's an auto-bind
	 * so if not free, find another...
	 */
	if (br->ADDR_length >= sizeof(struct sockaddr_nl)) {
		snl = (struct sockaddr_nl *)&br->ADDR;

		if (snl->nl_family != AF_NETLINK) {
			nl_reply_error_ack(wq, mp, T_BIND_REQ, EINVAL);
			return;
		}

		if (snl->nl_pid == 0)
			ep->nl_pid = atomic_fetch_add(&nl_next_pid, 1);
		else
			ep->nl_pid = snl->nl_pid;

		ep->nl_groups = snl->nl_groups;
	} else {
		ep->nl_pid = atomic_fetch_add(&nl_next_pid, 1);
		ep->nl_groups = 0;
	}

	ep->bound = true;

	nl_reply_bind_ack(wq, mp, ep);
}

static void
nl_wput_addr_req(queue_t *wq, mblk_t *mp)
{
	nl_endp_t *ep = wq->ptr;
	struct T_addr_ack *aa = (typeof(aa))mp->rptr;
	struct sockaddr_nl *snl;
	mblk_t *ackmp;

	str_freemsg(mp);

	ackmp = str_allocb(sizeof(struct T_addr_ack));
	if (ackmp == NULL) {
		nl_reply_error_ack(wq, mp, T_ADDR_REQ, ENOMEM);
		return;
	}

	ackmp->db->type = M_PCPROTO;
	ackmp->wptr += sizeof(struct T_addr_ack);
	aa = (struct T_addr_ack *)ackmp->rptr;
	aa->PRIM_type = T_ADDR_ACK;

	snl = (struct sockaddr_nl *)&aa->LOCADDR;
	snl->nl_family = AF_NETLINK;
	snl->nl_pad = 0;
	snl->nl_pid = ep->nl_pid;
	snl->nl_groups = ep->nl_groups;

	aa->REMADDR_length = 0;

	str_qreply(wq, mp);
}

static void
nl_dispatch_one(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	nl_endp_t *ep = wq->ptr;
	nl_handler_fn handler;
	int r;

	if (nlh->nlmsg_type == NLMSG_NOOP)
		return;

	handler = nl_handlers[ep->protocol];
	if (handler == NULL) {
		kfatal("NetLink: no handler for protocol 0x%x", ep->protocol);
		nl_send_error(wq, nlh, -ENOSYS);
		return;
	}

	r = handler(wq, mp, nlh);
	if (r != 0)
		nl_send_error(wq, nlh, r);
}

static void
nl_wput_data(queue_t *wq, mblk_t *mp)
{
	char *buf = (char *)mp->rptr;
	int remaining = mp->wptr - mp->rptr;
	struct nlmsghdr *nlh;

	if (remaining < (int)sizeof(struct nlmsghdr)) {
		str_freemsg(mp);
		return;
	}

	nlh = (struct nlmsghdr *)buf;
	while (NLMSG_OK(nlh, remaining)) {
		nl_dispatch_one(wq, mp, nlh);
		nlh = NLMSG_NEXT(nlh, remaining);
	}

	str_freemsg(mp);
}

static void
nl_wput(queue_t *wq, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_DATA:
		nl_wput_data(wq, mp);
		break;

	case M_PROTO:
	case M_PCPROTO: {
		union T_primitives *prim = (union T_primitives *)mp->rptr;

		switch (prim->type) {
		case T_BIND_REQ:
			return nl_wput_bind_req(wq, mp);

		case T_ADDR_REQ:
			return nl_wput_addr_req(wq, mp);

		default:
			kdprintf("netlink: unexpected TPI primitive %d\n",
			    prim->type);
			nl_reply_error_ack(wq, mp, prim->type, ENOTSUP);
			break;
		}
		break;
	}

	default:
		kdprintf("netlink: unexpected message type %d\n", mp->db->type);
		str_freemsg(mp);
		break;
	}
}
