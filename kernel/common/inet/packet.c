/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 30 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file packet.c
 * @brief Transport provider for PF_PACKET sockets.
 */

#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/tihdr.h>

#include <inet/ip.h>
#include <linux/filter.h>
#include <linux/if_packet.h>

static int packet_ropen(queue_t *, void *devp);
static void packet_rclose(queue_t *);
static void packet_wput(queue_t *, mblk_t *);

struct qinit packet_rinit = {
	.qopen = packet_ropen,
	.qclose = packet_rclose,
	.putp = str_putnext,
};

struct qinit packet_winit = {
	.putp = packet_wput,
};

struct streamtab packet_streamtab = {
	.rinit = &packet_rinit,
	.winit = &packet_winit,
};

typedef struct packet {
	kspinlock_t lock;
	bpf_listener_t bpf_listener;
	ip_if_t *bound_ifp;
	queue_t *bot_rq;
	struct sock_filter *bpf;
	uint32_t bpf_len;
} packet_t;

static int
packet_ropen(queue_t *rq, void *devp)
{
	packet_t *pkt;

	pkt = kmem_zalloc(sizeof(packet_t));
	ke_spinlock_init(&pkt->lock);
	pkt->bpf = NULL;
	pkt->bpf_len = 0;
	pkt->bot_rq = rq;
	rq->ptr = rq->other->ptr = pkt;

	return 0;
}

static void
packet_rclose(queue_t *rq)
{
	packet_t *pkt = rq->ptr;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&pkt->lock);
	/* FIXME: RCU freeing, detach etc */
	pkt->bot_rq = NULL;
	ke_spinlock_exit(&pkt->lock, ipl);
}

static void
packet_wput_bind_req(queue_t *wq, mblk_t *mp, struct T_bind_req *br)
{
	struct sockaddr_ll *sll = (typeof(sll))&br->ADDR;
	packet_t *pkt = wq->ptr;
	ip_if_t *ifp;
	int r;

	if (sll->sll_family != AF_PACKET)
		kfatal("packet bind: invalid family %d\n", sll->sll_family);

	if (sll->sll_protocol != htons(ETH_P_ALL))
		kfatal("packet bind: unsupported protocol 0x%xx",
		    ntohs(sll->sll_protocol));

	ifp = ip_if_lookup_by_muxid(sll->sll_ifindex);
	if (ifp == NULL)
		kfatal("packet bind: no ifp");

	r = ip_if_bpf_attach(ifp, &pkt->bpf_listener);
	if (r != 0)
		kfatal("packet bind: failed to attach BPF listener");

	pkt->bound_ifp = ifp;

	br->PRIM_type = T_BIND_ACK;
	str_qreply(wq, mp);
}

void
reply_error_ack(queue_t *wq, mblk_t *mp, int err_prim, int unix_error)
{
	struct T_error_ack *ack;

	kassert (mp->wptr - mp->rptr >= sizeof(struct T_error_ack));

	mp->db->type = M_PCPROTO;
	ack = (typeof(ack))mp->rptr;
	ack->PRIM_type = T_ERROR_ACK;
	ack->ERROR_prim = err_prim;
	ack->UNIX_error = unix_error;
	mp->wptr = mp->rptr + sizeof(struct T_error_ack);

	str_qreply(wq, mp);
}

static int
packet_attach_filter(packet_t *pkt, const struct sock_fprog *fprog)
{
	struct sock_filter *bpf;
	ipl_t ipl;

	if (fprog->len == 0 || fprog->len > 512)
		return -EINVAL;

	bpf = kmem_alloc(fprog->len * sizeof(struct sock_filter));
	if (bpf == NULL)
		return -ENOMEM;

	/* TODO: wrong context for a copyin - needs to happen in sockfs! */
	if (memcpy_from_user(bpf, fprog->filter,
		fprog->len * sizeof(struct sock_filter)) != 0) {
		kmem_free(bpf, fprog->len * sizeof(struct sock_filter));
		return -EFAULT;
	}

	ipl = ke_spinlock_enter(&pkt->lock);
	if (pkt->bpf != NULL) {
		ke_spinlock_exit(&pkt->lock, ipl);
		kmem_free(bpf, fprog->len * sizeof(struct sock_filter));
		return -EBUSY; /* ? right error */
	}

	pkt->bpf = bpf;
	pkt->bpf_len = fprog->len;

	ke_spinlock_exit(&pkt->lock, ipl);

	kdprintf("packet: attached SO_ATTACH_FILTER with %u instructions\n",
	    fprog->len);

	return 0;
}

