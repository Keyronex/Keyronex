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

union in_addr_union {
	struct in_addr in;
	struct in6_addr in6;
};

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
if_output(ip_if_t *ifp, mblk_t *mp, uint16_t ethertype, struct ether_addr *l2addr)
{
	mblk_t *ehmp = mp;
	struct ether_header *eh;

	if (STR_MBLKHEAD(ehmp) >= sizeof(struct ether_header) &&
	    ehmp->db->refcnt == 1) {
		ehmp->rptr -= sizeof(struct ether_header);
	} else {
		ehmp = str_allocb(sizeof(struct ether_header));
		if (ehmp == NULL) {
			str_freemsg(mp);
			return -ENOMEM;
		}
		ehmp->rptr += sizeof(struct ether_header);
		ehmp->cont = mp;
	}

	eh = (typeof(eh))ehmp->rptr;
	memcpy(eh->ether_dhost, l2addr, sizeof(struct ether_addr));
	memcpy(eh->ether_shost, ifp->mac, sizeof(struct ether_addr));
	eh->ether_type = htons(ethertype);

	return 0;
}

void
neighbour_cache_learn(neighbour_cache_t *nc, union in_addr_union *l3addr,
    struct ether_addr *l2addr)
{
	neighbour_t *n;
	mblk_t *mp = NULL;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&nc->lock);
	TAILQ_FOREACH(n, &nc->entries, tqentry) {
		if (memcmp(&n->l3addr, l3addr, sizeof(union in_addr_union)) ==
		    0) {
			memcpy(&n->l2addr, l2addr, sizeof(struct ether_addr));
			n->state = NUD_REACHABLE;
			if (n->pending) {
				mp = n->pending;
				n->pending = NULL;
			}
			ke_spinlock_exit(&nc->lock, ipl);
			if_output(nc->ifp, mp,
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
		memcpy(&n->l3addr, l3addr, sizeof(union in_addr_union));
		memcpy(&n->l2addr, l2addr, sizeof(struct ether_addr));
		n->pending = NULL;
		TAILQ_INSERT_HEAD(&nc->entries, n, tqentry);
	}

	ke_spinlock_exit(&nc->lock, ipl);
}
