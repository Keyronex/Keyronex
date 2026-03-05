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

#include <net/if.h>
#include <netinet/in.h>

#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static int
recv_ack(int fd, uint32_t seq)
{
	char buf[8192];
	struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct sockaddr_nl sa;
	struct msghdr msg = { .msg_name = &sa,
		.msg_namelen = sizeof(sa),
		.msg_iov = &iov,
		.msg_iovlen = 1 };

	ssize_t len = recvmsg(fd, &msg, 0);
	if (len < 0)
		err(1, "recvmsg");

	for (struct nlmsghdr *nh = (struct nlmsghdr *)buf;
	    NLMSG_OK(nh, (unsigned)len); nh = NLMSG_NEXT(nh, len)) {
		if (nh->nlmsg_seq != seq)
			errx(1, "recv_ack: sequence number mismatch");

		if (nh->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(nh);
			if (e->error == 0)
				return 0; // ACK
			errno = -e->error;
			return -1;
		}
	}

	errno = EPROTO;
	return -1;
}

int
main(int argc, char *argv[])
{
	struct {
		struct nlmsghdr n;
		struct rtmsg r;
		char buf[512];
	} req;
	struct sockaddr_nl local = { 0 }, kernel = { 0 };
	struct in_addr gw;
	struct msghdr msg;
	struct iovec iov;
	int fd;
	uint32_t seq = 1;

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

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd == -1)
		err(1, "socket");

	local.nl_family = AF_NETLINK;
	local.nl_pid = (uint32_t)getpid();

	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) == -1)
		err(1, "bind");

	kernel.nl_family = AF_NETLINK;

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL |
	    NLM_F_ACK;
	req.n.nlmsg_type = RTM_NEWROUTE;
	req.n.nlmsg_seq = seq;

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

	iov.iov_base = &req;
	iov.iov_len = req.n.nlmsg_len;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &kernel;
	msg.msg_namelen = sizeof(kernel);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	if (sendmsg(fd, &msg, 0) == -1)
		err(1, "sendmsg");

	if (recv_ack(fd, seq) == -1)
		err(1, "route add failed");

	close(fd);
	return 0;
}
