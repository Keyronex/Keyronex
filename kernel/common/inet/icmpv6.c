/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 23 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file icmpv6.c
 * @brief ICMP v6 handling.
 */

#include <sys/stream.h>
#include <netinet/icmp6.h>
#include <netinet/ip6.h>

#include <inet/ip.h>

void ndp_input(ip_if_t *, mblk_t *, ip_rxattr_t *);

static void
icmpv6_input_echo_request(ip_if_t *ifp, mblk_t *mp, ip_rxattr_t *attr)
{
	struct icmp6_hdr *icmp6 = (typeof(icmp6))mp->rptr;
	struct in6_addr src = attr->l3hdr.ip6->ip6_src;
	struct ip6_hdr *ip6 = (typeof(ip6))attr->l3hdr.ip6;

	if (IN6_IS_ADDR_MULTICAST(&attr->l3hdr.ip6->ip6_dst)) {
		kdprintf("icmpv6: ignore echo request to multicast address\n");
		str_freemsg(mp);
		return;
	}

	ip6->ip6_src = ip6->ip6_dst;
	ip6->ip6_dst = src;

	icmp6->icmp6_type = ICMP6_ECHO_REPLY;
	icmp6->icmp6_code = 0;

	icmp6->icmp6_cksum = 0;
	icmp6->icmp6_cksum = htons(ip_icmp6_checksum(&ip6->ip6_src,
	    &ip6->ip6_dst, icmp6, mp->wptr - mp->rptr));

	ipv6_output(mp);
}

void
icmpv6_input(ip_if_t *ifp, mblk_t *mp, ip_rxattr_t *attr)
{
	const struct icmp6_hdr *icmp6;
	size_t avail = mp->wptr - mp->rptr;

	if (avail < sizeof(*icmp6)) {
		kdprintf("icmpv6_input: packet too short for ICMPv6 header\n");
		str_freemsg(mp);
		return;
	}

	icmp6 = (typeof(icmp6))mp->rptr;

	/* todo: checksum */

	switch(icmp6->icmp6_type) {
	case ICMP6_ECHO_REQUEST:
		return icmpv6_input_echo_request(ifp, mp, attr);

	case ND_NEIGHBOR_SOLICIT:
	case ND_NEIGHBOR_ADVERT:
		return ndp_input(ifp, mp, attr);

	default:
		kdprintf("icmpv6_input: unsupported ICMPv6 type %u\n",
		    icmp6->icmp6_type);
		str_freemsg(mp);
		return;
	}

}
