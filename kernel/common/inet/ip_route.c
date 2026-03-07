/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Mar 06 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ip_route.c
 * @brief IP routing table.
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/k_thread.h>
#include <sys/kmem.h>

#include <inet/ip.h>
#include <inet/ip_intf.h>
#include <libkern/queue.h>
#include <linux/rtnetlink.h>

typedef struct ip_route {
	LIST_ENTRY(ip_route) link;
	struct in_addr	dst;
	uint8_t		dst_len;
	struct in_addr	gateway;
	ip_intf_t	*intf;
	uint8_t		type;
	uint8_t		protocol;
	uint8_t		scope;
	uint32_t	metric;
} ip_route_t;

static LIST_HEAD(, ip_route) ip_route_list =
    LIST_HEAD_INITIALIZER(ip_route_list);
static krwlock_t ip_route_rwlock = KRWLOCK_INITIALISER;

static inline in_addr_t
prefixlen_to_mask(uint8_t len)
{
	if (len == 0)
		return 0;
	return htonl(~((1u << (32 - len)) - 1));
}

static inline uint8_t
mask_to_prefixlen(in_addr_t mask)
{
	return __builtin_popcount(ntohl(mask));
}

static inline bool
route_matches(ip_route_t *rt, struct in_addr dst)
{
	in_addr_t mask = prefixlen_to_mask(rt->dst_len);
	return (dst.s_addr & mask) == (rt->dst.s_addr & mask);
}

int
ip_route_add(struct in_addr dst, uint8_t dst_len, struct in_addr gateway,
    ip_intf_t *intf, uint8_t protocol, uint8_t scope, uint8_t type,
    uint32_t metric)
{
	ip_route_t *rt, *existing;
	in_addr_t mask;

	kassert(intf != NULL);
	kassert(dst_len <= 32);

	mask = prefixlen_to_mask(dst_len);
	dst.s_addr &= mask;

	ke_rwlock_enter_write(&ip_route_rwlock, "ip_route_add");

	LIST_FOREACH(existing, &ip_route_list, link) {
		if (existing->dst.s_addr == dst.s_addr &&
		    existing->dst_len == dst_len &&
		    existing->gateway.s_addr == gateway.s_addr) {
			ke_rwlock_exit_write(&ip_route_rwlock);
			return -EEXIST;
		}
	}

	rt = kmem_alloc(sizeof(*rt));
	if (rt == NULL) {
		ke_rwlock_exit_write(&ip_route_rwlock);
		return -ENOMEM;
	}

	rt->dst = dst;
	rt->dst_len = dst_len;
	rt->gateway = gateway;
	rt->intf = ip_intf_retain(intf);
	rt->type = type;
	rt->protocol = protocol;
	rt->scope = scope;
	rt->metric = metric;

	LIST_INSERT_HEAD(&ip_route_list, rt, link);

	ke_rwlock_exit_write(&ip_route_rwlock);

	kdprintf("ip_route_add: %08x/%d via %08x dev %s proto %d\n",
	    ntohl(dst.s_addr), dst_len, ntohl(gateway.s_addr), intf->name,
	    protocol);

	return 0;
}

struct ip_route_result
ip_route_lookup(struct in_addr dst)
{
	ip_route_t *rt, *best = NULL;
	struct ip_route_result ret = { 0 };

	ke_rwlock_enter_read(&ip_route_rwlock, "ip_route_lookup");

	LIST_FOREACH(rt, &ip_route_list, link) {
		if (!route_matches(rt, dst))
			continue;
		if (best == NULL || rt->dst_len > best->dst_len)
			best = rt;
	}

	if (best != NULL) {
		ret.intf = ip_intf_retain(best->intf);
		if (best->gateway.s_addr != INADDR_ANY)
			ret.next_hop = best->gateway;
		else
			ret.next_hop = dst;
	}

	ke_rwlock_exit_read(&ip_route_rwlock);

	return ret;
}

void
ip_route_if_up(ip_intf_t *intf)
{
	struct in_addr net;
	uint8_t prefixlen;

	if (intf->addr.s_addr == INADDR_ANY ||
	    intf->netmask.s_addr == INADDR_ANY)
		return;

	net.s_addr = intf->addr.s_addr & intf->netmask.s_addr;
	prefixlen = mask_to_prefixlen(intf->netmask.s_addr);

	ip_route_add(net, prefixlen, (struct in_addr) { .s_addr = INADDR_ANY },
	    intf, RTPROT_KERNEL, RT_SCOPE_LINK, RTN_UNICAST, 0);

	ip_route_add(intf->addr, 32, (struct in_addr) { .s_addr = INADDR_ANY },
	    intf, RTPROT_KERNEL, RT_SCOPE_HOST, RTN_LOCAL, 0);
}

void
ip_route_if_down(ip_intf_t *intf)
{
	ip_route_t *rt, *tmp;

	ke_rwlock_enter_write(&ip_route_rwlock, "ip_route_if_down");

	LIST_FOREACH_SAFE(rt, &ip_route_list, link, tmp) {
		if (rt->intf == intf && rt->protocol == RTPROT_KERNEL) {
			LIST_REMOVE(rt, link);
			ip_intf_release(rt->intf);
			kmem_free(rt, sizeof(*rt));
		}
	}

	ke_rwlock_exit_write(&ip_route_rwlock);
}
