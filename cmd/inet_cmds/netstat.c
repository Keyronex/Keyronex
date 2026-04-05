/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Apr 05 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file netstat.c
 * @brief Network statistics
 */


#include <sys/socket.h>

#include <arpa/inet.h>
#include <linux/rtnetlink.h>
#include <net/if.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nlhelper.h"

static void
print_route(const struct rtmsg *rtm, struct rtattr *const tb[], void *arg)
{
	char dst_str[INET6_ADDRSTRLEN] = "default";
	char gw_str[INET6_ADDRSTRLEN] = "*";
	char ifname[IFNAMSIZ] = "*";

	if (rtm->rtm_table != RT_TABLE_MAIN)
		return;

	if (tb[RTA_DST] != NULL)
		inet_ntop(rtm->rtm_family, RTA_DATA(tb[RTA_DST]), dst_str,
		    sizeof(dst_str));

	if (tb[RTA_GATEWAY] != NULL)
		inet_ntop(rtm->rtm_family, RTA_DATA(tb[RTA_GATEWAY]), gw_str,
		    sizeof(gw_str));

	if (tb[RTA_OIF] != NULL) {
		unsigned int ifindex = *(unsigned int *)RTA_DATA(tb[RTA_OIF]);
		if_indextoname(ifindex, ifname);
	}

	printf("%-16s %-16s %-8s\n", dst_str, gw_str, ifname);
}

int
main(int argc, char *argv[])
{
	if (argc != 2 || strcmp(argv[1], "-r") != 0) {
		fprintf(stderr, "usage: netstat -r\n");
		return EXIT_FAILURE;
	}

	if (nl_open() < 0)
		err(EXIT_FAILURE, "nl_open");

	printf("%-16s %-16s %-8s\n", "Destination", "Gateway", "Iface");

	if (nl_foreach_route(print_route, NULL) < 0)
		err(EXIT_FAILURE, "nl_foreach_route");

	return EXIT_SUCCESS;
}
