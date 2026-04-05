/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Apr 05 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ipv4_output.c
 * @brief IPv4 output logic.
 */

#include <sys/errno.h>
#include <sys/libkern.h>
#include <sys/stream.h>

#include <netinet/ip.h>

#include <inet/ip.h>
#include <inet/util.h>

int
ipv4_output(mblk_t *mp)
{
	struct ip *iph;
	union sockaddr_union dst = {0};
	union in_addr_union nexthop = {0};
	route_result_t route;
	ip_if_t *ifp;
	size_t avail;
	int r;

	avail = mp->wptr - mp->rptr;
	if (avail < sizeof(*iph)) {
		str_freemsg(mp);
		return -EINVAL;
	}

	iph = (struct ip *)mp->rptr;

	iph->ip_sum = 0;
	iph->ip_sum = ip_checksum(iph, (size_t)iph->ip_hl * 4);

	dst.in.sin_family = AF_INET;
	dst.in.sin_addr = iph->ip_dst;

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

	nexthop.in = route.nexthop.in.sin_addr;
	r = neighbour_output(ifp, ifp->neighbours_ipv4, mp, &nexthop);

	ip_if_release(ifp);
	return r;
}
