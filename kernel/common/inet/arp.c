/*
 * Copyright (c) 2025-26 Cloudarox Solutions.
 * Created on Fri Feb 21 2025.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file arp.c
 * @brief Brief explanation.
 */

#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/stream.h>
#include <sys/strsubr.h>

#include <netinet/ether.h>
#include <netinet/in.h>

#include <inet/ip.h>
#include <inet/ip_intf.h>
#include <inet/util.h>

typedef struct arp_entry {
	enum arp_entry_state {
		ARP_NONE,
		ARP_PENDING,
		ARP_RESOLVED,
	} state;
	struct in_addr ip;
	struct ether_addr mac;
	bool valid;
	mblk_t *pending_mp;
} arp_entry_t;

#define ARP_CACHE_SIZE 16

struct arp_state {
	arp_entry_t cache[ARP_CACHE_SIZE];
	size_t rotor;
	krwlock_t lock;
};

#define TRACE_ARP(...) kdprintf("ARP: " __VA_ARGS__)

static inline in_addr_t
u8_to_ip(uint8_t *ip)
{
	in_addr_t addr;
	memcpy(&addr, ip, sizeof(in_addr_t));
	return addr;
}

static inline struct ether_addr
u8_to_mac(uint8_t *mac)
{
	struct ether_addr addr;
	memcpy(addr.ether_addr_octet, mac, ETHER_ADDR_LEN);
	return addr;
}

static arp_entry_t *
arp_cache_lookup(arp_state_t *state, in_addr_t ip, bool create_empty)
{
	arp_entry_t *empty_slot = NULL;

	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (state->cache[i].valid) {
			if (state->cache[i].ip.s_addr == ip)
				return &state->cache[i];
		} else if (empty_slot == NULL) {
			empty_slot = &state->cache[i];
		}
	}

	if (create_empty && empty_slot != NULL)
		return empty_slot;

	if (create_empty) {
		empty_slot = &state->cache[state->rotor];
		if (empty_slot->pending_mp) {
			str_freeb(empty_slot->pending_mp);
			empty_slot->pending_mp = NULL;
		}
		empty_slot->state = ARP_NONE;
		empty_slot->valid = false;
		state->rotor = (state->rotor + 1) % ARP_CACHE_SIZE;
		return empty_slot;
	}

	return NULL;
}

void
arp_state_init(ip_intf_t *ipif)
{
	arp_state_t *state;

	state = kmem_alloc(sizeof(arp_state_t));
	kassert(state != NULL);

	for (size_t i = 0; i < ARP_CACHE_SIZE; i++) {
		state->cache[i].state = ARP_NONE;
		state->cache[i].valid = false;
		state->cache[i].pending_mp = NULL;
	}

	state->rotor = 0;
	ke_rwlock_init(&state->lock);

	ipif->arp_state = state;
}

/* ifp stream lock remains held, as called from arp_input() */
static void
arp_cache_update(ip_intf_t *ifp, in_addr_t ip, const struct ether_addr *mac)
{
	arp_entry_t *entry = arp_cache_lookup(ifp->arp_state, ip, true);

	entry->ip.s_addr = ip;
	entry->mac = *mac;
	entry->state = ARP_RESOLVED;
	entry->valid = true;

	TRACE_ARP("cache updated: " FMT_IP4 " -> " FMT_MAC "\n", ARG_IP4(ip),
	    ARG_MAC(entry->mac));

	if (entry->pending_mp) {
		struct ether_header *eh = (typeof(eh))entry->pending_mp->rptr;
		eh->ether_type = htons(ETHERTYPE_IP);
		memcpy(eh->ether_shost, ifp->mac.ether_addr_octet,
		    ETHER_ADDR_LEN);
		memcpy(eh->ether_dhost, entry->mac.ether_addr_octet,
		    ETHER_ADDR_LEN);
		str_put(ifp->wq, entry->pending_mp);
		entry->pending_mp = NULL;
	}
}

/* ifp stream lock remains held, as called from arp_input(). */
static void
arp_reply(ip_intf_t *ifp, mblk_t *mp, in_addr_t ip, struct ether_addr mac)
{
	struct ether_header *eh;
	struct ether_arp *ea;

	eh = (struct ether_header *)mp->rptr;
	ea = (struct ether_arp *)(mp->rptr + sizeof(struct ether_header));

	eh->ether_type = htons(ETHERTYPE_ARP);
	memcpy(eh->ether_shost, ifp->mac.ether_addr_octet, ETHER_ADDR_LEN);
	memcpy(eh->ether_dhost, mac.ether_addr_octet, ETHER_ADDR_LEN);

	ea->ea_hdr.ar_hrd = htons(ARPHRD_ETHER);
	ea->ea_hdr.ar_pro = htons(ETHERTYPE_IP);
	ea->ea_hdr.ar_hln = ETHER_ADDR_LEN;
	ea->ea_hdr.ar_pln = sizeof(in_addr_t);
	ea->ea_hdr.ar_op = htons(ARPOP_REPLY);

	memcpy(ea->arp_sha, ifp->mac.ether_addr_octet, ETHER_ADDR_LEN);
	memcpy(ea->arp_spa, &ifp->addr, sizeof(in_addr_t));
	memcpy(ea->arp_tha, mac.ether_addr_octet, ETHER_ADDR_LEN);
	memcpy(ea->arp_tpa, &ip, sizeof(in_addr_t));

	TRACE_ARP("replying to " FMT_MAC "(" FMT_IP4 ")\n",
	    ARG_MAC_U8(ea->arp_sha), ARG_IP4_U8(ea->arp_spa));

	str_put(ifp->wq, mp);
}

