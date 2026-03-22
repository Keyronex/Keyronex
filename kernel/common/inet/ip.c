/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Thu Mar 05 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ip.c
 * @brief IP driver.
 */

#include <sys/dlpi.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/k_thread.h>
#include <sys/k_wait.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/stream.h>
#include <sys/stropts.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>

#include <fs/devfs/devfs.h>
#include <inet/ip.h>
#include <inet/util.h>

#define SIOCSIFNAMEBYMUXID 0x89A0

static void ip_uwput(queue_t *, mblk_t *);

static int ip_lropen(queue_t *, void *);
static void ip_lrput(queue_t *, mblk_t *);

static struct qinit ip_urinit = {};

static struct qinit ip_uwinit = {
	.putp = ip_uwput,
};

static struct qinit ip_lrinit = {
	.qopen = ip_lropen,
	.putp = ip_lrput,
};

static struct qinit ip_lwinit = {

};

static struct streamtab ip_streamtab = {
	.rinit = &ip_urinit,
	.winit = &ip_uwinit,
	.muxrinit = &ip_lrinit,
	.muxwinit = &ip_lwinit,
};

static dev_ops_t ip_devops = {
	.streamtab = &ip_streamtab,
};

static void
ip_uwput_ioctl_sgif(queue_t *wq, mblk_t *mp)
{
	struct strioctl *ioc = (typeof(ioc))mp->rptr;
	struct ifreq *ifr = (typeof(ifr))ioc->ic_dp;

	switch (ioc->ic_cmd) {
	case SIOCGIFINDEX: {
		ip_if_t *intf = ip_if_lookup_by_name(ifr->ifr_name);
		if (intf == NULL) {
			mp->db->type = M_IOCNAK;
			return str_qreply(wq, mp);
		}

		ifr->ifr_ifindex = intf->muxid;
		mp->db->type = M_IOCACK;
		return str_qreply(wq, mp);
	}

	case SIOCGIFNAME: {
		ip_if_t *intf = ip_if_lookup_by_muxid(ifr->ifr_ifindex);
		if (intf == NULL) {
			mp->db->type = M_IOCNAK;
			return str_qreply(wq, mp);
		}

		strncpy(ifr->ifr_name, intf->name, IF_NAMESIZE);
		mp->db->type = M_IOCACK;
		return str_qreply(wq, mp);
	}

	case SIOCSIFNAMEBYMUXID: {
		ip_if_t *intf = ip_if_lookup_by_muxid(ifr->ifr_ifindex);
		if (intf == NULL) {
			mp->db->type = M_IOCNAK;
			return str_qreply(wq, mp);
		}

		/* todo needs under allif_lock */
		strncpy(intf->name, ifr->ifr_name, IF_NAMESIZE);
		mp->db->type = M_IOCACK;
		return str_qreply(wq, mp);
	}

	default:
		mp->db->type = M_IOCNAK;
		return str_qreply(wq, mp);
	}
}

static void
ip_uwput(queue_t *wq, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_IOCTL: {
		struct strioctl *ioc = (typeof(ioc))mp->rptr;

		switch (ioc->ic_cmd) {
		case I_PLINK: {
			struct linkblk *linkp = (typeof(linkp))ioc->ic_dp;

			kassert(linkp->qbot->qinfo == &ip_lwinit);

			ip_if_publish(linkp->qbot->ptr, linkp->index);;

			mp->db->type = M_IOCACK;
			ioc->rval = 0;
			return str_qreply(wq, mp);
		}

		case SIOCGIFINDEX:
		case SIOCGIFNAME:
		case SIOCSIFNAMEBYMUXID:
			return ip_uwput_ioctl_sgif(wq, mp);

		default:
			kdprintf("ip_uwput: unhandled ioctl 0x%x\n",
			    ioc->ic_cmd);
			mp->db->type = M_IOCNAK;
			return str_qreply(wq, mp);
			ktodo();
		}
	}

	default:
		ktodo();
	}
}

static void ip_input(void *ptr, mblk_t *mp)
{
	ip_if_t *ifp = ptr;
	struct ether_header *eh = (struct ether_header *)mp->rptr;

	switch(ntohs(eh->ether_type)) {
	case ETHERTYPE_IP:
	break;
	}

	kdprintf("ip_rput_data: from " FMT_MAC " to " FMT_MAC "\n",
	    eh->ether_shost[0], eh->ether_shost[1], eh->ether_shost[2],
	    eh->ether_shost[3], eh->ether_shost[4], eh->ether_shost[5],
	    eh->ether_dhost[0], eh->ether_dhost[1], eh->ether_dhost[2],
	    eh->ether_dhost[3], eh->ether_dhost[4], eh->ether_dhost[5]);
}

struct attach_result {
	mblk_t *ack_mp;
	kevent_t ack_ev;
};

static int
ip_lropen(queue_t *rq, void *)
{
	struct attach_result attach_res;
	dl_keyronex_bind_req_t *req;
	dl_keyronex_bind_ack_t *ack;
	ip_if_t *ifp;
	mblk_t *mp;

	attach_res.ack_mp = NULL;
	ke_event_init(&attach_res.ack_ev, false);

	rq->ptr =  &attach_res;

	mp = str_allocb(sizeof(*req));
	mp->db->type = M_PROTO;
	req = (typeof(req))mp->rptr;
	req->dl_primitive = DL_KEYRONEX_BIND_REQ;
	mp->wptr += sizeof(*req);

	str_putnext(rq->other, mp);
	str_qwait(rq, &attach_res.ack_ev);

	ack = (typeof(ack))attach_res.ack_mp->rptr;

	ifp = ip_if_new(ack->dl_mac);
	rq->ptr = rq->other->ptr = ifp;
	memcpy(ifp->mac, ack->dl_mac, ETH_ALEN);
	*ack->pdata = ifp;
	*ack->pput = ip_input;

	return 0;
}

static void
ip_lrput(queue_t *q, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_PROTO: {
		union DL_primitives *dlp = (typeof(dlp))mp->rptr;

		switch (dlp->dl_primitive) {
		case DL_KEYRONEX_BIND_ACK: {
			struct attach_result *attach_res =
			    (typeof(attach_res))q->ptr;
			attach_res->ack_mp = mp;
			ke_event_set_signalled(&attach_res->ack_ev, true);
			return;
		}

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
ip_init(void)
{
	void rtnetlink_init(void);
	void tcp_init(void);

	devfs_create_node(DEV_KIND_STREAM, &ip_devops, NULL, "ip");
	rtnetlink_init();
#if 0
	tcp_init();
#endif
}
