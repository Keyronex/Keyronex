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
 *
 * TODO
 * ----
 *
 * ip_ifaddr should be tied into RCU readable lists.
 */

#ifndef ECX_INET_IP_H
#define ECX_INET_IP_H

#include <sys/krx_atomic.h>
#include <sys/queue.h>

#include <net/if.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>

#include <stdbool.h>

struct queue;
struct msgb;

union sockaddr_union {
	struct sockaddr sa;
	struct sockaddr_in in;
	struct sockaddr_in6 in6;
};

typedef struct ip_ifaddr {
	TAILQ_ENTRY(ip_ifaddr) tqentry; /* should be RCU-friendly list */
	union sockaddr_union addr;
	uint8_t prefixlen;
} ip_ifaddr_t;

typedef struct ip_if {
	TAILQ_ENTRY(ip_if) tqentry;
	atomic_uint refcnt;
	char name[IFNAMSIZ];
	uint8_t mac[ETH_ALEN];
	int muxid; /* ifindex */
	TAILQ_HEAD(, ip_ifaddr) addrs; /* should be RCU-friendly list */
} ip_if_t;

ip_if_t *ip_if_new(uint8_t *mac);
void ip_if_publish(ip_if_t *, int muxid);
ip_if_t *ip_if_lookup_by_muxid(int);
ip_if_t *ip_if_lookup_by_name(const char *);
ip_if_t *ip_if_retain(ip_if_t *ifp);
void ip_if_release(ip_if_t *);

void ip_if_addr_iterate(ip_if_t *, void (*)(ip_ifaddr_t *, void *), void *ctx);

struct ip_route_result {
	ip_if_t *intf;
	struct in_addr next_hop;
};

#endif /* ECX_INET_IP_H */