void
arp_input(ip_intf_t *ifp, mblk_t *mp)
{
	arp_state_t *state = ifp->arp_state;
	struct ether_arp *arp;
	in_addr_t sender_ip, target_ip;
	struct ether_addr sender_mac, target_mac;

	arp = (struct ether_arp *)(mp->rptr + sizeof(struct ether_header));

	if (arp->ea_hdr.ar_hrd != htons(ARPHRD_ETHER) ||
	    arp->ea_hdr.ar_pro != htons(ETHERTYPE_IP) ||
	    arp->ea_hdr.ar_hln != ETHER_ADDR_LEN ||
	    arp->ea_hdr.ar_pln != sizeof(in_addr_t)) {
		TRACE_ARP("Unsuitable type\n");
		return;
	}

	sender_ip = u8_to_ip(arp->arp_spa);
	target_ip = u8_to_ip(arp->arp_tpa);
	sender_mac = u8_to_mac(arp->arp_sha);
	target_mac = u8_to_mac(arp->arp_tha);

	(void)target_mac;

	ke_rwlock_enter_write(&state->lock, "arp_input");
	arp_cache_update(ifp, sender_ip, &sender_mac);
	ke_rwlock_exit_write(&state->lock);

	switch (ntohs(arp->arp_op)) {
	case ARPOP_REQUEST:
		TRACE_ARP("request from " FMT_MAC "(" FMT_IP4
			  "): Who has " FMT_IP4 "\n",
		    ARG_MAC_U8(arp->arp_sha), ARG_IP4_U8(arp->arp_spa),
		    ARG_IP4_U8(arp->arp_tpa));

		kdprintf("Our interface address is " FMT_IP4 "\n", ARG_IP4(ifp->addr.s_addr));

		if (target_ip == ifp->addr.s_addr) {
			arp_reply(ifp, mp, sender_ip, sender_mac);
			mp = NULL; /* stolen by arp_reply() */
		}

		break;

	case ARPOP_REPLY:
		/* cache was already updated */

		TRACE_ARP("reply from " FMT_MAC " for " FMT_IP4 "\n",
		    ARG_MAC_U8(arp->arp_sha), ARG_IP4_U8(arp->arp_spa));

		break;
	}

	if (mp != NULL)
		str_freemsg(mp);
}

void
arp_output(ip_intf_t *ifp, in_addr_t dst, mblk_t *m, bool ifp_locked)
{
	arp_state_t *state = ifp->arp_state;
	struct ether_header *eh;
	mblk_t *arp_req;
	struct ether_arp *ea;
	arp_entry_t *entry;

	ke_rwlock_enter_write(&state->lock, "arp_output");

	entry = arp_cache_lookup(state, dst, true);

	if (entry->state == ARP_RESOLVED) {
		eh = (struct ether_header *)m->rptr;
		eh->ether_type = htons(ETHERTYPE_IP);
		memcpy(eh->ether_shost, ifp->mac.ether_addr_octet,
		    ETHER_ADDR_LEN);
		memcpy(eh->ether_dhost, entry->mac.ether_addr_octet,
		    ETHER_ADDR_LEN);

		ke_rwlock_exit_write(&state->lock);

		/* could be factored out... */
		if (!ifp_locked)
			str_enter(ifp->wq->stdata, "arp_output");
		str_put(ifp->wq, m);
		if (!ifp_locked)
			str_exit(ifp->wq->stdata);
		return;
	} else if (entry->state == ARP_PENDING) {
		if (entry->pending_mp)
			str_freeb(entry->pending_mp);
		entry->pending_mp = m;
		ke_rwlock_exit_write(&state->lock);
		return;
	}

	entry->state = ARP_PENDING;
	entry->ip.s_addr = dst;
	entry->valid = true;
	entry->pending_mp = m;

	arp_req = str_allocb(sizeof(struct ether_arp) +
	    sizeof(struct ether_header));
	eh = (struct ether_header *)arp_req->rptr;
	ea = (struct ether_arp *)(arp_req->rptr + sizeof(struct ether_header));
	arp_req->wptr += sizeof(struct ether_arp) + sizeof(struct ether_header);

	eh->ether_type = htons(ETHERTYPE_ARP);
	memcpy(eh->ether_shost, ifp->mac.ether_addr_octet, ETHER_ADDR_LEN);
	memset(eh->ether_dhost, 0xff, ETHER_ADDR_LEN); /* Broadcast */

	ea->ea_hdr.ar_hrd = htons(ARPHRD_ETHER);
	ea->ea_hdr.ar_pro = htons(ETHERTYPE_IP);
	ea->ea_hdr.ar_hln = ETHER_ADDR_LEN;
	ea->ea_hdr.ar_pln = sizeof(in_addr_t);
	ea->ea_hdr.ar_op = htons(ARPOP_REQUEST);

	memcpy(ea->arp_sha, ifp->mac.ether_addr_octet, ETHER_ADDR_LEN);
	memcpy(ea->arp_spa, &ifp->addr, sizeof(in_addr_t));
	memset(ea->arp_tha, 0, ETHER_ADDR_LEN);
	memcpy(ea->arp_tpa, &dst, sizeof(in_addr_t));

	ke_rwlock_exit_write(&state->lock);

	/* could be factored out... */
	if (!ifp_locked)
		str_enter(ifp->wq->stdata, "arp_output");
	str_put(ifp->wq, arp_req);
	if (!ifp_locked)
		str_exit(ifp->wq->stdata);
}
