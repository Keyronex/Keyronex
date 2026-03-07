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

#include <net/if.h>
#include <netinet/ether.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>

#include <fs/devfs/devfs.h>
#include <inet/ip.h>
#include <inet/ip_intf.h>
#include <inet/util.h>

#define SIOCSIFADDR		0x8916
#define SIOCSIFNETMASK		0x891C
#define SIOCSIFNAMEBYMUXID	0x89A0

void arp_input(ip_intf_t *, mblk_t *);
void icmp_input(ip_intf_t *, mblk_t *);
void ip_input(ip_intf_t *, mblk_t *);
void tcp_input(ip_intf_t *, mblk_t *);

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

static LIST_HEAD(, ip_intf) ip_intf_list = LIST_HEAD_INITIALIZER(ip_intf_list);
static krwlock_t ip_intf_rwlock = KRWLOCK_INITIALISER;

#define TRACE_IP(...) kdprintf("IP: " __VA_ARGS__)

ip_intf_t *
ip_intf_retain(ip_intf_t *intf)
{
	atomic_fetch_add_explicit(&intf->refcnt, 1, memory_order_relaxed);
	return intf;
}

void
ip_intf_release(ip_intf_t *intf)
{
	if (atomic_fetch_sub_explicit(&intf->refcnt, 1, memory_order_relaxed) ==
	    1) {
		kfatal("ip_intf_release");
	}
}

ip_intf_t *
ip_intf_lookup_by_muxid(int muxid)
{
	ip_intf_t *intf;

	ke_rwlock_enter_read(&ip_intf_rwlock, "ip_intf_lookup_by_muxid");
	LIST_FOREACH(intf, &ip_intf_list, if_list_link) {
		if (intf->muxid == muxid) {
			intf = ip_intf_retain(intf);
			break;
		}
	}
	ke_rwlock_exit_read(&ip_intf_rwlock);

	return intf;
}

ip_intf_t *
ip_intf_lookup_by_name(const char *name)
{
	ip_intf_t *intf;

	ke_rwlock_enter_read(&ip_intf_rwlock, "ip_intf_lookup_by_name");
	LIST_FOREACH(intf, &ip_intf_list, if_list_link) {
		if (strcmp(intf->name, name) == 0) {
			intf = ip_intf_retain(intf);
			break;
		}
	}
	ke_rwlock_exit_read(&ip_intf_rwlock);

	return intf;
}

void
ip_input(ip_intf_t *ifp, mblk_t *mp)
{
	struct ip *ip;
	size_t msgsize = mp->wptr - mp->rptr;
	uint16_t hlen;

	if (msgsize < sizeof(struct ether_header) + sizeof(struct ip)) {
		TRACE_IP("Packet too small\n");
		str_freemsg(mp);
		return;
	}

	ip = (struct ip *)(mp->rptr + sizeof(struct ether_header));

	hlen = ip->ip_hl << 2;
	if (hlen < sizeof(struct ip)) {
		TRACE_IP("Invalid IP header length\n");
		str_freemsg(mp);
		return;
	}

	if (msgsize < sizeof(struct ether_header) + hlen) {
		TRACE_IP("Packet too small for IP header\n");
		str_freemsg(mp);
		return;
	}

	if (ip->ip_v != IPVERSION) {
		TRACE_IP("Invalid version %d\n", ip->ip_v);
		str_freemsg(mp);
		return;
	}

	if ((ip->ip_sum = ip_checksum(ip, hlen)) != 0) {
		TRACE_IP("Checksum error\n");
		str_freemsg(mp);
		return;
	}

	switch (ip->ip_p) {
	case IPPROTO_TCP:
		tcp_input(ifp, mp);
		break;

	case IPPROTO_ICMP:
		icmp_input(ifp, mp);
		break;

	default:
		TRACE_IP("Unknown IP protocol %d\n", ip->ip_p);
		str_freemsg(mp);
		break;
	}
}

