/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Mar 22 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ip_if.c
 * @brief IP interface management.
 */

#include <sys/k_intr.h>
#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/libkern.h>

#include <inet/ip.h>

TAILQ_HEAD(, ip_if) ip_allif = TAILQ_HEAD_INITIALIZER(ip_allif);
kspinlock_t ip_allif_lock = KSPINLOCK_INITIALISER;

ip_if_t *
ip_if_new(uint8_t *mac)
{
	ip_if_t *ifp = kmem_alloc(sizeof(ip_if_t));
	ifp->refcnt = 1;
	ifp->muxid = -1;
	memcpy(ifp->mac, mac, ETH_ALEN);
	return ifp;
}

void
ip_if_publish(ip_if_t *ifp, int muxid)
{
	ipl_t ipl = ke_spinlock_enter(&ip_allif_lock);
	ifp->muxid = muxid;
	TAILQ_INSERT_TAIL(&ip_allif, ifp, tqentry);
	ke_spinlock_exit(&ip_allif_lock, ipl);
}

ip_if_t *
ip_if_lookup_by_muxid(int muxid)
{
	ip_if_t *ifp;
	ipl_t ipl = ke_spinlock_enter(&ip_allif_lock);
	TAILQ_FOREACH(ifp, &ip_allif, tqentry) {
		if (ifp->muxid == muxid) {
			ip_if_retain(ifp);
			break;
		}
	}
	ke_spinlock_exit(&ip_allif_lock, ipl);
	return ifp;

}

ip_if_t *
ip_if_lookup_by_name(const char *name)
{
	ip_if_t *ifp;
	ipl_t ipl = ke_spinlock_enter(&ip_allif_lock);
	TAILQ_FOREACH(ifp, &ip_allif, tqentry) {
		if (strcmp(ifp->name, name) == 0) {
			ip_if_retain(ifp);
			break;
		}
	}
	ke_spinlock_exit(&ip_allif_lock, ipl);
	return ifp;
}

ip_if_t *
ip_if_retain(ip_if_t *ifp)
{
	atomic_fetch_add_explicit(&ifp->refcnt, 1, memory_order_relaxed);
	return ifp;
}

void
ip_if_release(ip_if_t *ifp)
{
	if (atomic_fetch_sub_explicit(&ifp->refcnt, 1, memory_order_relaxed) ==
	    1) {
		kdprintf("ip_if_release: todo free if %s\n", ifp->name);
	}
}
