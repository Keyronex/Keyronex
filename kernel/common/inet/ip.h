/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Jan 09 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ip.h
 * @brief IP stack header.
 */

#ifndef ECX_INET_IP_H
#define ECX_INET_IP_H

#include <netinet/in.h>

struct queue;

typedef struct ip_intf ip_intf_t;
typedef struct arp_state arp_state_t;

ip_intf_t *ip_intf_retain(ip_intf_t *intf);
void ip_intf_release(ip_intf_t *intf);

ip_intf_t *ip_intf_lookup_by_muxid(int muxid);
ip_intf_t *ip_intf_lookup_by_name(const char *name);

struct ip_route_result {
	ip_intf_t *intf;
	struct in_addr next_hop;
};

int ip_route_add(struct in_addr dst, uint8_t dst_len, struct in_addr gateway,
    ip_intf_t *intf, uint8_t protocol, uint8_t scope, uint8_t type,
    uint32_t metric);

struct ip_route_result ip_route_lookup(struct in_addr dst);

/* connected-network route management */
void ip_route_if_up(ip_intf_t *intf);
void ip_route_if_down(ip_intf_t *intf);

#endif /* ECX_INET_IP_H */
