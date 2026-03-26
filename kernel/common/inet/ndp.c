/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 23 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ndp.c
 * @brief Neighbour Discovery Protocol (NDP) handling.
 */

#include <sys/errno.h>
#include <sys/libkern.h>
#include <sys/stream.h>

#include <netinet/icmp6.h>
#include <netinet/ip6.h>

#include <inet/ip.h>
#include <inet/util.h>

/* TODO: more intelligent selection, and move to maybe ipv6_output.c */
ip_ifaddr_t *
ip_if_ipv6_source_select(ip_if_t *ifp)
{
	ip_ifaddr_t *ifa = NULL;

	kassert(ke_ipl() == IPL_DISP); /* i.e. an RCU read section */

	TAILQ_FOREACH(ifa, &ifp->addrs, tqentry)
		if (ifa->addr.in6.sin6_family == AF_INET6)
			break;

	return ifa;
}

bool
ip_if_has_v6addr(ip_if_t *ifp, const struct in6_addr *addr)
{
	ip_ifaddr_t *ifa;

	kassert(ke_ipl() == IPL_DISP); /* i.e. an RCU read section */

	TAILQ_FOREACH(ifa, &ifp->addrs, tqentry) {
		if (ifa->addr.in6.sin6_family == AF_INET6 &&
		    memcmp(&ifa->addr.in6.sin6_addr, addr,
			sizeof(struct in6_addr)) == 0)
			return true;
	}

	return false;
}

/*
 * TODO: do like FreeBSD for checksums: require an mblk to be passed in with rptr at start of
 * IPv6 header, offset of L4/ICMPv6 header, and write an str_mapply() to deal
 * with chains.
 */

static uint16_t
checksum_fold(uint32_t sum)
{
	while ((sum >> 16) != 0)
		sum = (sum & 0xffffU) + (sum >> 16);
	return (uint16_t)~sum;
}

uint16_t
ip_checksum_bytes(const void *buf, size_t len)
{
	const uint8_t *bytes = buf;
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i + 1 < len; i += 2)
		sum += ((uint32_t)bytes[i] << 8) | bytes[i + 1];
	if ((len & 1U) != 0)
		sum += (uint32_t)bytes[len - 1] << 8;
	return checksum_fold(sum);
}

uint16_t
ip_icmp6_checksum(const struct in6_addr *src, const struct in6_addr *dst,
    const void *payload, size_t payload_len)
{
	uint32_t sum = 0;
	const uint8_t *bytes = payload;
	uint32_t upper_len = htonl((uint32_t)payload_len);
	uint32_t nxt = htonl(IPPROTO_ICMPV6);
	size_t i;

	for (i = 0; i < sizeof(*src); i += 2)
		sum += ((const uint8_t *)src)[i] << 8 |
		    ((const uint8_t *)src)[i + 1];
	for (i = 0; i < sizeof(*dst); i += 2)
		sum += ((const uint8_t *)dst)[i] << 8 |
		    ((const uint8_t *)dst)[i + 1];
	for (i = 0; i < sizeof(upper_len); i += 2)
		sum += ((const uint8_t *)&upper_len)[i] << 8 |
		    ((const uint8_t *)&upper_len)[i + 1];
	for (i = 0; i < sizeof(nxt); i += 2)
		sum += ((const uint8_t *)&nxt)[i] << 8 |
		    ((const uint8_t *)&nxt)[i + 1];
	for (i = 0; i + 1 < payload_len; i += 2)
		sum += ((uint32_t)bytes[i] << 8) | bytes[i + 1];
	if ((payload_len & 1U) != 0)
		sum += (uint32_t)bytes[payload_len - 1] << 8;
	return checksum_fold(sum);
}

