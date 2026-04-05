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
 * @brief Neighbour cache
 *
 * General approach is to implement the IPv6 ND semantics since ARP is a bit
 * like a subset of ND.
 *
 * TODO
 * ----
 * - RCU fast path:
 *   - let route result store copy of neighbour data including a generation
 *     counter snapshot; would need to keep a reference to the neighbour anyway
 *     though.
 *   - or seqlock on l2addr? seems more sensible since we'd need a reference to
 *     the neighbour regardless.
 * - hash table
 * - eviction/gc when at capacity
 * - reachability hinting from L4
 */

#include <sys/errno.h>
#include <sys/k_cpu.h>
#include <sys/k_intr.h>
#include <sys/k_wait.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/stream.h>

#include <netinet/if_ether.h>

#include <inet/ip.h>
#include <stdatomic.h>
#include <string.h>

/* RFC 4861 s10 constants */
#define NUD_REACHABLE_TIME_MS	30000ULL	/* BASE_REACHABLE_TIME */
#define NUD_RETRANS_TIMER_MS	1000ULL		/* RETRANS_TIMER */
#define NUD_DELAY_PROBE_TIME_MS	5000ULL		/* DELAY_FIRST_PROBE_TIME */
#define NUD_MAX_MCAST_SOLICIT	3		/* MAX_MULTICAST_SOLICIT */
#define NUD_MAX_UCAST_SOLICIT	3		/* MAX_UNICAST_SOLICIT */

typedef enum nud_state nud_state_t;

typedef struct neighbour {
	TAILQ_ENTRY(neighbour) tqentry;
	atomic_uint	refcnt;
	nud_state_t	state;
	struct ether_addr l2addr;
	union in_addr_union l3addr;
	mblk_t		*pending;   /* as BSD, one queued packet */
	uint8_t		probes;     /* solicitation retransmit count */
	kcallout_t	callout;
	kdpc_t		dpc;
} neighbour_t;

typedef struct neighbour_cache {
	kspinlock_t	lock;
	ip_if_t		*ifp;
	sa_family_t	family;
	TAILQ_HEAD(, neighbour) entries;  /* TODO RCU-friendly hash */
} neighbour_cache_t;

void arp_solicit(ip_if_t *, const struct in_addr *);
void ndp_solicit(ip_if_t *, const struct in6_addr *);
void ndp_solicit_unicast(ip_if_t *, const struct in6_addr *,
    const struct ether_addr *);

