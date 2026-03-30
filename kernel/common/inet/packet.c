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
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/tihdr.h>

#include <inet/ip.h>
#include <linux/if_packet.h>

static int packet_ropen(queue_t *, void *devp);
static void packet_wput(queue_t *, mblk_t *);

struct qinit packet_rinit = {
	.qopen = packet_ropen,
};

struct qinit packet_winit = {
	.putp = packet_wput,
};

struct streamtab packet_streamtab = {
	.rinit = &packet_rinit,
	.winit = &packet_winit,
};

typedef struct packet {
	bpf_listener_t bpf_listener;
} packet_t;

static int
packet_ropen(queue_t *rq, void *devp)
{
	packet_t *pkt;

	pkt = kmem_zalloc(sizeof(packet_t));
	rq->ptr = rq->other->ptr = pkt;

	return 0;
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

	br->PRIM_type = T_BIND_ACK;
	str_qreply(wq, mp);
}

static void
packet_wput(queue_t *wq, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_PROTO: {
		union T_primitives *prim = (typeof(prim))mp->rptr;
		switch (prim->type) {
		case T_BIND_REQ:
			packet_wput_bind_req(wq, mp, &prim->bind_req);
			break;

		default:
			ktodo();
		}
		break;
	}

	default:
		ktodo();
	}
}

void
bpf_input(bpf_listener_t *bpf, mblk_t *mp)
{
	packet_t *pkt = (typeof(pkt))bpf;

	kdprintf("bpf_input\n");
}
