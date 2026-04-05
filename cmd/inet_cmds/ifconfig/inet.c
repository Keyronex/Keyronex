/*
 * Copyright (c) 2026 Cloudarox Solutions.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file inet.c
 * @brief IPv4 address family handler.
 */

#include <sys/socket.h>

#include <netinet/in.h>

#include <arpa/inet.h>
#include <stdio.h>

#include "ifconfig.h"

static void
inet_status(const struct ifa_entry *ifae)
{
	char addr[INET_ADDRSTRLEN];
	char bcast[INET_ADDRSTRLEN];

	inet_ntop(AF_INET, ifae->addr, addr, sizeof(addr));
	printf("\tinet %s/%u", addr, ifae->prefixlen);

	if (ifae->has_bcast) {
		inet_ntop(AF_INET, ifae->bcast, bcast, sizeof(bcast));
		printf(" broadcast %s", bcast);
	}

	printf("\n");
}

struct afhandler af_inet = {
	.af_name = "inet",
	.af_status = inet_status,
};
