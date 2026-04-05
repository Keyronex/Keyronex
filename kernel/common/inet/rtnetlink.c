/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Mar 06 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file rtnetlink.c
 * @brief NetLink routing protocol.
 */

#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/netlinksubr.h>
#include <sys/stream.h>
#include <sys/strsubr.h>

#include <net/if.h>
#include <netinet/in.h>

#include <inet/ip.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdatomic.h>

extern TAILQ_HEAD(, ip_if) ip_allif;
extern kspinlock_t ip_allif_lock;

#if 0

struct rt_attrs {
	struct in_addr	*dst;		/* RTA_DST */
	struct in_addr	*gateway;	/* RTA_GATEWAY */
	unsigned	*oif;		/* RTA_OIF (muxid) */
	uint32_t	*metric;	/* RTA_PRIORITY */
};

static void
rtnl_parse_attrs(struct nlmsghdr *nlh, struct rt_attrs *out)
{
	struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
	int attr_len = (int)(nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*rtm)));
	struct rtattr *rta;

	memset(out, 0, sizeof(*out));

	if (attr_len <= 0)
		return;

	rta = (struct rtattr *)((char *)rtm + NLMSG_ALIGN(sizeof(*rtm)));

	while (RTA_OK(rta, attr_len)) {
		switch (rta->rta_type) {
		case RTA_DST:
			if (RTA_PAYLOAD(rta) >= sizeof(struct in_addr))
				out->dst = (struct in_addr *)RTA_DATA(rta);
			break;
		case RTA_GATEWAY:
			if (RTA_PAYLOAD(rta) >= sizeof(struct in_addr))
				out->gateway =
				    (struct in_addr *)RTA_DATA(rta);
			break;
		case RTA_OIF:
			if (RTA_PAYLOAD(rta) >= sizeof(unsigned))
				out->oif = (unsigned *)RTA_DATA(rta);
			break;
		case RTA_PRIORITY:
			if (RTA_PAYLOAD(rta) >= sizeof(uint32_t))
				out->metric = (uint32_t *)RTA_DATA(rta);
			break;
		default:
			/* silently skip unknown attrs */
			break;
		}
		rta = RTA_NEXT(rta, attr_len);
	}
}


static ip_if_t *
rtnl_resolve_intf(struct rt_attrs *attrs)
{
	if (attrs->oif != NULL)
		return ip_if_lookup_by_muxid(*attrs->oif);


	if (attrs->gateway != NULL) {
		struct ip_route_result via = ip_route_lookup(*attrs->gateway);
		/* look up the gateway; we'll get the connected route */
		if (via.intf != NULL)
			return via.intf; /* that's retained */
	}

	return NULL;
}

static int
rtnl_newroute(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
	struct rt_attrs attrs;
	struct in_addr dst = { .s_addr = INADDR_ANY };
	struct in_addr gw = { .s_addr = INADDR_ANY };
	ip_if_t *intf;
	uint32_t metric = 0;
	int r;

	if (rtm->rtm_family != AF_INET)
		return -EAFNOSUPPORT;

	rtnl_parse_attrs(nlh, &attrs);

	if (attrs.dst != NULL)
		dst = *attrs.dst;
	if (attrs.gateway != NULL)
		gw = *attrs.gateway;
	if (attrs.metric != NULL)
		metric = *attrs.metric;

	intf = rtnl_resolve_intf(&attrs);
	if (intf == NULL) {
		kdprintf("rtnl_newroute: cannot resolve output interface\n");
		return -ENETUNREACH;
	}

	r = ip_route_add(dst, rtm->rtm_dst_len, gw, intf,
	    rtm->rtm_protocol, rtm->rtm_scope, rtm->rtm_type, metric);

	ip_if_release(intf);

	nl_send_error(wq, nlh, 0);

	return r;
}
#endif


