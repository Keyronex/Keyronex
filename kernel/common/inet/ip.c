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
#include <sys/k_wait.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/stream.h>
#include <sys/stropts.h>

#include <net/if.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>

#include <fs/devfs/devfs.h>
#include <inet/util.h>

#define SIOCSIFADDR		0x8916
#define SIOCSIFNETMASK		0x891C
#define SIOCSIFNAMEBYMUXID	0x89A0

typedef struct ip_intf {
	struct ether_addr mac;
	uint16_t mtu;

	mblk_t *sync_ack_mp;
	kevent_t *sync_ack_ev;
} ip_intf_t;

static void ip_uwput(queue_t *, mblk_t *);

static int ip_lopen(queue_t *, void *);
static void ip_lrput(queue_t *, mblk_t *);

static struct qinit ip_urinit = {};

static struct qinit ip_uwinit = {
	.putp = ip_uwput,
};

static struct qinit ip_lrinit = {
	.qopen = ip_lopen,
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

	kfatal("ip_uwput_ioctl_sgif (if name %s)", ifr->ifr_name);
}

static void
ip_uwput(queue_t *wq, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_IOCTL: {
		struct strioctl *ioc = (typeof(ioc))mp->rptr;

		switch (ioc->ic_cmd) {
		case SIOCGIFFLAGS:
		case SIOCSIFFLAGS:
		case SIOCGIFADDR:
		case SIOCSIFADDR:
		case SIOCGIFNETMASK:
		case SIOCSIFNETMASK:
			return ip_uwput_ioctl_sgif(wq, mp);

		case I_PLINK: {
			mp->db->type = M_IOCACK;
			return str_qreply(wq, mp);
		}

		default:
			kfatal("ip_uwput: unexpected ioctl type 0x%x",
			    ioc->ic_cmd);
		}

		break;
	}

	default:
		kfatal("ip_uwput: unexpected message type %d", mp->db->type);
	}
}

/*
 * Send and wait synchronously for the DLPI bind request to be returned.
 *
 */
static void
send_dlpi_bind(queue_t *rq)
{
	ip_intf_t *intf = rq->ptr;
	kevent_t ack_ev;
	struct dl_bind_req *req;
	struct dl_bind_ack *ack;
	mblk_t *mp;

	ke_event_init(&ack_ev, false);

	mp = str_allocb(sizeof(*req));

	mp->db->type = M_PROTO;
	req = (typeof(req))mp->rptr;
	req->dl_primitive = DL_BIND_REQ;
	req->dl_sap = -1; /* all SAPs */
	req->dl_max_conind = 0;
	req->dl_service_mode = DL_CLDLS;
	req->dl_conn_mgmt = 0;
	req->dl_xidtest_flg = 0;
	mp->wptr += sizeof(*req);

	intf->sync_ack_mp = NULL;
	intf->sync_ack_ev = &ack_ev;

	str_putnext(rq->other, mp);
	str_qwait(rq, intf->sync_ack_ev);

	mp = intf->sync_ack_mp;
	ack = (typeof(ack))mp->rptr;

	memcpy(&intf->mac, (char *)ack + ack->dl_addr_offset,
	    sizeof(struct ether_addr));

	intf->mtu = 1500;
}

static int
ip_lopen(queue_t *q, void *)
{
	ip_intf_t *intf = kmem_alloc(sizeof(*intf));
	q->ptr = intf;

	send_dlpi_bind(q);

	kdprintf("ip_open: if bound, MAC " FMT_MAC ", MTU %d\n",
	    ARG_MAC(intf->mac), intf->mtu);

	return 0;
}

static void
ip_lrput(queue_t *q, mblk_t *mp)
{
	ip_intf_t *intf = q->ptr;

	switch (mp->db->type) {
	case M_PROTO: {
		union DL_primitives *dlp = (typeof(dlp))mp->rptr;

		switch (dlp->dl_primitive) {
		case DL_BIND_ACK:
			kassert(intf->sync_ack_mp == NULL);
			intf->sync_ack_mp = mp;
			ke_event_set_signalled(intf->sync_ack_ev, true);
			break;
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
	devfs_create_node(DEV_KIND_STREAM, &ip_devops, NULL, "ip");
}
