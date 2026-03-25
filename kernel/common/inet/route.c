/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Tue Mar 24 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file route.c
 * @brief Routing table.
 */

#include <sys/errno.h>
#include <sys/k_intr.h>
#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/libkern.h>

#include <inet/ip.h>
#include <inet/radix.h>
#include <linux/rtnetlink.h>
#include <stdint.h>

typedef struct route {
	sa_family_t	family;
	atomic_uint	refcnt;
	union sockaddr_union gateway;
	union sockaddr_union prefix;
	uint8_t		prefixlen;
	enum rt_class_t	table;
	uint32_t	priority;
	uint32_t	mtu;
	enum rt_prot	protocol;
	enum rt_scope_t	scope;
	enum rt_type_t	type;
	uint8_t		tos;
	ip_if_t		*ifp;	/* retained pointer */
} route_t;

typedef struct route_table {
	kspinlock_t lock;
	radix_tree_t tree;
	atomic_uint generation;
} route_table_t;

route_table_t *tables[AF_MAX];
uint8_t family_bytes[AF_MAX] = {
	[AF_INET] = 4,
	[AF_INET6] = 16,
};

static route_table_t *
route_table_new(sa_family_t family)
{
	route_table_t *table = kmem_alloc(sizeof(*table));
	ke_spinlock_init(&table->lock);
	kassert(family_bytes[family] != 0);
	radix_tree_init(&table->tree, family_bytes[family]);
	atomic_init(&table->generation, 0);
	return table;
}

void
route_init(void)
{
	tables[AF_INET] = route_table_new(AF_INET);
	tables[AF_INET6] = route_table_new(AF_INET6);
}

static unsigned int
route_table_inc_generation(route_table_t *table)
{
	return atomic_fetch_add_explicit(&table->generation, 1,
	    memory_order_acq_rel) + 1;
}

static const uint8_t *
addr_cbytes(const union sockaddr_union *addr)
{
	switch(addr->sa.sa_family) {
		case AF_INET:
			return (const uint8_t *)&addr->in.sin_addr;

		case AF_INET6:
			return (const uint8_t *)&addr->in6.sin6_addr;

		default:
			kfatal("unsupported family %u", addr->sa.sa_family);
	}
}

/*
 * api
 */
int
route_lookup(const union sockaddr_union *dst, route_result_t *out,
    bool retain_ifp)
{
	route_result_t result = {0};
	radix_node_t *pnode;
	route_t *best;
	route_table_t *table;
	ipl_t ipl;
	int r = 0;

	kassert(dst->sa.sa_family < AF_MAX);
	table = tables[dst->sa.sa_family];
	kassert(table != NULL);

	ipl = ke_spinlock_enter(&table->lock);

	pnode = radix_longest_match(&table->tree, addr_cbytes(dst));
	if (pnode == NULL) {
		r = -ESRCH;
		goto out;
	}

	best = pnode->data;
	if (best == NULL) {
		r = -ESRCH;
		goto out;
	}

	if (best->ifp != NULL && retain_ifp)
		ip_if_retain(best->ifp);
	result.ifp = best->ifp;
	result.mtu = best->mtu;
	result.generation = atomic_load_explicit(&table->generation,
	    memory_order_acquire);
out:
	ke_spinlock_exit(&table->lock, ipl);

	if (r == 0)
		*out = result;

	return r;
}

int
route_add(route_info_t *spec)
{
	route_t *rt;
	radix_node_t *node;
	route_table_t *table;
	ipl_t ipl;
	int r = 0;

	kassert(spec->prefix.sa.sa_family < AF_MAX);
	table = tables[spec->prefix.sa.sa_family];
	kassert(table != NULL);

	kassert(spec->prefixlen <= family_bytes[spec->prefix.sa.sa_family] * 8);
	kassert(spec->gateway.sa.sa_family == AF_UNSPEC ||
	    spec->gateway.sa.sa_family == spec->prefix.sa.sa_family);

	rt = kmem_alloc(sizeof(*rt));
	if (rt == NULL)
		return -ENOMEM;

	atomic_init(&rt->refcnt, 1);
	rt->family = spec->prefix.sa.sa_family;
	rt->prefix = spec->prefix;
	rt->prefixlen = spec->prefixlen;
	rt->gateway = spec->gateway;
	rt->priority = spec->priority;
	rt->mtu = spec->mtu;
	rt->protocol = spec->protocol;
	rt->scope = spec->scope;
	rt->type = spec->type;
	rt->table = spec->table;
	rt->tos = spec->tos;
	rt->ifp = spec->ifp;

	ipl = ke_spinlock_enter(&table->lock);

	node = radix_insert(&table->tree, addr_cbytes(&spec->prefix),
	    spec->prefixlen);
	if (node->data != NULL) {
		/* only support one route per prefix for now */
		kmem_free(rt, sizeof(*rt));
		r = -EEXIST;
		goto out;
	}

	node->data = rt;
	if (rt->ifp != NULL)
		ip_if_retain(rt->ifp);
	route_table_inc_generation(table);

out:
	ke_spinlock_exit(&table->lock, ipl);

	return r;
}

/*
 * key api
 */

void
route_info_init(route_info_t *info, const union sockaddr_union *prefix,
    uint8_t prefixlen)
{
	kassert(info != NULL);
	kassert(prefix != NULL);

	memset(info, 0, sizeof(*info));
	info->prefix = *prefix;
	info->prefixlen = prefixlen;

	info->table = RT_TABLE_MAIN;
	info->protocol = RTPROT_BOOT;
	info->scope = RT_SCOPE_UNIVERSE;
	info->type = RTN_UNICAST;
}

void
route_info_set_gateway(route_info_t *info, const union sockaddr_union *gw)
{
	info->gateway = *gw;
	info->match |= ROUTE_MATCH_GATEWAY;
}

void
route_info_set_priority(route_info_t *info, uint32_t priority)
{
	info->priority = priority;
	info->match |= ROUTE_MATCH_PRIORITY;
}

void
route_info_set_protocol(route_info_t *info, enum rt_prot protocol)
{
	info->protocol = protocol;
	info->match |= ROUTE_MATCH_PROTOCOL;
}

void
route_info_set_scope(route_info_t *info, enum rt_scope_t scope)
{
	info->scope = scope;
	info->match |= ROUTE_MATCH_SCOPE;
}

void
route_info_set_table(route_info_t *info, enum rt_class_t table)
{
	info->table = table;
	info->match |= ROUTE_MATCH_TABLE;
}

void
route_info_set_ifp(route_info_t *info, ip_if_t *ifp)
{
	info->ifp = ifp;
	info->match |= ROUTE_MATCH_IFP;
}

void
route_info_set_tos(route_info_t *info, uint8_t tos)
{
	info->tos = tos;
	info->match |= ROUTE_MATCH_TOS;
}

void
route_info_set_type(route_info_t *info, enum rt_type_t type)
{
	info->type = type;
	info->match |= ROUTE_MATCH_TYPE;
}
