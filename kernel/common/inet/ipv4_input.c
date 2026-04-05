/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 30 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ipv4_input.c
 * @brief IPv4 input handling.
 */

#include <sys/libkern.h>
#include <sys/stream.h>

#include <netinet/in.h>
#include <netinet/ip.h>

#include <inet/ip.h>
#include <inet/util.h>

void udp_ipv4_input(ip_if_t *, mblk_t *, ip_rxattr_t *);

static bool
ipv4_input_is_for_us(ip_if_t *ifp, struct in_addr dst)
{
	ip_ifaddr_t *ifa;

	if (dst.s_addr == INADDR_BROADCAST)
		return true;

	RCULIST_FOREACH(ifa, &ifp->addrs, rlentry) {
		struct in_addr addr;

		if (ifa->addr.sa.sa_family != AF_INET)
			continue;

		addr = ifa->addr.in.sin_addr;

		if (addr.s_addr == dst.s_addr)
			return true;

		if (ifa->prefixlen > 0 && ifa->prefixlen < 32) {
			uint32_t mask = htonl(~0u << (32 - ifa->prefixlen));
			struct in_addr bcast;
			bcast.s_addr = (addr.s_addr & mask) | ~mask;
			if (dst.s_addr == bcast.s_addr)
				return true;
		}
	}

	return false;
}

static ip_ifaddr_t *
ipv4_find_ifa(ip_if_t *ifp, struct in_addr dst)
{
	ip_ifaddr_t *ifa;

	RCULIST_FOREACH(ifa, &ifp->addrs, rlentry) {
		if (ifa->addr.sa.sa_family != AF_INET)
			continue;
		if (ifa->addr.in.sin_addr.s_addr == dst.s_addr)
			return ifa;
	}

	return NULL;
}

void
ipv4_input(ip_if_t *ifp, mblk_t *mp)
{
	const struct ip *iph;
	ip_rxattr_t attr;
	size_t avail, hlen, pktlen;
	uint16_t ip_off;

	avail = mp->wptr - mp->rptr;

	if (avail < sizeof(*iph)) {
		kdprintf("ipv4_input: packet too short for IP header\n");
		str_freemsg(mp);
		return;
	}

	iph = (const struct ip *)mp->rptr;

	if (iph->ip_v != 4) {
		kdprintf("ipv4_input: bad IP version %u\n", iph->ip_v);
		str_freemsg(mp);
		return;
	}

	hlen = (size_t)iph->ip_hl * 4;

	if (hlen < sizeof(*iph) || avail < hlen) {
		kdprintf("ipv4_input: bad IP header length\n");
		str_freemsg(mp);
		return;
	}

	pktlen = ntohs(iph->ip_len);

	if (pktlen < hlen || avail < pktlen) {
		kdprintf("ipv4_input: packet truncated\n");
		str_freemsg(mp);
		return;
	}

	if (ip_checksum((void *)iph, hlen) != 0) {
		kdprintf("ipv4_input: bad IP checksum\n");
		str_freemsg(mp);
		return;
	}

	/* no fragmentation for now  */
	ip_off = ntohs(iph->ip_off);
	if (ip_off & (IP_MF | IP_OFFMASK)) {
		str_freemsg(mp);
		return;
	}

	if (!ipv4_input_is_for_us(ifp, iph->ip_dst)) {
		str_freemsg(mp);
		return;
	}

	/* trim any padding */
	mp->wptr = mp->rptr + pktlen;

	attr.l3hdr.ip4 = iph;
	attr.ifa.ifa = ipv4_find_ifa(ifp, iph->ip_dst);

	mp->rptr += hlen;

	switch (iph->ip_p) {
	case IPPROTO_TCP:
		tcp_ipv4_input(ifp, mp, &attr);
		break;

	case IPPROTO_UDP:
		udp_ipv4_input(ifp, mp, &attr);
		break;

	default:
		kdprintf("ipv4_input: unsupported protocol %u, dropping\n",
		    iph->ip_p);
		str_freemsg(mp);
		break;
	}
}
