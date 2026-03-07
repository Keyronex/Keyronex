/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Mar 06 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ip_intf.h
 * @brief IP interface definitions.
 */

#ifndef ECX_INET_IP_INTF_H
#define ECX_INET_IP_INTF_H

#include <sys/stream.h>

#include <net/if.h>
#include <netinet/ether.h>
#include <netinet/in.h>

struct ip_intf {
	LIST_ENTRY(ip_intf) if_list_link;
	atomic_uint refcnt;
	char name[IFNAMSIZ];
	int muxid;   /* mux id for DLPI lower stream */
	queue_t *wq; /* write queue for DLPI lower stream */

	struct in_addr addr;
	struct in_addr netmask;

	struct ether_addr mac;
	uint16_t mtu;

	mblk_t *sync_ack_mp;
	kevent_t *sync_ack_ev;

	struct arp_state *arp_state;
};

void arp_state_init(struct ip_intf *);

#endif /* ECX_INET_IP_INTF_H */
