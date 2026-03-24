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

#include <sys/k_log.h>

#include <inet/ip.h>

void
arp_solicit(ip_if_t *ifp, const struct in_addr *target)
{
	kfatal("Implement me\n");
}
