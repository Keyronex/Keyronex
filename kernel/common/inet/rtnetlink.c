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

struct rt_attrs {
	const void	*dst;		/* RTA_DST */
	const void	*gateway;	/* RTA_GATEWAY */
	const unsigned	*oif;		/* RTA_OIF (muxid) */
	const uint32_t	*priority;	/* RTA_PRIORITY */
};

static void
rtnl_parse_rt_attrs(struct nlmsghdr *nlh, struct rt_attrs *out)
{
	struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
	int attr_len = (int)RTM_PAYLOAD(nlh);
	struct rtattr *rta = RTM_RTA(rtm);
	size_t alen;

	memset(out, 0, sizeof(*out));

	if (rtm->rtm_family == AF_INET)
		alen = sizeof(struct in_addr);
	else if (rtm->rtm_family == AF_INET6)
		alen = sizeof(struct in6_addr);
	else
		return;

	while (RTA_OK(rta, attr_len)) {
		switch (rta->rta_type) {
		case RTA_DST:
			if (RTA_PAYLOAD(rta) >= alen)
				out->dst = RTA_DATA(rta);
			break;
		case RTA_GATEWAY:
			if (RTA_PAYLOAD(rta) >= alen)
				out->gateway = RTA_DATA(rta);
			break;
		case RTA_OIF:
			if (RTA_PAYLOAD(rta) >= sizeof(unsigned))
				out->oif = (const unsigned *)RTA_DATA(rta);
			break;
		case RTA_PRIORITY:
			if (RTA_PAYLOAD(rta) >= sizeof(uint32_t))
				out->priority = (const uint32_t *)RTA_DATA(rta);
			break;
		default:
			break;
		}
		rta = RTA_NEXT(rta, attr_len);
	}
}

/*
 * Fill a route_info_t from an RTM_NEWROUTE or RTM_DELROUTE message.
 * If an interface is retained (info->ifp != NULL), the caller must release it.
 */
static int
rtnl_fill_route_info(struct nlmsghdr *nlh, route_info_t *info)
{
	struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
	struct rt_attrs attrs;
	union sockaddr_union prefix;
	sa_family_t family = rtm->rtm_family;

	if (family != AF_INET && family != AF_INET6)
		return -EAFNOSUPPORT;

	rtnl_parse_rt_attrs(nlh, &attrs);

	memset(&prefix, 0, sizeof(prefix));
	prefix.sa.sa_family = family;
	if (attrs.dst != NULL) {
		if (family == AF_INET)
			memcpy(&prefix.in.sin_addr, attrs.dst,
			    sizeof(struct in_addr));
		else
			memcpy(&prefix.in6.sin6_addr, attrs.dst,
			    sizeof(struct in6_addr));
	}

	route_info_init(info, &prefix, rtm->rtm_dst_len);
	info->protocol = rtm->rtm_protocol;
	info->scope = rtm->rtm_scope;
	info->type = rtm->rtm_type;
	info->tos = rtm->rtm_tos;

	if (attrs.gateway != NULL) {
		union sockaddr_union gw;
		memset(&gw, 0, sizeof(gw));
		gw.sa.sa_family = family;
		if (family == AF_INET)
			memcpy(&gw.in.sin_addr, attrs.gateway,
			    sizeof(struct in_addr));
		else
			memcpy(&gw.in6.sin6_addr, attrs.gateway,
			    sizeof(struct in6_addr));
		route_info_set_gateway(info, &gw);
	}

	if (attrs.priority != NULL)
		route_info_set_priority(info, *attrs.priority);

	if (attrs.oif != NULL) {
		info->ifp = ip_if_lookup_by_muxid((int)*attrs.oif);
		if (info->ifp == NULL)
			return -ENODEV;
	} else if (attrs.gateway != NULL) {
		union sockaddr_union gw_addr;
		route_result_t via;

		memset(&gw_addr, 0, sizeof(gw_addr));
		gw_addr.sa.sa_family = family;
		if (family == AF_INET)
			memcpy(&gw_addr.in.sin_addr, attrs.gateway,
			    sizeof(struct in_addr));
		else
			memcpy(&gw_addr.in6.sin6_addr, attrs.gateway,
			    sizeof(struct in6_addr));
		if (route_lookup(&gw_addr, &via, true) == 0)
			info->ifp = via.ifp;
	}

	return 0;
}

static int
rtnl_newroute(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	route_info_t info;
	int r;

	r = rtnl_fill_route_info(nlh, &info);
	if (r != 0) {
		nl_send_error(wq, nlh, -r);
		return 0;
	}

	r = route_add(&info);

	if (info.ifp != NULL)
		ip_if_release(info.ifp);

	nl_send_error(wq, nlh, -r);

	return 0;
}

static int
rtnl_delroute(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	route_info_t info;
	int r;

	r = rtnl_fill_route_info(nlh, &info);
	if (r != 0) {
		nl_send_error(wq, nlh, -r);
		return 0;
	}

	if (info.ifp != NULL) {
		ip_if_release(info.ifp);
		info.ifp = NULL;
	}

	r = route_del(&info);
	nl_send_error(wq, nlh, -r);
	return 0;
}

struct rtnl_route_emit_ctx {
	queue_t		*wq;
	uint32_t	seq;
	uint32_t	pid;
};

