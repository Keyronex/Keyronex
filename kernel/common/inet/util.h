/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Jan 11 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file util.h
 * @brief utilities for the internet stack
 */

#ifndef ECX_INET_UTIL_H
#define ECX_INET_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define FMT_MAC "%02x:%02x:%02x:%02x:%02x:%02x"
#define ARG_MAC(mac)                                              \
	(mac).ether_addr_octet[0], (mac).ether_addr_octet[1],     \
	    (mac).ether_addr_octet[2], (mac).ether_addr_octet[3], \
	    (mac).ether_addr_octet[4], (mac).ether_addr_octet[5]
#define ARG_MAC_U8(mac) \
	(mac)[0], (mac)[1], (mac)[2], (mac)[3], (mac)[4], (mac)[5]

#define FMT_IP4 "%d.%d.%d.%d"
#define ARG_IP4(ip_net) \
	((ntohl(ip_net) >> 24) & 0xff), \
	((ntohl(ip_net) >> 16) & 0xff), \
	((ntohl(ip_net) >>  8) & 0xff), \
	((ntohl(ip_net) >>  0) & 0xff)
#define ARG_IP4_U8(ip) (ip)[0], (ip)[1], (ip)[2], (ip)[3]

uint16_t ip_checksum(void *data, size_t len);

struct in6_addr ipv6_solicited_node_mc(const struct in6_addr *);

#endif /* ECX_INET_UTIL_H */
