/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 23 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ndp.c
 * @brief Neighbour Discovery Protocol (NDP) handling.
 */

#include <sys/libkern.h>
#include <sys/stream.h>

#include <netinet/icmp6.h>
#include <netinet/ip6.h>

#include <inet/ip.h>

void
ndp_input(ip_if_t *ifp, mblk_t *mp)
{
	const struct icmp6_hdr *icmp6 = (typeof(icmp6))mp->rptr;

	switch (icmp6->icmp6_type) {
	case ND_NEIGHBOR_SOLICIT:
		kdprintf("ndp_input: received neighbor solicitation\n");
		str_freemsg(mp);
		break;

	case ND_NEIGHBOR_ADVERT:
		kdprintf("ndp_input: received neighbor advertisement\n");
		str_freemsg(mp);
		break;

	default:
		kdprintf("ndp_input: unsupported NDP message type %u\n",
		    icmp6->icmp6_type);
		str_freemsg(mp);
		return;
	}
}