static void
neighbour_timer_dpc(void *arg1, void *arg2)
{
	neighbour_t *n = arg1;
	neighbour_cache_t *nc = arg2;
	bool do_mcast_solicit = false;
	bool do_ucast_solicit = false;
	bool arm_retrans = false;
	union in_addr_union probe_l3 = {{0}};
	struct ether_addr probe_l2 = {{0}};
	mblk_t *mp_to_free = NULL;

	kassert_dbg(ke_ipl() == IPL_DISP);

	ke_spinlock_enter_nospl(&nc->lock);

	if (n->callout.deadline > ke_time()) {
		ke_spinlock_exit_nospl(&nc->lock);
		return;	/* stale */
	}

	switch (n->state) {
	case NUD_REACHABLE:
		n->state = NUD_STALE;
		break;

	case NUD_DELAY:
		n->state = NUD_PROBE;
		n->probes = 1;
		probe_l3 = n->l3addr;
		probe_l2 = n->l2addr;
		if (nc->family == AF_INET6)
			do_ucast_solicit = true;
		arm_retrans = true;
		break;

	case NUD_PROBE:
		if (n->probes >= NUD_MAX_UCAST_SOLICIT) {
			n->state = NUD_FAILED;
			mp_to_free = n->pending;
			n->pending = NULL;
			kdprintf("neighbour: host unreachable "
			    "(PROBE timeout)\n");
		} else {
			n->probes++;
			probe_l3 = n->l3addr;
			probe_l2 = n->l2addr;
			if (nc->family == AF_INET6)
				do_ucast_solicit = true;
			arm_retrans = true;
		}
		break;

	case NUD_INCOMPLETE:
		if (n->probes >= NUD_MAX_MCAST_SOLICIT) {
			n->state = NUD_FAILED;
			mp_to_free = n->pending;
			n->pending = NULL;
			kdprintf("neighbour: address resolution failed "
			    "(INCOMPLETE timeout)\n");
		} else {
			n->probes++;
			probe_l3 = n->l3addr;
			do_mcast_solicit = true;
			arm_retrans = true;
		}
		break;

	case NUD_STALE:
	case NUD_FAILED:
		/* stale DPC, no longer relevant */
		break;
	}

	if (arm_retrans)
		ke_callout_set(&n->callout,
		    ke_time() + NUD_RETRANS_TIMER_MS * NS_PER_MS);

	ke_spinlock_exit_nospl(&nc->lock);

	if (mp_to_free != NULL)
		str_freemsg(mp_to_free);

	if (do_mcast_solicit) {
		if (nc->family == AF_INET6)
			ndp_solicit(nc->ifp, &probe_l3.in6);
		else
			arp_solicit(nc->ifp, &probe_l3.in);
	}
	if (do_ucast_solicit)
		ndp_solicit_unicast(nc->ifp, &probe_l3.in6, &probe_l2);
}

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
	uint16_t ethertype = nc->family == AF_INET ? ETHERTYPE_IP :
	    ETHERTYPE_IPV6;
	size_t addr_len = nc->family == AF_INET ? sizeof(struct in_addr) :
	    sizeof(struct in6_addr);
	union in_addr_union probe_l3 = {{0}};

	ipl = ke_spinlock_enter(&nc->lock);
	TAILQ_FOREACH(n, &nc->entries, tqentry) {
		if (memcmp(&n->l3addr, l3addr, addr_len) == 0)
			break;
	}

	if (n != NULL) {
		switch (n->state) {
		case NUD_REACHABLE:
		case NUD_DELAY:
		case NUD_PROBE:
			memcpy(&l2addr, &n->l2addr, sizeof(struct ether_addr));
			ke_spinlock_exit(&nc->lock, ipl);
			return ip_if_output(ifp, mp, ethertype, &l2addr);

		case NUD_STALE:
			/*
			 * RFC 4861 s7.3.3: send using cached L2 address,
			 * transition to DELAY and start probe grace period
			 */
			memcpy(&l2addr, &n->l2addr, sizeof(struct ether_addr));
			n->state = NUD_DELAY;
			ke_callout_set(&n->callout,
			    ke_time() + NUD_DELAY_PROBE_TIME_MS * NS_PER_MS);
			ke_spinlock_exit(&nc->lock, ipl);
			return ip_if_output(ifp, mp, ethertype, &l2addr);

		case NUD_INCOMPLETE:
			if (n->pending != NULL)
				str_freemsg(n->pending);
			n->pending = mp;
			ke_spinlock_exit(&nc->lock, ipl);
			return 0;

		case NUD_FAILED:
			ke_spinlock_exit(&nc->lock, ipl);
			str_freemsg(mp);
			return -EHOSTUNREACH;
		}
	}

	/* no entry, make an INCOMPLETE and send first NS */
	n = kmem_alloc(sizeof(neighbour_t));
	if (n == NULL) {
		ke_spinlock_exit(&nc->lock, ipl);
		str_freemsg(mp);
		return -ENOMEM;
	}
	n->refcnt = 1;
	n->state = NUD_INCOMPLETE;
	n->probes = 1;
	memset(&n->l3addr, 0, sizeof(n->l3addr));
	memcpy(&n->l3addr, l3addr, addr_len);
	n->pending = mp;
	ke_callout_init_dpc(&n->callout, &n->dpc, neighbour_timer_dpc, n, nc);
	TAILQ_INSERT_HEAD(&nc->entries, n, tqentry);
	probe_l3 = *l3addr;

	ke_callout_set(&n->callout,
	    ke_time() + NUD_RETRANS_TIMER_MS * NS_PER_MS);

	ke_spinlock_exit(&nc->lock, ipl);


	if (nc->family == AF_INET)
		arp_solicit(ifp, &probe_l3.in);
	else
		ndp_solicit(ifp, &probe_l3.in6);

	return 0;
}

