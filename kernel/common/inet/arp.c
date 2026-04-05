/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 23 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file arp.c
 * @brief ARP handling.
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/libkern.h>
#include <sys/stream.h>

#include <netinet/if_ether.h>

#include <inet/ip.h>

static const struct ether_addr ether_bcast = {
	{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
};

static ip_ifaddr_t *
ip_if_ipv4_source_select(ip_if_t *ifp)
{
	ip_ifaddr_t *ifa;

	RCULIST_FOREACH(ifa, &ifp->addrs, rlentry) {
		if (ifa->addr.sa.sa_family == AF_INET)
			return ifa;
	}

	return NULL;
}


static ip_ifaddr_t *
ip_if_lookup_v4addr_noretain(ip_if_t *ifp, const struct in_addr *addr)
{
	ip_ifaddr_t *ifa;

	RCULIST_FOREACH(ifa, &ifp->addrs, rlentry) {
		if (ifa->addr.sa.sa_family != AF_INET)
			continue;
		if (memcmp(&ifa->addr.in.sin_addr, addr,
		    sizeof(struct in_addr)) == 0)
			return ifa;
	}

	return NULL;
}

void
arp_solicit(ip_if_t *ifp, const struct in_addr *target)
{
	struct ether_arp pkt;
	ip_ifaddr_t *ifa;
	mblk_t *mp;

	ifa = ip_if_ipv4_source_select(ifp);
	if (ifa == NULL)
		return;

	mp = str_allocb(sizeof(pkt) + sizeof(struct ether_header));
	if (mp == NULL)
		return;

	mp->rptr += sizeof(struct ether_header);
	mp->wptr = mp->rptr + sizeof(pkt);

	memset(&pkt, 0, sizeof(pkt));
	pkt.ea_hdr.ar_hrd = htons(ARPHRD_ETHER);
	pkt.ea_hdr.ar_pro = htons(ETHERTYPE_IP);
	pkt.ea_hdr.ar_hln = ETHER_ADDR_LEN;
	pkt.ea_hdr.ar_pln = sizeof(struct in_addr);
	pkt.ea_hdr.ar_op  = htons(ARPOP_REQUEST);

	memcpy(pkt.arp_sha, ifp->mac, ETHER_ADDR_LEN);
	memcpy(pkt.arp_spa, &ifa->addr.in.sin_addr, sizeof(struct in_addr));
	/* arp_tha left zeroed */
	memcpy(pkt.arp_tpa, target, sizeof(struct in_addr));

	memcpy(mp->rptr, &pkt, sizeof(pkt));

	ip_if_output(ifp, mp, ETHERTYPE_ARP, &ether_bcast);
}


static void
arp_reply(ip_if_t *ifp, mblk_t *mp, const struct in_addr *our_ip,
    const struct in_addr *dst_ip, const struct ether_addr *dst_mac)
{
	struct ether_arp pkt;
	size_t needed = sizeof(struct ether_header) + sizeof(pkt);

	if (mp == NULL || (mp->db->lim - mp->db->base) < needed ||
	    mp->db->refcnt != 1) {
		if (mp != NULL)
			str_freemsg(mp);
		mp = str_allocb(needed);
		if (mp == NULL)
			return;
	}

	mp->rptr = mp->db->base + sizeof(struct ether_header);
	mp->wptr = mp->rptr + sizeof(pkt);

	memset(&pkt, 0, sizeof(pkt));
	pkt.ea_hdr.ar_hrd = htons(ARPHRD_ETHER);
	pkt.ea_hdr.ar_pro = htons(ETHERTYPE_IP);
	pkt.ea_hdr.ar_hln = ETHER_ADDR_LEN;
	pkt.ea_hdr.ar_pln = sizeof(struct in_addr);
	pkt.ea_hdr.ar_op  = htons(ARPOP_REPLY);

	memcpy(pkt.arp_sha, ifp->mac, ETHER_ADDR_LEN);
	memcpy(pkt.arp_spa, our_ip, sizeof(struct in_addr));
	memcpy(pkt.arp_tha, dst_mac, ETHER_ADDR_LEN);
	memcpy(pkt.arp_tpa, dst_ip, sizeof(struct in_addr));

	memcpy(mp->rptr, &pkt, sizeof(pkt));

	ip_if_output(ifp, mp, ETHERTYPE_ARP, dst_mac);
}

void
arp_input(ip_if_t *ifp, mblk_t *mp)
{
	const struct ether_arp *arp;
	size_t avail;
	struct in_addr sender_ip, target_ip;
	struct ether_addr sender_mac;
	ip_ifaddr_t *ifa;
	uint16_t op;

	avail = mp->wptr - mp->rptr;
	if (avail < sizeof(struct ether_arp)) {
		kdprintf("arp_input: packet too short\n");
		str_freemsg(mp);
	 	return;
	}

	arp = (typeof(arp))mp->rptr;

	if (ntohs(arp->ea_hdr.ar_hrd) != ARPHRD_ETHER ||
	    ntohs(arp->ea_hdr.ar_pro) != ETHERTYPE_IP ||
	    arp->ea_hdr.ar_hln != ETHER_ADDR_LEN ||
	    arp->ea_hdr.ar_pln != sizeof(struct in_addr)) {
		str_freemsg(mp);
		return;
	}

	op = ntohs(arp->ea_hdr.ar_op);

	memcpy(&sender_ip, arp->arp_spa, sizeof(sender_ip));
	memcpy(&target_ip, arp->arp_tpa, sizeof(target_ip));
	memcpy(&sender_mac, arp->arp_sha, sizeof(sender_mac));

	ifa = ip_if_lookup_v4addr_noretain(ifp, &target_ip);

	if (sender_ip.s_addr != INADDR_ANY) {
		neighbour_cache_learn(ifp->neighbours_ipv4,
		    (const union in_addr_union *)&sender_ip,
		    &sender_mac,
		    op == ARPOP_REPLY /* solicited */);
	}

	if (ifa != NULL && op == ARPOP_REQUEST)
		arp_reply(ifp, mp, &ifa->addr.in.sin_addr, &sender_ip,
		    &sender_mac);
	else
		str_freemsg(mp);
}