static void
rtnl_emit_newroute(queue_t *wq, const route_info_t *info,
    uint32_t seq, uint32_t pid)
{
	sa_family_t family = info->prefix.sa.sa_family;
	size_t alen;
	const void *dst_ptr, *gw_ptr;
	bool has_gw;
	size_t total;
	mblk_t *mp;
	struct nlmsghdr *nlh;
	struct rtmsg *rtm;
	struct rtattr *rta;
	char *p;

	if (family == AF_INET) {
		alen = sizeof(struct in_addr);
		dst_ptr = &info->prefix.in.sin_addr;
	} else if (family == AF_INET6) {
		alen = sizeof(struct in6_addr);
		dst_ptr = &info->prefix.in6.sin6_addr;
	} else {
		return;
	}

	has_gw = (info->gateway.sa.sa_family == family);
	gw_ptr = NULL;
	if (has_gw)
		gw_ptr = (family == AF_INET) ?
		    (const void *)&info->gateway.in.sin_addr :
		    (const void *)&info->gateway.in6.sin6_addr;

	total = NLMSG_SPACE(sizeof(struct rtmsg)) +
	    RTA_SPACE(alen) +
	    (has_gw ? RTA_SPACE(alen) : 0) +
	    (info->ifp != NULL ? RTA_SPACE(sizeof(int)) : 0) +
	    RTA_SPACE(sizeof(uint32_t));

	mp = str_allocb(total);
	if (mp == NULL)
		return;

	memset(mp->rptr, 0, total);
	mp->db->type = M_DATA;

	nlh = (struct nlmsghdr *)mp->rptr;
	nlh->nlmsg_len = (uint32_t)total;
	nlh->nlmsg_type = RTM_NEWROUTE;
	nlh->nlmsg_flags = NLM_F_MULTI;
	nlh->nlmsg_seq = seq;
	nlh->nlmsg_pid = pid;

	rtm = (struct rtmsg *)NLMSG_DATA(nlh);
	rtm->rtm_family = (unsigned char)family;
	rtm->rtm_dst_len = info->prefixlen;
	rtm->rtm_src_len = 0;
	rtm->rtm_tos = info->tos;
	rtm->rtm_table = (unsigned char)info->table;
	rtm->rtm_protocol = (unsigned char)info->protocol;
	rtm->rtm_scope = (unsigned char)info->scope;
	rtm->rtm_type = (unsigned char)info->type;
	rtm->rtm_flags = 0;

	p = (char *)RTM_RTA(rtm);

	rta = (struct rtattr *)p;
	rta->rta_type = RTA_DST;
	rta->rta_len = (uint16_t)RTA_LENGTH(alen);
	memcpy(RTA_DATA(rta), dst_ptr, alen);
	p += RTA_SPACE(alen);

	if (has_gw) {
		rta = (struct rtattr *)p;
		rta->rta_type = RTA_GATEWAY;
		rta->rta_len = (uint16_t)RTA_LENGTH(alen);
		memcpy(RTA_DATA(rta), gw_ptr, alen);
		p += RTA_SPACE(alen);
	}

	if (info->ifp != NULL) {
		rta = (struct rtattr *)p;
		rta->rta_type = RTA_OIF;
		rta->rta_len = (uint16_t)RTA_LENGTH(sizeof(int));
		*(int *)RTA_DATA(rta) = info->ifp->muxid;
		p += RTA_SPACE(sizeof(int));
	}

	rta = (struct rtattr *)p;
	rta->rta_type = RTA_PRIORITY;
	rta->rta_len = (uint16_t)RTA_LENGTH(sizeof(uint32_t));
	*(uint32_t *)RTA_DATA(rta) = info->priority;

	mp->wptr = mp->rptr + total;
	str_qreply(wq, mp);
}

static void
rtnl_getroute_emit_cb(const route_info_t *info, void *arg)
{
	struct rtnl_route_emit_ctx *ctx = arg;
	rtnl_emit_newroute(ctx->wq, info, ctx->seq, ctx->pid);
}

static int
rtnl_getroute(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	struct rtnl_route_emit_ctx ctx = {
		.wq = wq,
		.seq = nlh->nlmsg_seq,
		.pid = nlh->nlmsg_pid,
	};
	sa_family_t family = AF_UNSPEC;

	if (nlh->nlmsg_len >= NLMSG_LENGTH(sizeof(struct rtmsg))) {
		struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
		family = rtm->rtm_family;
	}

	if (family == AF_UNSPEC || family == AF_INET)
		route_walk(AF_INET, rtnl_getroute_emit_cb, &ctx);
	if (family == AF_UNSPEC || family == AF_INET6)
		route_walk(AF_INET6, rtnl_getroute_emit_cb, &ctx);

	nl_send_done(wq, nlh->nlmsg_seq, nlh->nlmsg_pid);
	return 0;
}


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

	nl_send_error(wq, nlh, -r);
	return 0;
}

static int
rtnl_handler(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	switch (nlh->nlmsg_type) {
	case RTM_GETLINK:
		return rtnl_getlink(wq, mp, nlh);

	case RTM_NEWADDR:
		return rtnl_newaddr(wq, mp, nlh);

	case RTM_DELADDR:
		kdprintf("rtnetlink: RTM_DELADDR not yet supported\n");
		return -EOPNOTSUPP;

	case RTM_GETADDR:
		return rtnl_getaddr(wq, mp, nlh);

	case RTM_NEWROUTE:
		return rtnl_newroute(wq, mp, nlh);

	case RTM_DELROUTE:
		return rtnl_delroute(wq, mp, nlh);

	case RTM_GETROUTE:
		return rtnl_getroute(wq, mp, nlh);

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
