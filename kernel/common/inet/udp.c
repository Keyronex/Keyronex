/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 30 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file udp.c
 * @brief User datagram protocol implementation.
 */

#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/tihdr.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <inet/ip.h>

void reply_error_ack(queue_t *, mblk_t *, int err_prim, int unix_error);

static int udp_ipv4_ropen(queue_t *, void *devp);
static int udp_ipv6_ropen(queue_t *, void *devp);
static void udp_rclose(queue_t *);
static void udp_wput(queue_t *, mblk_t *);

struct qinit udp_ipv4_rinit = {
	.qopen = udp_ipv4_ropen,
	.qclose = udp_rclose,
};

struct qinit udp_ipv6_rinit = {
	.qopen = udp_ipv6_ropen,
	.qclose = udp_rclose,
};

struct qinit udp_winit = {
	.putp = udp_wput,
};

struct streamtab udp_ipv4_streamtab = {
	.rinit = &udp_ipv4_rinit,
	.winit = &udp_winit,
};

struct streamtab udp_ipv6_streamtab = {
	.rinit = &udp_ipv6_rinit,
	.winit = &udp_winit,
};

typedef struct udp {
	RCULIST_ENTRY(udp) pcb_entry;
	krcu_entry_t rcu_entry;
	kspinlock_t lock;
	bool	ipv6: 1,
		reuseattr: 1,
		recvpktinfo: 1,
		bound: 1;
	union {
		struct sockaddr_in laddr_in4;
		struct sockaddr_in6 laddr_in6;
	};
	queue_t *rq;
} udp_t;


static kspinlock_t udp_bind_lock = KSPINLOCK_INITIALISER;
static RCULIST_HEAD(, udp) udp_ipv4_pcb_list;

static uint16_t udp_ephemeral_rotor = 49152;

static in_port_t
udp_alloc_ephemeral_locked(void)
{
	uint16_t start;

	kassert(ke_spinlock_held(&udp_bind_lock));

	start = udp_ephemeral_rotor;

	do {
		udp_t *u;
		bool in_use = false;
		in_port_t candidate;

		candidate = htons(udp_ephemeral_rotor);

		if (udp_ephemeral_rotor == 65535)
			udp_ephemeral_rotor = 49152;
		else
		 	udp_ephemeral_rotor++;

		RCULIST_FOREACH(u, &udp_ipv4_pcb_list, pcb_entry) {
			if (u->laddr_in4.sin_port == candidate) {
				in_use = true;
				break;
			}
		}

		if (!in_use)
			return candidate;

	} while (udp_ephemeral_rotor != start);

	return 0;
}

static int
udp_ipv4_ropen(queue_t *rq, void *devp)
{
	udp_t *udp = kmem_zalloc(sizeof(udp_t));
	udp->rq = rq;
	rq->ptr = rq->other->ptr = udp;
	return 0;
}

static int
udp_ipv6_ropen(queue_t *rq, void *devp)
{
	udp_t *udp = kmem_zalloc(sizeof(udp_t));
	udp->ipv6 = true;
	udp->rq = rq;
	rq->ptr = rq->other->ptr = udp;
	return 0;
}

static void
udp_rcu_free(void *ptr)
{
	udp_t *udp = ptr;
	kmem_free(udp, sizeof(*udp));
}

static void
udp_rclose(queue_t *rq)
{
	udp_t *udp = rq->ptr;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&udp_bind_lock);
	if (udp->bound)
		RCULIST_REMOVE(udp, pcb_entry);
	ke_spinlock_exit(&udp_bind_lock, ipl);

	ke_rcu_call(&udp->rcu_entry, udp_rcu_free, udp);
}

