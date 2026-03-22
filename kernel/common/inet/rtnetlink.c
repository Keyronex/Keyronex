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

static int
rtnl_handler(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	switch (nlh->nlmsg_type) {
#if 0
	case RTM_NEWROUTE:
		return rtnl_newroute(wq, mp, nlh);
#endif

	default:
		kdprintf("rtnl_handler: unknown type %d\n", nlh->nlmsg_type);
		return -EOPNOTSUPP;
	}
}

void
rtnetlink_init(void)
{
	nl_register_protocol(NETLINK_ROUTE, rtnl_handler);
}
