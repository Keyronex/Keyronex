/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Mar 27 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ipv6_if.c
 * @brief Brief explanation.
 */

#include <sys/k_cpu.h>
#include <sys/k_log.h>
#include <sys/libkern.h>
#include <sys/kmem.h>

#include <inet/ip.h>

#define NUD_RETRANS_TIMER NS_PER_S /* 1s */

extern kspinlock_t ip_allif_lock;

static void
ipv6_ifaddr_dad_dpc(void *arg1, void *arg2)
{
	ip_ifaddr_t *ifa = arg2;

	ke_spinlock_enter_nospl(&ip_allif_lock);

	if (ifa->ipv6_state == IFADDR_TENTATIVE)
		ifa->ipv6_state = IFADDR_PREFERRED;

	/* if not, then this was already handled */

	ke_spinlock_exit_nospl(&ip_allif_lock);
}

int
ipv6_if_newaddr(ip_if_t *ifp, const struct in6_addr *addr, uint8_t prefixlen)
{
	ipl_t ipl;
	ip_ifaddr_t *ifa;

	ifa = kmem_alloc(sizeof(*ifa));

	memset(&ifa->addr, 0x0, sizeof(ifa->addr.in6));

	ifa->addr.in6.sin6_family = AF_INET6;
	ifa->addr.in6.sin6_port = 0;
	ifa->addr.in6.sin6_flowinfo = 0;
	memcpy(&ifa->addr.in6.sin6_addr, addr, sizeof(*addr));
	ifa->addr.in6.sin6_scope_id = ifp->muxid;
	ifa->prefixlen = prefixlen;

	ifa->ipv6_state = IFADDR_TENTATIVE;
	ifa->dad_probes_nsent = 0;

	ipl = ke_spinlock_enter(&ip_allif_lock);
	RCULIST_INSERT_HEAD(&ifp->addrs, ifa, rlentry);
	ke_spinlock_exit(&ip_allif_lock, ipl);

	route_add_connected(&ifa->addr, ifa->prefixlen, ifp);

	ke_callout_init_dpc(&ifa->dad_callout, &ifa->dad_dpc,
	    ipv6_ifaddr_dad_dpc, ifp, ifa);
	ndp_solicit_dad(ifp, &ifa->addr.in6.sin6_addr);
	ke_callout_set(&ifa->dad_callout, ke_time() + NUD_RETRANS_TIMER);

	return 0;
}

void
ipv6_ifaddr_dad_fail(ip_ifaddr_t *ifa)
{
	char buf[INET6_ADDRSTRLEN];
	ipl_t ipl;

	inet_ntop(AF_INET6, &ifa->addr.in6.sin6_addr, buf, sizeof(buf));

	ipl = ke_spinlock_enter(&ip_allif_lock);
	if (ifa->ipv6_state == IFADDR_TENTATIVE) {
		ke_callout_stop(&ifa->dad_callout);
		ifa->ipv6_state = IFADDR_DUPLICATED;
		kdprintf("ipv6: DAD failed for %s\n", buf);
	}
	ke_spinlock_exit(&ip_allif_lock, ipl);
}