void
ndp_solicit(ip_if_t *ifp, const struct in6_addr *target)
{
	struct {
		struct ip6_hdr ip6;
		struct nd_neighbor_solicit ns;
		struct {
			struct nd_opt_hdr hdr;
			struct ether_addr lladdr;
		} opt;
	} pkt;
	ip_ifaddr_t *ifa;
	struct ether_addr dst_l2addr;
	mblk_t *mp;

	ifa = ip_if_ipv6_source_select(ifp);
	if (ifa == NULL)
		return;

	mp = str_allocb(sizeof(pkt) + sizeof(struct ether_header));
	if (mp == NULL)
		return;

	mp->rptr += sizeof(struct ether_header);
	mp->wptr = mp->rptr + sizeof(pkt);

	memset(&pkt, 0, sizeof(pkt));
	pkt.ip6.ip6_flow = htonl((uint32_t)6 << 28);
	pkt.ip6.ip6_hlim = 255;
	pkt.ip6.ip6_nxt = IPPROTO_ICMPV6;
	pkt.ip6.ip6_plen = htons(sizeof(pkt.ns) + sizeof(pkt.opt));
	pkt.ip6.ip6_src = ifa->addr.in6.sin6_addr;

	/* maybe use ipv6_solicited_node_mc() */
	pkt.ip6.ip6_dst = ipv6_solicited_node_mc(target);

	pkt.ns.nd_ns_type = ND_NEIGHBOR_SOLICIT;
	pkt.ns.nd_ns_code = 0;
	pkt.ns.nd_ns_target = *target;
	pkt.opt.hdr.nd_opt_type = ND_OPT_SOURCE_LINKADDR;
	pkt.opt.hdr.nd_opt_len = 1; /* 8-byte units */
	memcpy(&pkt.opt.lladdr, ifp->mac, sizeof(struct ether_addr));
	pkt.ns.nd_ns_hdr.icmp6_cksum = htons(ip_icmp6_checksum(&pkt.ip6.ip6_src,
	    &pkt.ip6.ip6_dst, &pkt.ns, sizeof(pkt.ns) + sizeof(pkt.opt)));

	memcpy(mp->rptr, &pkt, sizeof(pkt));

	dst_l2addr = (struct ether_addr) { {
	    0x33,
	    0x33,
	    0xff,
	    target->s6_addr[13],
	    target->s6_addr[14],
	    target->s6_addr[15],
	} };

	ip_if_output(ifp, mp, ETHERTYPE_IPV6, &dst_l2addr);
}

static int
ndp_advertise(ip_if_t *ifp, mblk_t *mp, const struct in6_addr *target,
    const struct in6_addr *dst, const struct ether_addr *dst_l2)
{
	struct {
		struct ip6_hdr ip6;
		struct nd_neighbor_advert na;
		struct {
			struct nd_opt_hdr hdr;
			struct ether_addr lladdr;
		} opt;
	} pkt;
	size_t needed = sizeof(struct ether_header) + sizeof(pkt);

	if (mp == NULL || (mp->db->lim - mp->db->base) < needed ||
	    mp->db->refcnt != 1) {
		if (mp != NULL)
			str_freemsg(mp);
		mp = str_allocb(needed);
		if (mp == NULL)
			return -ENOMEM;
	}

	mp->rptr = mp->db->base + sizeof(struct ether_header);
	mp->wptr = mp->rptr + sizeof(pkt);
	if (mp->cont)
		str_freemsg(mp->cont);
	mp->cont = NULL;

	memset(&pkt, 0, sizeof(pkt));
	pkt.ip6.ip6_flow = htonl((uint32_t)6 << 28);
	pkt.ip6.ip6_hlim = 255;
	pkt.ip6.ip6_nxt = IPPROTO_ICMPV6;
	pkt.ip6.ip6_plen = htons(sizeof(pkt.na) + sizeof(pkt.opt));
	pkt.ip6.ip6_src = *target;
	pkt.ip6.ip6_dst = *dst;

	pkt.na.nd_na_type = ND_NEIGHBOR_ADVERT;
	pkt.na.nd_na_code = 0;
	pkt.na.nd_na_target = *target;
	pkt.na.nd_na_flags_reserved = ND_NA_FLAG_OVERRIDE |
	    (IN6_IS_ADDR_MULTICAST(dst) ? 0 : ND_NA_FLAG_SOLICITED);

	pkt.opt.hdr.nd_opt_type = ND_OPT_TARGET_LINKADDR;
	pkt.opt.hdr.nd_opt_len = 1; /* units of 8 bytes */
	memcpy(&pkt.opt.lladdr, ifp->mac, sizeof(struct ether_addr));

	pkt.na.nd_na_hdr.icmp6_cksum = htons(ip_icmp6_checksum(
	    &pkt.ip6.ip6_src, &pkt.ip6.ip6_dst,
	    &pkt.na, sizeof(pkt.na) + sizeof(pkt.opt)));

	memcpy(mp->rptr, &pkt, sizeof(pkt));

	return ip_if_output(ifp, mp, ETHERTYPE_IPV6, dst_l2);
}