static void
rtnl_emit_newlink(queue_t *wq, ip_if_t *ifp, uint32_t seq, uint32_t pid)
{
	mblk_t *mp;
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifi;
	struct rtattr *rta;
	size_t namelen = strlen(ifp->name) + 1;
	size_t total;

	total = NLMSG_SPACE(sizeof(struct ifinfomsg)) + RTA_SPACE(namelen) +
	    RTA_SPACE(ETH_ALEN) + RTA_SPACE(sizeof(int));

	mp = str_allocb(total);
	if (mp == NULL)
		return;

	memset(mp->rptr, 0, total);
	mp->db->type = M_DATA;

	nlh = (struct nlmsghdr *)mp->rptr;
	nlh->nlmsg_len = (uint32_t)total;
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_MULTI;
	nlh->nlmsg_seq = seq;
	nlh->nlmsg_pid = pid;

	ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_type = ARPHRD_ETHER;
	ifi->ifi_index = ifp->muxid;
	ifi->ifi_flags = IFF_UP | IFF_RUNNING |
	    IFF_LOWER_UP; /* todo: real flags */
	ifi->ifi_change = ifi->ifi_flags;

	/* IFLA_IFNAME */
	rta = IFLA_RTA(ifi);
	rta->rta_type = IFLA_IFNAME;
	rta->rta_len = (unsigned short)RTA_LENGTH(namelen);
	memcpy(RTA_DATA(rta), ifp->name, namelen);

	/* IFLA_ADDRESS (LL addr) */
	rta = (struct rtattr *)((char *)rta + RTA_SPACE(namelen));
	rta->rta_type = IFLA_ADDRESS;
	rta->rta_len = (unsigned short)RTA_LENGTH(ETH_ALEN);
	memcpy(RTA_DATA(rta), ifp->mac, ETH_ALEN);

	/* IFLA_MTU */
	rta = (struct rtattr *)((char *)rta + RTA_SPACE(ETH_ALEN));
	rta->rta_type = IFLA_MTU;
	rta->rta_len = (unsigned short)RTA_LENGTH(sizeof(int));
	*(int *)RTA_DATA(rta) = 1500; /* todo: real MTU */


	mp->wptr = mp->rptr + total;
	str_qreply(wq, mp);
}

static int
rtnl_getlink(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	ip_if_t *ifp;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&ip_allif_lock);

	TAILQ_FOREACH(ifp, &ip_allif, tqentry) {
		rtnl_emit_newlink(wq, ifp, nlh->nlmsg_seq, nlh->nlmsg_pid);
	}

	ke_spinlock_exit(&ip_allif_lock, ipl);

	nl_send_done(wq, nlh->nlmsg_seq, nlh->nlmsg_pid);
	return 0;
}

static void
rtnl_emit_newaddr(queue_t *wq, ip_if_t *ifp, ip_ifaddr_t *ifa,
    uint32_t seq, uint32_t pid)
{
	sa_family_t family = ifa->addr.sa.sa_family;
	size_t alen;
	const void *addr_ptr;
	mblk_t *mp;
	struct nlmsghdr *nlh;
	struct ifaddrmsg *ifam;
	struct rtattr *rta;
	size_t total;

	if (family == AF_INET) {
		alen = sizeof(struct in_addr);
		addr_ptr = &ifa->addr.in.sin_addr;
	} else if (family == AF_INET6) {
		alen = sizeof(struct in6_addr);
		addr_ptr = &ifa->addr.in6.sin6_addr;
	} else {
		return;
	}

	total = NLMSG_SPACE(sizeof(struct ifaddrmsg)) + 2 * RTA_SPACE(alen);

	mp = str_allocb(total);
	memset(mp->rptr, 0, total);
	mp->db->type = M_DATA;

	nlh = (struct nlmsghdr *)mp->rptr;
	nlh->nlmsg_len = (uint32_t)total;
	nlh->nlmsg_type = RTM_NEWADDR;
	nlh->nlmsg_flags = NLM_F_MULTI;
	nlh->nlmsg_seq = seq;
	nlh->nlmsg_pid = pid;

	ifam = (struct ifaddrmsg *)NLMSG_DATA(nlh);
	ifam->ifa_family = family;
	ifam->ifa_prefixlen = ifa->prefixlen;
	ifam->ifa_flags = 0;
	ifam->ifa_scope = 0;
	ifam->ifa_index = (unsigned)ifp->muxid;

	/* IFA_ADDRESS */
	rta = IFA_RTA(ifam);
	rta->rta_type = IFA_ADDRESS;
	rta->rta_len = (unsigned short)RTA_LENGTH(alen);
	memcpy(RTA_DATA(rta), addr_ptr, alen);

	/* IFA_LOCAL */
	rta = (struct rtattr *)((char *)rta + RTA_SPACE(alen));
	rta->rta_type = IFA_LOCAL;
	rta->rta_len = (unsigned short)RTA_LENGTH(alen);
	memcpy(RTA_DATA(rta), addr_ptr, alen);

	mp->wptr = mp->rptr + total;
	str_qreply(wq, mp);
}

