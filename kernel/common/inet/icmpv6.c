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
		kdprintf("icmpv6_input: received ICMPv6 echo request\n");
		str_freemsg(mp);
		break;

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