static void
packet_wput_optmgmt_req(queue_t *wq, mblk_t *mp, struct T_optmgmt_req *req)
{
	struct T_optmgmt_ack *ack;
	packet_t *pkt = wq->ptr;
	struct opthdr *opt;
	size_t msg_len = (size_t)(mp->wptr - mp->rptr);
	int r;

	if (req->OPT_length < sizeof(struct opthdr) ||
	    req->OPT_offset + req->OPT_length > msg_len)
		return reply_error_ack(wq, mp, req->PRIM_type, EINVAL);

	if (req->MGMT_flags != T_NEGOTIATE)
		return reply_error_ack(wq, mp, req->PRIM_type, EOPNOTSUPP);

	opt = (struct opthdr *)(mp->rptr + req->OPT_offset);

	if (opt->level != SOL_SOCKET)
		return reply_error_ack(wq, mp, req->PRIM_type, ENOPROTOOPT);

	switch (opt->name) {
	case SO_ATTACH_FILTER:
		if (opt->len != sizeof(struct sock_fprog))
			return reply_error_ack(wq, mp, req->PRIM_type, EINVAL);
		r = packet_attach_filter(pkt,
		    (const struct sock_fprog *)OPTVAL(opt));
		if (r != 0)
			return reply_error_ack(wq, mp, req->PRIM_type, -r);
		break;

	case SO_LOCK_FILTER:
		if (opt->len != sizeof(int))
			return reply_error_ack(wq, mp, req->PRIM_type, EINVAL);
		kdprintf("packet: SO_LOCK_FILTER set to %d\n",
		    *(int *)OPTVAL(opt));
		break;

	default:
		return reply_error_ack(wq, mp, req->PRIM_type, ENOPROTOOPT);
	}

	mp->db->type = M_PCPROTO;
	ack = (struct T_optmgmt_ack *)mp->rptr;
	ack->PRIM_type = T_OPTMGMT_ACK;
	str_qreply(wq, mp);
}

static void
packet_wput(queue_t *wq, mblk_t *mp)
{
	struct packet *pkt = wq->ptr;

	switch (mp->db->type) {
	case M_DATA: {
		ipl_t ipl = spldisp();
		pkt->bound_ifp->nic_wput(pkt->bound_ifp->nic_data, mp);
		splx(ipl);
		break;
	}

	case M_PROTO: {
		union T_primitives *prim = (typeof(prim))mp->rptr;
		switch (prim->type) {
		case T_BIND_REQ:
			return packet_wput_bind_req(wq, mp, &prim->bind_req);

		case T_OPTMGMT_REQ:
			return packet_wput_optmgmt_req(wq, mp, &prim->optmgmt_req);

		default:
			ktodo();
		}
		break;
	}

	default:
		ktodo();
	}
}

uint32_t bpf_filter(const struct sock_filter *, size_t, const mblk_t *);

void
bpf_input(bpf_listener_t *bpf, mblk_t *mp)
{
	packet_t *pkt = (typeof(pkt))containerof(bpf, packet_t, bpf_listener);
	mblk_t *nmp;

	nmp = str_copymsg(mp);
	if (nmp == NULL)
		return;

	ke_spinlock_enter_nospl(&pkt->lock);

	if (pkt->bot_rq == NULL) {
		ke_spinlock_exit_nospl(&pkt->lock);
		str_freemsg(nmp);
		return;
	}

	if (pkt->bpf != NULL) {
		uint32_t len = bpf_filter(pkt->bpf, pkt->bpf_len, nmp);
		size_t msglen = str_msgsize(nmp);
		kdprintf("bpf_input: new len %u (msgsize was %zu)\n", len,
		    msglen);
		if (len == 0) {
			str_freemsg(nmp);
			ke_spinlock_exit_nospl(&pkt->lock);
		} else if (len < msglen) {
			nmp->wptr = nmp->rptr + len;
		}
	}

	str_ingress_putq(pkt->bot_rq->stdata, nmp);

	ke_spinlock_exit_nospl(&pkt->lock);
}
