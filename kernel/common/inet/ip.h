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

#include <linux/rtnetlink.h>
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


enum route_match {
	ROUTE_MATCH_GATEWAY = 1U << 0,
	ROUTE_MATCH_PRIORITY = 1U << 1,
	ROUTE_MATCH_PROTOCOL = 1U << 2,
	ROUTE_MATCH_SCOPE = 1U << 3,
	ROUTE_MATCH_TOS = 1U << 4,
	ROUTE_MATCH_TYPE = 1U << 5,
	ROUTE_MATCH_TABLE = 1U << 6,
	ROUTE_MATCH_IFP = 1U << 7,
};

/*
 * Key for adding/deleting routes.
 * At least prefix (and prefixlen) has to be specified. Rest are optional.
 */
typedef struct route_info {
	union sockaddr_union prefix;
	uint8_t		prefixlen;

	union sockaddr_union gateway;
	uint32_t	priority;
	uint32_t	mtu;
	uint32_t	priv_flags;
	uint32_t	rtm_flags;
	enum rt_class_t	table : 8;
	enum rt_prot	protocol : 8;
	enum rt_scope_t	scope : 8;
	enum rt_type_t	type : 8;
	uint8_t		tos;
	struct ip_if	*ifp;

	enum route_match match : 8;
} route_info_t;

/*
 * Result of performing a route lookup.
 */
typedef struct route_result {
	union sockaddr_union nexthop;
	uint32_t	rtm_flags;
	uint32_t	mtu;
	uint32_t	generation;
	struct ip_if	*ifp;	/* retained unless asked for otherwise */
} route_result_t;

typedef struct ip_rxattr {
	struct ether_addr *src_l2;
	union {
		const struct ip6_hdr *ip6;
		const struct ip *ip4;
	} l3hdr;
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

void route_info_init(route_info_t *, const union sockaddr_union *,
    uint8_t prefixlen);
void route_info_setup_spec(route_info_t *);
void route_info_set_gateway(route_info_t *, const union sockaddr_union *);
void route_info_set_priority(route_info_t *, uint32_t);
void route_info_set_protocol(route_info_t *, enum rt_prot);
void route_info_set_scope(route_info_t *, enum rt_scope_t);
void route_info_set_table(route_info_t *, enum rt_class_t);
void route_info_set_ifp(route_info_t *, struct ip_if *);
void route_info_set_tos(route_info_t *, uint8_t);
void route_info_set_type(route_info_t *, enum rt_type_t);

int route_add_connected(const union sockaddr_union *prefix, uint8_t prefixlen,
    ip_if_t *ifp);
int route_lookup(const union sockaddr_union *dst, route_result_t *out,
    bool retain_ifp);

void icmpv6_input(ip_if_t *, struct msgb *, ip_rxattr_t *);
void ndp_input(ip_if_t *, struct msgb *, ip_rxattr_t *);

int ipv6_output(struct msgb *);

uint16_t ip_icmp6_checksum(const struct in6_addr *src,
    const struct in6_addr *dst, const void *payload, size_t payload_len);

/* currently missing from mlibc */
#define ip6_flow  ip6_ctlun.ip6_un1.ip6_un1_flow
#define ip6_plen  ip6_ctlun.ip6_un1.ip6_un1_plen
#define ip6_hlim  ip6_ctlun.ip6_un1.ip6_un1_hlim
#define ip6_hops  ip6_ctlun.ip6_un1.ip6_un1_hlim

#endif /* ECX_INET_IP_H */
