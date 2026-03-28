/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 23 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ipv6_input.c
 * @brief IPv6 input handling.
 */

#include <sys/libkern.h>
#include <sys/stream.h>

#include <netinet/ip6.h>

#include <inet/ip.h>

#define ip6_vfc ip6_ctlun.ip6_un2_vfc

static bool
ipv6_addr_is_all_nodes_mc(const struct in6_addr *addr)
{
	static const uint8_t all_nodes[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 1 };

	return memcmp(addr, all_nodes, sizeof(all_nodes)) == 0;
}

static bool
ipv6_addr_equal(const struct in6_addr *a, const struct in6_addr *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

struct in6_addr
ipv6_solicited_node_mc(const struct in6_addr *addr)
{
	struct in6_addr mc;
	mc.s6_addr[0] = 0xff;
	mc.s6_addr[1] = 0x02;
	memset(&mc.s6_addr[2], 0, 9);
	mc.s6_addr[11] = 0x01;
	mc.s6_addr[12] = 0xff;
	memcpy(&mc.s6_addr[13], &addr->s6_addr[13], 3);
	return mc;
}

static bool
ipv6_input_is_for_us(ip_if_t *ifp, const struct in6_addr *dst)
{
	ip_ifaddr_t *ifa;

	if (IN6_IS_ADDR_LOOPBACK(dst)) /* TODO: only if ifp is loopback */
		return true;

	if (ipv6_addr_is_all_nodes_mc(dst)) /* ff02::1 */
		return true;

	/* all input processing is an RCU grace period */
	TAILQ_FOREACH(ifa, &ifp->addrs, tqentry) { /* TODO RCU-friendly queue */
		struct in6_addr solicited;

		if (ifa->addr.sa.sa_family != AF_INET6)
			continue;

		if (ifa->ipv6_state == IFADDR_DUPLICATED)
			continue;

		if (ifa->ipv6_state != IFADDR_TENTATIVE &&
		    ipv6_addr_equal(&ifa->addr.in6.sin6_addr, dst))
			return true;

		solicited = ipv6_solicited_node_mc(&ifa->addr.in6.sin6_addr);
		if (ipv6_addr_equal(&solicited, dst))
			return true;
	}

	/* TODO: joined multicast groups */

	return false;
}

void icmpv6_input(ip_if_t *, mblk_t *, ip_rxattr_t *);

void
ipv6_input(ip_if_t *ifp, mblk_t *mp)
{
	const struct ip6_hdr *ip6;
	ip_rxattr_t attr;

	size_t avail = mp->wptr - mp->rptr;

	if (avail < sizeof(*ip6)) {
		kdprintf("ipv6_input: packet too short for IPv6 header\n");
		str_freemsg(mp);
		return;
	}

	ip6 = (typeof(ip6))mp->rptr;

	if ((ip6->ip6_vfc >> 4) != 6) {
		kdprintf("ipv6_input: bad IPv6 version\n");
		str_freemsg(mp);
		return;
	}

	if (!ipv6_input_is_for_us(ifp, &ip6->ip6_dst)) {
		char dst_str[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &ip6->ip6_dst, dst_str, sizeof(dst_str));
		kdprintf("ipv6_input: packet not for us, dropping (dst=%s)\n",
		    dst_str);
		str_freemsg(mp);
		return;
	}

	mp->rptr += sizeof(*ip6);
	attr.l3hdr.ip6 = ip6;

	switch(ip6->ip6_nxt) {
	case IPPROTO_ICMPV6:
		icmpv6_input(ifp, mp, &attr);
		break;

	default:
		kdprintf("ipv6_input: unsupported next header %u\n", ip6->ip6_nxt);
		str_freemsg(mp);
		break;
	}
}
