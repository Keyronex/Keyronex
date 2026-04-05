/*
 * Copyright (c) 2026 Cloudarox Solutions.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file inet6.c
 * @brief IPv6 address family handler.
 */

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <stdio.h>

#include "ifconfig.h"

static void
inet6_status(const struct ifa_entry *ifae)
{
	char addr[INET6_ADDRSTRLEN];

	inet_ntop(AF_INET6, ifae->addr, addr, sizeof(addr));
	printf("\tinet6 %s prefixlen %u\n", addr, ifae->prefixlen);
}

struct afhandler af_inet6 = {
	.af_name = "inet6",
	.af_status = inet6_status,
};
