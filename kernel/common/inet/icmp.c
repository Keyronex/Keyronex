/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Jan 12 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file icmp.c
 * @brief Internet Control Message Protocol implementation.
 */


#include <netinet/ip_icmp.h>

#include <inet/ip.h>
#include <inet/ip_intf.h>
#include <inet/util.h>

#define TRACE_ICMP(...) kdprintf("ICMP: " __VA_ARGS__)

void
icmp_input(ip_intf_t *ifp, mblk_t *mp)
{
	struct ether_header *eh;
	struct ip *ip;
	struct icmp *icmp;
	size_t ip_hlen, len;

	eh = (struct ether_header *)mp->rptr;
	ip = (struct ip *)(eh + 1);
	ip_hlen = ip->ip_hl << 2;

	if ((mp->wptr - mp->rptr) <
	    (sizeof(struct ether_header) + ip_hlen + sizeof(struct icmp))) {
		TRACE_ICMP("ICMP packet too short\n");
		str_freemsg(mp);
		return;
	}

	icmp = (struct icmp *)((uint8_t *)ip + ip_hlen);
	len = ntohs(ip->ip_len) - ip_hlen;

	if (ip_checksum(icmp, len) != 0) {
		TRACE_ICMP("ICMP checksum failed\n");
		str_freemsg(mp);
		return;
	}

	switch (icmp->icmp_type) {
	case ICMP_ECHO: {
		struct in_addr tmp = ip->ip_src;

		icmp->icmp_type = ICMP_ECHOREPLY;
		icmp->icmp_cksum = 0;
		icmp->icmp_cksum = ip_checksum(icmp, len);

		ip->ip_src = ip->ip_dst;
		ip->ip_dst = tmp;

		if (ip->ip_src.s_addr == INADDR_ANY ||
		    ip->ip_src.s_addr == INADDR_BROADCAST)
			ip->ip_src = ifp->addr;

#if 0
		TRACE_ICMP("Sending ICMP Echo Reply to " FMT_IP4 "\n",
		    ARG_IP4(ip->ip_dst.s_addr));
#endif

		/*
		 * if routing reveals it's for output to same interface, can go
		 * directly back out;
		 * otherwise, since we hold our interface's per-stream mutex,
		 * we'd have to do some other way?
		 * FIXME: so here's something for the meantime...
		 * TODO: probably extract into a separate ip_output_intflocked()
		 * or something?
		 * maybe have an egress queue abstraction in STREAMS as  we have
		 * already for ingress?
		 */
		{
			struct ether_header *eh = (typeof(eh)) mp->rptr;
			struct ip *ip = (struct ip *)(eh + 1);
			struct ip_route_result rt;

			rt = ip_route_lookup(ip->ip_dst);
			if (rt.intf == NULL) {
				TRACE_ICMP("No route to " FMT_IP4 "\n",
				    ARG_IP4(ip->ip_dst.s_addr));
				str_freemsg(mp);
				return;
			}

			if (rt.intf != ifp) {
				TRACE_ICMP("Route for " FMT_IP4
				    " goes out different interface, "
				    "can't yet handle!!\n",
				    ARG_IP4(ip->ip_dst.s_addr));
				ktodo();
			}

			eh->ether_type = htons(ETHERTYPE_IP);
			ip->ip_sum = 0;
			ip->ip_sum = ip_checksum(ip, ip->ip_hl << 2);

			arp_output(rt.intf, rt.next_hop.s_addr, mp, true);

			ip_intf_release(rt.intf);
		}

		break;
	}

	default:
		TRACE_ICMP("Ignored ICMP type %d\n", icmp->icmp_type);
		str_freemsg(mp);
		break;
	}
}