static void
ip_uwput_ioctl_sgif(queue_t *wq, mblk_t *mp)
{
	struct strioctl *ioc = (typeof(ioc))mp->rptr;
	struct ifreq *ifr = (typeof(ifr))ioc->ic_dp;
	ip_intf_t *intf = ip_intf_lookup_by_name(ifr->ifr_name);

	if (intf == NULL) {
		kdprintf("ip_uwput_ioctl_sgif: no such interface %s\n",
		    ifr->ifr_name);
		mp->db->type = M_IOCNAK;
		ioc->rval = -ENOENT;
		return str_qreply(wq, mp);
	}

	ke_rwlock_enter_write(&ip_intf_rwlock, "ip_uwput_ioctl_sgif");

	switch (ioc->ic_cmd) {
	case SIOCGIFFLAGS:
		ifr->ifr_flags = IFF_UP | IFF_RUNNING;
		break;

	case SIOCSIFFLAGS:
		/* ignore for now */
		break;

	case SIOCGIFADDR:
		ifr->ifr_addr.sa_family = AF_INET;
		((struct sockaddr_in *)&ifr->ifr_addr)->sin_addr = intf->addr;
		break;

	case SIOCSIFADDR:
		ip_route_if_down(intf);
		intf->addr = ((struct sockaddr_in *)&ifr->ifr_addr)->sin_addr;
		ip_route_if_up(intf);
		break;

	case SIOCGIFNETMASK:
		ifr->ifr_netmask.sa_family = AF_INET;
		((struct sockaddr_in *)&ifr->ifr_netmask)->sin_addr =
		    intf->netmask;
		break;

	case SIOCSIFNETMASK:
		ip_route_if_down(intf);
		intf->netmask =
		    ((struct sockaddr_in *)&ifr->ifr_netmask)->sin_addr;
		ip_route_if_up(intf);
		break;

	default:
		kunreachable(); /* only called for above */
	}

	ke_rwlock_exit_write(&ip_intf_rwlock);

	ip_intf_release(intf);

	ioc->ic_len = sizeof(struct ifreq);
	mp->db->type = M_IOCACK;
	str_qreply(wq, mp);
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

		case SIOCSIFNAMEBYMUXID: {
			struct ifreq *ifr = (typeof(ifr))ioc->ic_dp;
			ip_intf_t *intf;

			intf = ip_intf_lookup_by_muxid(ifr->ifr_ifindex);
			if (intf == NULL) {
				kdprintf("/dev/ip: no such muxid %d\n",
				    ifr->ifr_ifindex);
				mp->db->type = M_IOCNAK;
				return str_qreply(wq, mp);
			}

			strncpy(intf->name, ifr->ifr_name, IFNAMSIZ);
			ip_intf_release(intf);

			kdprintf("/dev/ip: muxid %d now named <%s>\n",
			    ifr->ifr_ifindex, ifr->ifr_name);

			mp->db->type = M_IOCACK;
			return str_qreply(wq, mp);
		}

		case I_PLINK: {
			ip_intf_t *intf;
			struct linkblk *linkp = (typeof(linkp))ioc->ic_dp;

			kassert(linkp->qbot->qinfo == &ip_lwinit);

			ke_rwlock_enter_write(&ip_intf_rwlock,
			    "ip_uwput I_PLINK");
			intf = linkp->qbot->ptr;
			intf->muxid = linkp->index;
			ke_rwlock_exit_write(&ip_intf_rwlock);

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
 * lower multiplexor side
 */

/* Send and wait synchronously for the DLPI bind request to be returned. */
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
ip_lropen(queue_t *rq, void *)
{
	ip_intf_t *intf;

	intf = kmem_alloc(sizeof(*intf));

	intf->refcnt = 1;
	intf->name[0] = '\0';
	intf->muxid = -1;
	intf->wq = rq->other->next;

	intf->addr.s_addr = INADDR_ANY;
	intf->netmask.s_addr = INADDR_ANY;

	arp_state_init(intf);

	rq->ptr = rq->other->ptr = intf;

	send_dlpi_bind(rq);

	kdprintf("ip_open: if bound, MAC " FMT_MAC ", MTU %d\n",
	    ARG_MAC(intf->mac), intf->mtu);

	ke_rwlock_enter_write(&ip_intf_rwlock, "ip_lopen");
	LIST_INSERT_HEAD(&ip_intf_list, intf, if_list_link);
	ke_rwlock_exit_write(&ip_intf_rwlock);

	return 0;
}

static void
ip_lrput(queue_t *q, mblk_t *mp)
{
	ip_intf_t *intf = q->ptr;

	switch (mp->db->type) {
	case M_DATA: {
		struct ether_header *eh = (struct ether_header *)mp->rptr;
		if (ntohs(eh->ether_type) == ETHERTYPE_IP) {
			ip_input(intf, mp);
		} else if (ntohs(eh->ether_type) == ETHERTYPE_ARP) {
			arp_input(intf, mp);
		} else {
			kdprintf("IP: unknown ethertype 0x%04x\n",
			    ntohs(eh->ether_type));
			str_freemsg(mp);
		}
		break;
	}

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

int
ip_output(mblk_t *mp)
{
	return ip_output_intfheld(mp, NULL);
}

int
ip_output_intfheld(mblk_t *m, ip_intf_t *ifp)
{
	struct ether_header *eh = (struct ether_header *)m->rptr;
	struct ip *ip = (struct ip *)(eh + 1);
	struct ip_route_result rt;

	rt =  ip_route_lookup(ip->ip_dst);
	if (rt.intf == NULL) {
		TRACE_IP("No route to " FMT_IP4 "\n",
		    ARG_IP4(ip->ip_dst.s_addr));
		str_freemsg(m);
		return -EHOSTUNREACH;
	}

	eh->ether_type = htons(ETHERTYPE_IP);
	ip->ip_sum = 0;
	ip->ip_sum = ip_checksum(ip, ip->ip_hl << 2);

	if (ifp != NULL && rt.intf != ifp)
		kfatal("handle ip output from unheld interface");

	arp_output(rt.intf, rt.next_hop.s_addr, m, true);

	ip_intf_release(rt.intf);

	return 0;
}

void
ip_init(void)
{
	void rtnetlink_init(void);

	devfs_create_node(DEV_KIND_STREAM, &ip_devops, NULL, "ip");
	rtnetlink_init();
}
