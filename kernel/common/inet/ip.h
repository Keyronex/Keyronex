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

union in_addr_union {
	struct in_addr in;
	struct in6_addr in6;
};

union sockaddr_union {
	struct sockaddr sa;
	struct sockaddr_in in;
	struct sockaddr_in6 in6;
};

typedef struct neighbour_cache neighbour_cache_t;

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

	neighbour_cache_t *neighbours_ipv4;
	neighbour_cache_t *neighbours_ipv6;

	void *nic_data;
	int (*nic_wput)(void *, struct msgb *);
} ip_if_t;

typedef struct ip_rxattr {
	struct ether_addr *src_l2;
	union {
		const struct in_addr *in;
		const struct in6_addr *in6;
	} src;
	union {
		const struct in_addr *in;
		const struct in6_addr *in6;
	} dst;
} ip_rxattr_t;

ip_if_t *ip_if_new(uint8_t *mac);
void ip_if_publish(ip_if_t *, int muxid);
ip_if_t *ip_if_lookup_by_muxid(int);
ip_if_t *ip_if_lookup_by_name(const char *);
ip_if_t *ip_if_retain(ip_if_t *ifp);
void ip_if_release(ip_if_t *);

int ip_if_output(ip_if_t *, struct msgb *, uint16_t ethertype,
    const struct ether_addr *);

void ip_if_addr_iterate(ip_if_t *, void (*)(ip_ifaddr_t *, void *), void *ctx);

neighbour_cache_t *neighbour_cache_new(ip_if_t *, sa_family_t);
void neighbour_cache_learn(neighbour_cache_t *, const union in_addr_union *,
    const struct ether_addr *);

void icmpv6_input(ip_if_t *, struct msgb *, ip_rxattr_t *);
void ndp_input(ip_if_t *, struct msgb *, ip_rxattr_t *);

struct ip_route_result {
	ip_if_t *intf;
	struct in_addr next_hop;
};

/* currently missing from mlibc */
#define ip6_flow  ip6_ctlun.ip6_un1.ip6_un1_flow
#define ip6_plen  ip6_ctlun.ip6_un1.ip6_un1_plen
#define ip6_hlim  ip6_ctlun.ip6_un1.ip6_un1_hlim
#define ip6_hops  ip6_ctlun.ip6_un1.ip6_un1_hlim

#endif /* ECX_INET_IP_H */