void
neighbour_cache_learn(neighbour_cache_t *nc, const union in_addr_union *l3addr,
    const struct ether_addr *l2addr, bool solicited)
{
	neighbour_t *n;
	mblk_t *mp = NULL;
	ipl_t ipl;
	char l3addr_str[INET6_ADDRSTRLEN];
	size_t addr_len = nc->family == AF_INET ? sizeof(struct in_addr) :
	    sizeof(struct in6_addr);
	bool arm_reachable = false;
	uint16_t ethertype = nc->family == AF_INET ? ETHERTYPE_IP :
	    ETHERTYPE_IPV6;

	inet_ntop(nc->family, l3addr, l3addr_str, sizeof(l3addr_str));
	kdprintf("neighbour: %s is at %02x:%02x:%02x:%02x:%02x:%02x (%s)\n",
	    l3addr_str,
	    l2addr->ether_addr_octet[0], l2addr->ether_addr_octet[1],
	    l2addr->ether_addr_octet[2], l2addr->ether_addr_octet[3],
	    l2addr->ether_addr_octet[4], l2addr->ether_addr_octet[5],
	    solicited ? "solicited" : "unsolicited");

	ipl = ke_spinlock_enter(&nc->lock);
	TAILQ_FOREACH(n, &nc->entries, tqentry) {
		if (memcmp(&n->l3addr, l3addr, addr_len) == 0)
			break;
	}

	if (n != NULL) {
		memcpy(&n->l2addr, l2addr, sizeof(struct ether_addr));
		if (solicited) {
			/*
			 * Solicited NA confirmed reachable, stop any pending
			 * probe/retransmit timer and arm the REACHABLE timer.
			 */
			ke_callout_stop(&n->callout);
			n->state = NUD_REACHABLE;
			n->probes = 0;
			arm_reachable = true;
			mp = n->pending;
			n->pending = NULL;
		} else {
			/*
			 * Unsolicited NA or NS-source learn, mark STALE
			 * (RFC 4861 s7.2.3, s7.2.6).
			 */
			ke_callout_stop(&n->callout);
			n->state = NUD_STALE;
			n->probes = 0;
		}
	} else {
		n = kmem_alloc(sizeof(neighbour_t));
		if (n == NULL) {
			ke_spinlock_exit(&nc->lock, ipl);
			return;
		}
		n->refcnt = 1;
		n->probes = 0;
		n->pending = NULL;
		memset(&n->l3addr, 0, sizeof(n->l3addr));
		memcpy(&n->l3addr, l3addr, addr_len);
		memcpy(&n->l2addr, l2addr, sizeof(struct ether_addr));
		ke_callout_init_dpc(&n->callout, &n->dpc,
		    neighbour_timer_dpc, n, nc);
		if (solicited) {
			n->state = NUD_REACHABLE;
			arm_reachable = true;
		} else {
			n->state = NUD_STALE;
		}
		TAILQ_INSERT_HEAD(&nc->entries, n, tqentry);
	}

	if (arm_reachable)
		ke_callout_set(&n->callout,
		    ke_time() + NUD_REACHABLE_TIME_MS * NS_PER_MS);

	ke_spinlock_exit(&nc->lock, ipl);


	if (mp != NULL)
		ip_if_output(nc->ifp, mp, ethertype, l2addr);
}

void
neighbour_cache_confirm(neighbour_cache_t *nc,
    const union in_addr_union *l3addr)
{
	neighbour_t *n;
	ipl_t ipl;
	size_t addr_len = nc->family == AF_INET ? sizeof(struct in_addr) :
	    sizeof(struct in6_addr);

	ipl = ke_spinlock_enter(&nc->lock);
	TAILQ_FOREACH(n, &nc->entries, tqentry) {
		if (memcmp(&n->l3addr, l3addr, addr_len) == 0)
			break;
	}

	if (n != NULL &&
	    n->state != NUD_INCOMPLETE &&
	    n->state != NUD_FAILED) {
		ke_callout_stop(&n->callout);
		n->state = NUD_REACHABLE;
		n->probes = 0;
		ke_callout_set(&n->callout,
		    ke_time() + NUD_REACHABLE_TIME_MS * NS_PER_MS);
	}

	ke_spinlock_exit(&nc->lock, ipl);

}