void
ndp_input_neighbor_solicit(ip_if_t *ifp, mblk_t *mp, ip_rxattr_t *attr,
    const struct icmp6_hdr *icmp6)
{
	size_t len;
	const struct nd_neighbor_solicit *ns;
	const uint8_t *optp;
	size_t optlen;
	const struct ether_addr *src_l2addr = NULL;
	struct ether_addr dst_l2;
	struct in6_addr src;
	struct in6_addr dst;

	len = mp->wptr - mp->rptr;

	if (len < sizeof(struct nd_neighbor_solicit)) {
		kdprintf("ndp_input: neighbor solicitation too short\n");
		str_freemsg(mp);
		return;
	}

	ns = (typeof(ns))icmp6;
	if (!ip_if_has_v6addr(ifp, &ns->nd_ns_target)) {
		str_freemsg(mp);
		return;
	}

	src = attr->l3hdr.ip6->ip6_src;

	optp = (const uint8_t *)(ns + 1);
	optlen = len - sizeof(*ns);

	if (optlen >= sizeof(struct nd_opt_hdr) + ETHER_ADDR_LEN &&
	    optp[0] == ND_OPT_SOURCE_LINKADDR && optp[1] == 1) {
		src_l2addr = (const struct ether_addr *)(optp +
		    sizeof(struct nd_opt_hdr));
		if (!IN6_IS_ADDR_UNSPECIFIED(&src))
			neighbour_cache_learn(ifp->neighbours_ipv6,
			    (const union in_addr_union *)&src,
			    src_l2addr);
	}

	if (IN6_IS_ADDR_UNSPECIFIED(&src)) {
		/* DAD, unsolicited advert to all-nodes multicast */
		dst = (struct in6_addr) { { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0,
		    0, 0, 0, 0, 0, 0, 1 } } };
		dst_l2 = (struct ether_addr){ { 0x33, 0x33, 0, 0, 0, 0x01 } };
	} else {
		dst = src;
		if (src_l2addr != NULL) {
			dst_l2 = *src_l2addr;
		} else {
			/*
			 * TODO: if attr->src_l2 is set and fine, use it?
			 * alternatively we might even put the reply out through
			 * neighbour_output() and let it resolve L2?
			 */
			str_freemsg(mp);
			return;
		}
	}

	ndp_advertise(ifp, mp, &ns->nd_ns_target, &dst, &dst_l2);
}

static void
ndp_input_neighbor_advert(ip_if_t *ifp, mblk_t *mp, ip_rxattr_t *attr,
    const struct icmp6_hdr *icmp6)
{
	const struct nd_neighbor_advert *na;
	const uint8_t *optp;
	size_t len, optlen;
	const struct ether_addr *tgt_l2addr = NULL;

	len = mp->wptr - mp->rptr;

	if (len < sizeof(struct nd_neighbor_advert)) {
		kdprintf("ndp_input: neighbor advertisement too short\n");
		str_freemsg(mp);
		return;
	}

	na = (typeof(na))icmp6;

	optp = (const uint8_t *)(na + 1);
	optlen = len - sizeof(*na);

	while (optlen >= sizeof(struct nd_opt_hdr)) {
		uint8_t opt_type = optp[0];
		uint8_t opt_len8 = optp[1]; /* in units of 8 bytes */
		size_t opt_bytes;

		if (opt_len8 == 0)
			break;
		opt_bytes = (size_t)opt_len8 * 8;
		if (opt_bytes > optlen)
			break;

		if (opt_type == ND_OPT_TARGET_LINKADDR &&
		    opt_bytes >= sizeof(struct nd_opt_hdr) + ETHER_ADDR_LEN)
			tgt_l2addr = (const struct ether_addr *)(optp +
			    sizeof(struct nd_opt_hdr));

		optp += opt_bytes;
		optlen -= opt_bytes;
	}

	/*
	 * TODO: fully conform with RFC 4861 s7.2.5. some of this logic belongs
	 * in neighbour cache?
	 *
	 * if NCE is INCOMPLETE:
	 * - if no TLLA, silently discard
	 * - else record l2 addr in NCE. if Solicited flag set, set state to
	 *   REACHABLE, otherwise STALE. set NCE.IsRouter and send pendings.
	 *
	 * if NCE is other than INCOMPLETE
	 * - if Override flag unset or l2 addr is same as in NCE, or no TLLA:
	 *   - update NCE.l2addr if TLLA included & differs
	 *   - if Solicited, set NCE to REACHABLE. if not Solicited and l2addr
	 *     was updated to another addr, set NCE to STALE. otherwise no
	 *     change to NCE state.
	 *   - update NCE.IsRouter. If it became false, remove router from
	 *     Default Router list & update Destination Cache.
	 */
	if (tgt_l2addr != NULL)
		neighbour_cache_learn(ifp->neighbours_ipv6,
		    (const union in_addr_union *)&na->nd_na_target, tgt_l2addr);
	else
		kdprintf("ndp_input: NA without target link-layer address\n");

	str_freemsg(mp);
}

void
ndp_input(ip_if_t *ifp, mblk_t *mp, ip_rxattr_t *attr)
{
	const struct icmp6_hdr *icmp6 = (typeof(icmp6))mp->rptr;

	/* icmpv6_input has already validated icmp header length */

	switch (icmp6->icmp6_type) {
	case ND_NEIGHBOR_SOLICIT:
		return ndp_input_neighbor_solicit(ifp, mp, attr, icmp6);

	case ND_NEIGHBOR_ADVERT:
		return ndp_input_neighbor_advert(ifp, mp, attr, icmp6);

	default:
		kdprintf("ndp_input: unsupported NDP message type %u\n",
		    icmp6->icmp6_type);
		str_freemsg(mp);
		return;
	}
}
