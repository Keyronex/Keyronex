/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 23 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file neighbour.c
 * @brief Neighbour cache - common logic for ARP and NDP.
 *
 * TODO
 * ----
 *
 * - All NUD states
 * - Timers
 * - RCU fast path
 *
 * For RCU fast path, need to ensure when we read l2addr, we don't end race
 * with an update. Either we have a generation counter that we check before and
 * after, and do atomic reads of l2addr, or we could use an atomic pointer to
 * a separate l2addr. But that requires dealing with allocation failure, so the
 * generation counter is better. (This is like a linux 'seqlock' so might be
 * worth stealing the seqlock concept.)
 *
 * Or maybe - simpler - cache the neighbour lookup result in the route result,
 * along with a generation counter, and when we're outputting, we'll test if
 * the generation of the neighbour entry matches the generation in the route
 * result.
 */

#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/stream.h>

#include <netinet/if_ether.h>

#include <inet/ip.h>
#include <stdatomic.h>
#include <string.h>

typedef enum ndm_state {
	NUD_INCOMPLETE,
	NUD_REACHABLE,
} ndm_state_t;

typedef struct neighbour {
	TAILQ_ENTRY(neighbour) tqentry;
	atomic_uint	refcnt;
	ndm_state_t	state;
	struct ether_addr l2addr;
	union in_addr_union l3addr;
	mblk_t		*pending;	/* 1 packet (like BSD) */
} neighbour_t;

typedef struct neighbour_cache {
	kspinlock_t	lock;
	ip_if_t		*ifp;
	sa_family_t	family;
	TAILQ_HEAD(, neighbour) entries;	/* TODO RCU-friendly hash */
} neighbour_cache_t;

void arp_solicit(ip_if_t *, const struct in_addr *);
void ndp_solicit(ip_if_t *, const struct in6_addr *);

neighbour_cache_t *
neighbour_cache_new(ip_if_t *ifp, sa_family_t family)
{
	neighbour_cache_t *nc;

	kassert(family == AF_INET || family == AF_INET6);

	nc = kmem_alloc(sizeof(neighbour_cache_t));
	ke_spinlock_init(&nc->lock);
	nc->family = family;
	nc->ifp = ifp;
	TAILQ_INIT(&nc->entries);
	return nc;
}

int
neighbour_output(ip_if_t *ifp, neighbour_cache_t *nc, mblk_t *mp,
    const union in_addr_union *l3addr)
{
	ipl_t ipl;
	struct ether_addr l2addr;
	neighbour_t *n;
	uint16_t ethertype = nc->family == AF_INET ? ETHERTYPE_IP : ETHERTYPE_IPV6;
	size_t l3addr_len = nc->family == AF_INET ? sizeof(struct in_addr) :
	    sizeof(struct in6_addr);

	ipl = ke_spinlock_enter(&nc->lock);
	TAILQ_FOREACH(n, &nc->entries, tqentry) {
		if (memcmp(&n->l3addr, l3addr, l3addr_len) == 0)
			break;
	}

	if (n != NULL) {
		if (n->state == NUD_REACHABLE) {
			memcpy(&l2addr, &n->l2addr, sizeof(struct ether_addr));
			ke_spinlock_exit(&nc->lock, ipl);
			return ip_if_output(ifp, mp, ethertype, &l2addr);
		} else {
			if (n->pending != NULL) {
				str_freemsg(n->pending);
				n->pending = NULL;
			}
			n->pending = mp;
			ke_spinlock_exit(&nc->lock, ipl);
			return 0;
		}
	} else {
		n = kmem_alloc(sizeof(neighbour_t));
		if (n == NULL) {
			ke_spinlock_exit(&nc->lock, ipl);
			str_freemsg(mp);
			return -ENOMEM;
		}
		n->refcnt = 1;
		n->state = NUD_INCOMPLETE;
		memset(&n->l3addr, 0, sizeof(n->l3addr));
		memcpy(&n->l3addr, l3addr, l3addr_len);
		n->pending = mp;
		TAILQ_INSERT_HEAD(&nc->entries, n, tqentry);
		ke_spinlock_exit(&nc->lock, ipl);
		if (nc->family == AF_INET)
			arp_solicit(ifp, &l3addr->in);
		else
			ndp_solicit(ifp, &l3addr->in6);
		return 0;
	}
}

void
neighbour_cache_learn(neighbour_cache_t *nc, const union in_addr_union *l3addr,
    const struct ether_addr *l2addr)
{
	neighbour_t *n;
	mblk_t *mp = NULL;
	ipl_t ipl;
	char l3addr_str[INET6_ADDRSTRLEN];
	size_t l3addr_len = nc->family == AF_INET ? sizeof(struct in_addr) :
	    sizeof(struct in6_addr);

	inet_ntop(nc->family, l3addr, l3addr_str, sizeof(l3addr_str));

	kdprintf("neighbour: learned %s is at %02x:%02x:%02x:%02x:%02x:%02x\n",
	    l3addr_str,
	    l2addr->ether_addr_octet[0], l2addr->ether_addr_octet[1],
	    l2addr->ether_addr_octet[2], l2addr->ether_addr_octet[3],
	    l2addr->ether_addr_octet[4], l2addr->ether_addr_octet[5]);

	ipl = ke_spinlock_enter(&nc->lock);
	TAILQ_FOREACH(n, &nc->entries, tqentry) {
		if (memcmp(&n->l3addr, l3addr, l3addr_len) == 0) {
			memcpy(&n->l2addr, l2addr, sizeof(struct ether_addr));
			n->state = NUD_REACHABLE;
			if (n->pending) {
				mp = n->pending;
				n->pending = NULL;
			}
			ke_spinlock_exit(&nc->lock, ipl);
			if (mp != NULL)
				ip_if_output(nc->ifp, mp,
				    nc->family == AF_INET ? ETHERTYPE_IP :
				        ETHERTYPE_IPV6, l2addr);
			return;
		}
	}
	if (n == NULL) {
		n = kmem_alloc(sizeof(neighbour_t));
		if (n == NULL) {
			ke_spinlock_exit(&nc->lock, ipl);
			return;
		}
		n->refcnt = 1;
		n->state = NUD_REACHABLE;
		memset(&n->l3addr, 0, sizeof(n->l3addr));
		memcpy(&n->l3addr, l3addr, l3addr_len);
		memcpy(&n->l2addr, l2addr, sizeof(struct ether_addr));
		n->pending = NULL;
		TAILQ_INSERT_HEAD(&nc->entries, n, tqentry);
	}

	ke_spinlock_exit(&nc->lock, ipl);
}
