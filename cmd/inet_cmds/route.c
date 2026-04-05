/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Thu Mar 05 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file route.c
 * @brief Routing administration tool.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nlhelper.h"

static void
addattr_l(struct nlmsghdr *n, int maxlen, int type, const void *data,
    size_t alen)
{
	int len = RTA_LENGTH(alen);
	struct rtattr *rta;

	if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > (unsigned)maxlen)
		errx(1, "addattr_l: message exceeded maxlen");

	rta = (struct rtattr *)(((char *)n) + NLMSG_ALIGN(n->nlmsg_len));
	rta->rta_type = type;
	rta->rta_len = len;
	memcpy(RTA_DATA(rta), data, alen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
}

int
main(int argc, char *argv[])
{
	struct {
		struct nlmsghdr n;
		struct rtmsg r;
		char buf[512];
	} req;
	struct in_addr gw;

	if (argc != 4 && argc != 5) {
		fprintf(stderr, "usage: %s add default <gateway> [ifname]\n",
		    argv[0]);
		exit(1);
	}
	if (strcmp(argv[1], "add") != 0 || strcmp(argv[2], "default") != 0) {
		fprintf(stderr, "usage: %s add default <gateway> [ifname]\n",
		    argv[0]);
		exit(1);
	}

	if (inet_pton(AF_INET, argv[3], &gw) != 1)
		errx(1, "invalid gateway IPv4 address: %s", argv[3]);

	if (nl_open() < 0)
		err(1, "nl_open");

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
	req.n.nlmsg_type = RTM_NEWROUTE;

	req.r.rtm_family = AF_INET;
	req.r.rtm_table = RT_TABLE_MAIN;
	req.r.rtm_protocol = RTPROT_STATIC;
	req.r.rtm_scope = RT_SCOPE_UNIVERSE;
	req.r.rtm_type = RTN_UNICAST;
	req.r.rtm_dst_len = 0; // default

	addattr_l(&req.n, sizeof(req), RTA_GATEWAY, &gw, sizeof(gw));

	if (argc == 5) {
		unsigned ifindex = if_nametoindex(argv[4]);
		if (ifindex == 0)
			errx(1, "unknown interface: %s", argv[4]);
		addattr_l(&req.n, sizeof(req), RTA_OIF, &ifindex,
		    sizeof(ifindex));
	}

	if (nl_exchange(&req.n) < 0)
		err(1, "route add failed");

	return 0;
}
