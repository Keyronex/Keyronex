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

#include <sys/stream.h>
#include <netinet/ip6.h>

#include <inet/ip.h>

int
ipv6_output(mblk_t *mp)
{
	struct ip6_hdr *ip6 = (typeof(ip6))mp->rptr;

	kfatal("ipv6_output\n");
}