static void
udp_wput_bind_req(queue_t *wq, mblk_t *mp, struct T_bind_req *br)
{
	udp_t *udp = wq->ptr;
	struct sockaddr_in *sin;
	struct T_bind_ack *ack;
	struct in_addr laddr;
	ipl_t ipl;

	if (br->ADDR_length < (int)sizeof(struct sockaddr_in))
		return reply_error_ack(wq, mp, T_BIND_REQ, EINVAL);

	sin = (struct sockaddr_in *)&br->ADDR;

	if (sin->sin_family != AF_INET)
		return reply_error_ack(wq, mp, T_BIND_REQ, EAFNOSUPPORT);

	laddr = sin->sin_addr;

	ipl = ke_spinlock_enter(&udp_bind_lock);

	if (sin->sin_port == 0) {
		sin->sin_port = udp_alloc_ephemeral_locked();
		if (sin->sin_port == 0) {
			ke_spinlock_exit(&udp_bind_lock, ipl);
			return reply_error_ack(wq, mp, T_BIND_REQ, EADDRINUSE);
		}
	} else {
		udp_t *u;

		RCULIST_FOREACH(u, &udp_ipv4_pcb_list, pcb_entry) {
			if (u->laddr_in4.sin_port != sin->sin_port)
				continue;

			if (u->laddr_in4.sin_addr.s_addr == laddr.s_addr) {
				ke_spinlock_exit(&udp_bind_lock, ipl);
				return reply_error_ack(wq, mp, T_BIND_REQ,
				    EADDRINUSE);
			}

			if ((u->laddr_in4.sin_addr.s_addr == INADDR_ANY ||
			    laddr.s_addr == INADDR_ANY) &&
			    (!u->reuseattr || !udp->reuseattr)) {
				ke_spinlock_exit(&udp_bind_lock, ipl);
				return reply_error_ack(wq, mp, T_BIND_REQ,
				    EADDRINUSE);
			}
		}
	}

	udp->laddr_in4 = *sin;
	udp->bound = true;
	RCULIST_INSERT_HEAD(&udp_ipv4_pcb_list, udp, pcb_entry);

	ke_spinlock_exit(&udp_bind_lock, ipl);

	mp->db->type = M_PCPROTO;
	ack = (struct T_bind_ack *)mp->rptr;
	ack->PRIM_type = T_BIND_ACK;
	str_qreply(wq, mp);
}

static void
udp_wput_optmgmt_req(queue_t *wq, mblk_t *mp, struct T_optmgmt_req *req)
{
	struct T_optmgmt_ack *ack;
	udp_t *udp = wq->ptr;
	struct opthdr *opt;
	ipl_t ipl;
	size_t msg_len = (size_t)(mp->wptr - mp->rptr);

	if (req->OPT_length < sizeof(struct opthdr) ||
	    req->OPT_offset + req->OPT_length > msg_len)
		return reply_error_ack(wq, mp, req->PRIM_type, EINVAL);

	if (req->MGMT_flags != T_NEGOTIATE)
		return reply_error_ack(wq, mp, req->PRIM_type, EOPNOTSUPP);

	opt = (struct opthdr *)(mp->rptr + req->OPT_offset);

	switch (opt->level) {
	case SOL_SOCKET:
		switch (opt->name) {
		case SO_REUSEADDR:
			if (opt->len != sizeof(int))
				return reply_error_ack(wq, mp, req->PRIM_type,
				    EINVAL);
			ipl = ke_spinlock_enter(&udp->lock);
			udp->reuseattr = *(int *)OPTVAL(opt) != 0;
			ke_spinlock_exit(&udp->lock, ipl);
			break;

		default:
			kdprintf("udp: SOL_SOCKET unhandled option %zu\n",
			    opt->name);
			return reply_error_ack(wq, mp, req->PRIM_type,
			    ENOPROTOOPT);
		}

		break;

	case IPPROTO_IP: {
		switch (opt->name) {
		case IP_PKTINFO:
			if (opt->len != sizeof(int))
				return reply_error_ack(wq, mp, req->PRIM_type,
				    EINVAL);
			ipl = ke_spinlock_enter(&udp->lock);
			udp->recvpktinfo = *(int *)OPTVAL(opt) != 0;
			ke_spinlock_exit(&udp->lock, ipl);
			break;

		default:
			kdprintf("udp: IPPROTO_IP unhandled option %zu\n",
			    opt->name);
			return reply_error_ack(wq, mp, req->PRIM_type,
			    ENOPROTOOPT);
		}

		break;
	}

	default:
		return reply_error_ack(wq, mp, req->PRIM_type, ENOPROTOOPT);
	}

	mp->db->type = M_PCPROTO;
	ack = (struct T_optmgmt_ack *)mp->rptr;
	ack->PRIM_type = T_OPTMGMT_ACK;
	str_qreply(wq, mp);
}

static void
udp_wput(queue_t *wq, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_PROTO:
	case M_PCPROTO: {
		union T_primitives *prim = (union T_primitives *)mp->rptr;

		switch (prim->type) {
		case T_BIND_REQ:
			return udp_wput_bind_req(wq, mp, &prim->bind_req);

		case T_OPTMGMT_REQ:
			return udp_wput_optmgmt_req(wq, mp, &prim->optmgmt_req);

		default:
			kfatal("udp_wput: unhandled primitive type %d\n",
			    prim->type);
			ktodo();
		}

		break;
	}

	case M_IOCTL: {
		struct strioctl *ioc = (typeof(ioc))mp->rptr;

		switch (ioc->ic_cmd) {
		case SIOCGIFMTU:
			return ip_uwput_ioctl_sgif(wq, mp);
		}

		break;
	}

	default:
		kfatal("udp_wput: unhandled message type %d\n", mp->db->type);
	}
}

