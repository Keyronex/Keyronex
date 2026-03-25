/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Wed Mar 25 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ipv6_output.c
 * @brief IPv6 output logic.
 */

#include <sys/errno.h>
#include <sys/libkern.h>
#include <sys/stream.h>

#include <netinet/ip6.h>

#include <inet/ip.h>

static void
ipv6_map_multicast_l2(const struct in6_addr *dst, struct ether_addr *l2addr)
{
	l2addr->ether_addr_octet[0] = 0x33;
	l2addr->ether_addr_octet[1] = 0x33;
	l2addr->ether_addr_octet[2] = dst->s6_addr[12];
	l2addr->ether_addr_octet[3] = dst->s6_addr[13];
	l2addr->ether_addr_octet[4] = dst->s6_addr[14];
	l2addr->ether_addr_octet[5] = dst->s6_addr[15];
}

int
ipv6_output(mblk_t *mp)
{
	const struct ip6_hdr *ip6;
	union sockaddr_union dst = {0};
	union in_addr_union nexthop = {0};
	route_result_t route;
	struct ether_addr dst_l2addr;
	ip_if_t *ifp;
	size_t avail;
	int r;

	avail = mp->wptr - mp->rptr;
	if (avail < sizeof(*ip6)) {
		str_freemsg(mp);
		return -EINVAL;
	}

	ip6 = (typeof(ip6))mp->rptr;

	dst.in6.sin6_family = AF_INET6;
	dst.in6.sin6_addr = ip6->ip6_dst;

	r = route_lookup(&dst, &route, true);
	if (r != 0) {
		str_freemsg(mp);
		return r;
	}

	ifp = route.ifp;
	if (ifp == NULL) {
		str_freemsg(mp);
		return -ENETUNREACH;
	}

	if (IN6_IS_ADDR_MULTICAST(&ip6->ip6_dst)) {
		ipv6_map_multicast_l2(&ip6->ip6_dst, &dst_l2addr);
		r = ip_if_output(ifp, mp, ETHERTYPE_IPV6, &dst_l2addr);
	} else {
		nexthop.in6 = route.nexthop.in6.sin6_addr;
		r = neighbour_output(ifp, ifp->neighbours_ipv6, mp, &nexthop);
	}

	ip_if_release(ifp);
	return r;
}
