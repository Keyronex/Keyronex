/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Mar 29 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ipv4_if.c
 * @brief IPv4 interface code.
 */

#include <inet/ip.h>
#include <sys/kmem.h>
#include <sys/libkern.h>

extern kspinlock_t ip_allif_lock;

int
ipv4_if_newaddr(ip_if_t *ifp, const struct in_addr *addr, uint8_t prefixlen)
{
	ipl_t ipl;
	ip_ifaddr_t *ifa;

	ifa = kmem_alloc(sizeof(*ifa));

	memset(&ifa->addr, 0x0, sizeof(ifa->addr.in));

	ifa->addr.in.sin_family = AF_INET;
	ifa->addr.in.sin_port = 0;
	ifa->addr.in.sin_addr = *addr;
	ifa->prefixlen = prefixlen;

	ipl = ke_spinlock_enter(&ip_allif_lock);
	RCULIST_INSERT_HEAD(&ifp->addrs, ifa, rlentry);
	ke_spinlock_exit(&ip_allif_lock, ipl);

	route_add_connected(&ifa->addr, ifa->prefixlen, ifp);

	return 0;
}