static void
udp_deliver(udp_t *udp, ip_if_t *ifp, ip_rxattr_t *attr,
    const struct udphdr *uh, mblk_t *payload)
{
	struct T_unitdata_ind *ind;
	struct sockaddr_in *src;
	mblk_t *hdr_mp;
	size_t pktinfo_space, hdr_size;

	ke_spinlock_enter_nospl(&udp->lock);

	pktinfo_space = udp->recvpktinfo ?
	    TI_OPT_SPACE(sizeof(struct in_pktinfo)) : 0;
	hdr_size = sizeof(struct T_unitdata_ind) + pktinfo_space;

	hdr_mp = str_allocb(hdr_size);
	if (hdr_mp == NULL) {
		str_freemsg(payload);
		return;
	}

	hdr_mp->db->type = M_PROTO;
	hdr_mp->wptr = hdr_mp->rptr + hdr_size;

	ind = (struct T_unitdata_ind *)hdr_mp->rptr;
	memset(ind, 0, hdr_size);
	ind->PRIM_type = T_UNITDATA_IND;
	ind->SRC_length = sizeof(struct sockaddr_in);
	src = (struct sockaddr_in *)&ind->SRC;
	src->sin_family = AF_INET;
	src->sin_port = uh->uh_sport;
	src->sin_addr = attr->l3hdr.ip4->ip_src;

	if (udp->recvpktinfo) {
		struct T_opthdr *toph;
		struct in_pktinfo *pktinfo;

		ind->OPT_length = pktinfo_space;
		ind->OPT_offset = sizeof(struct T_unitdata_ind);

		toph = (struct T_opthdr *)(hdr_mp->rptr + ind->OPT_offset);
		toph->len = TI_OPT_LEN(sizeof(struct in_pktinfo));
		toph->level = IPPROTO_IP;
		toph->name = IP_PKTINFO;
		toph->status = 0;

		pktinfo = (struct in_pktinfo *)TI_OPT_DATA(toph);
		pktinfo->ipi_ifindex = ifp->muxid;
		pktinfo->ipi_spec_dst = attr->ifa.ifa != NULL ?
		    attr->ifa.ifa->addr.in.sin_addr : attr->l3hdr.ip4->ip_dst;
		pktinfo->ipi_addr = attr->l3hdr.ip4->ip_dst;
	}

	hdr_mp->cont = payload;
	str_ingress_putq(udp->rq->stdata, hdr_mp);
	ke_spinlock_exit_nospl(&udp->lock);
}

void
udp_ipv4_input(ip_if_t *ifp, mblk_t *mp, ip_rxattr_t *attr)
{
	const struct ip *iph = attr->l3hdr.ip4;
	const struct udphdr *uh;
	size_t avail, udp_len;
	udp_t *last = NULL;
	bool exact;
	udp_t *udp;

	/* this is entirely within the context of an RCU critical section */

	avail = mp->wptr - mp->rptr;
	if (avail < sizeof(*uh)) {
		str_freemsg(mp);
		return;
	}

	uh = (const struct udphdr *)mp->rptr;
	udp_len = ntohs(uh->uh_ulen);

	if (udp_len < sizeof(*uh) || avail < udp_len) {
		str_freemsg(mp);
		return;
	}

	mp->wptr = mp->rptr + udp_len;
	mp->rptr += sizeof(*uh);

	RCULIST_FOREACH(udp, &udp_ipv4_pcb_list, pcb_entry) {
		if (udp->laddr_in4.sin_port != uh->uh_dport)
			continue;
		if (udp->laddr_in4.sin_addr.s_addr != iph->ip_dst.s_addr)
			continue;

		if (last != NULL) {
			mblk_t *copy = str_copymsg(mp);
			if (copy)
				udp_deliver(last, ifp, attr, uh, copy);
		}
		last = udp;
		exact = true;
	}

	if (!exact) {
		RCULIST_FOREACH(udp, &udp_ipv4_pcb_list, pcb_entry) {
			if (udp->laddr_in4.sin_port != uh->uh_dport)
				continue;
			if (udp->laddr_in4.sin_addr.s_addr != INADDR_ANY)
				continue;

			if (last != NULL) {
				mblk_t *copy = str_copymsg(mp);
				if (copy != NULL)
					udp_deliver(last, ifp, attr, uh, copy);
			}
			last = udp;
		}
	}

	if (last != NULL)
		udp_deliver(last, ifp, attr, uh, mp);
	else
		str_freemsg(mp);
}