static int
rtnl_getaddr(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	struct ifaddrmsg *req_ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
	sa_family_t family = req_ifa->ifa_family;
	ip_if_t *ifp;
	ip_ifaddr_t *ifa;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&ip_allif_lock);

	TAILQ_FOREACH(ifp, &ip_allif, tqentry) {
		RCULIST_FOREACH(ifa, &ifp->addrs, rlentry) {
			if (family != AF_UNSPEC &&
			    ifa->addr.sa.sa_family != family)
				continue;
			rtnl_emit_newaddr(wq, ifp, ifa,
			    nlh->nlmsg_seq, nlh->nlmsg_pid);
		}
	}

	ke_spinlock_exit(&ip_allif_lock, ipl);

	nl_send_done(wq, nlh->nlmsg_seq, nlh->nlmsg_pid);
	return 0;
}

struct ifa_attrs {
	void *ifa_address;	/* IFA_ADDRESS */
	void *ifa_local;	/* IFA_LOCAL */
};

static void
rtnl_parse_ifa_attrs(struct nlmsghdr *nlh, struct ifa_attrs *out)
{
	struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
	struct rtattr *rta = IFA_RTA(ifa);
	int len = (int)IFA_PAYLOAD(nlh);

	memset(out, 0, sizeof(*out));

	while (RTA_OK(rta, len)) {
		switch (rta->rta_type) {
		case IFA_ADDRESS:
			out->ifa_address = RTA_DATA(rta);
			break;
		case IFA_LOCAL:
			out->ifa_local = RTA_DATA(rta);
			break;
		default:
			break;
		}
		rta = RTA_NEXT(rta, len);
	}
}

static int
rtnl_newaddr(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	struct ifaddrmsg *req_ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
	struct ifa_attrs attrs;
	ip_if_t *ifp;
	const void *addr_src;
	int r;

	if (req_ifa->ifa_family != AF_INET &&
	    req_ifa->ifa_family != AF_INET6) {
		nl_send_error(wq, nlh, EAFNOSUPPORT);
		return -EAFNOSUPPORT;
	}

	rtnl_parse_ifa_attrs(nlh, &attrs);

	/* prefer IFA_LOCAL, fall back to IFA_ADDRESS */
	addr_src = attrs.ifa_local != NULL ? attrs.ifa_local :
	    attrs.ifa_address;
	if (addr_src == NULL) {
		nl_send_error(wq, nlh, EINVAL);
		return -EINVAL;
	}

	ifp = ip_if_lookup_by_muxid((int)req_ifa->ifa_index);
	if (ifp == NULL) {
		nl_send_error(wq, nlh, ENODEV);
		return -ENODEV;
	}

	if (req_ifa->ifa_family == AF_INET) {
		r = ipv4_if_newaddr(ifp, addr_src, req_ifa->ifa_prefixlen);
	} else if (req_ifa->ifa_family == AF_INET6) {
		r = ipv6_if_newaddr(ifp, addr_src, req_ifa->ifa_prefixlen);
	} else {
		ktodo();
	}

	ip_if_release(ifp);

	nl_send_error(wq, nlh, r);
	return 0;
}

static int
rtnl_handler(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	switch (nlh->nlmsg_type) {
	case RTM_GETLINK:
		return rtnl_getlink(wq, mp, nlh);

#if 0
	case RTM_NEWROUTE:
		return rtnl_newroute(wq, mp, nlh);
#endif

	case RTM_NEWADDR:
		return rtnl_newaddr(wq, mp, nlh);

	case RTM_DELADDR:
		kdprintf("rtnetlink: RTM_DELADDR not yet supported\n");
		return -EOPNOTSUPP;

	case RTM_GETADDR:
		return rtnl_getaddr(wq, mp, nlh);

	default:
		kdprintf("rtnetlink: unknown type %d\n", nlh->nlmsg_type);
		return -EOPNOTSUPP;
	}
}

void
rtnetlink_init(void)
{
	nl_register_protocol(NETLINK_ROUTE, rtnl_handler);
}
