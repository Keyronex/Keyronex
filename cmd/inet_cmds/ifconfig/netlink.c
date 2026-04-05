/*
 * Copyright (c) 2026 Cloudarox Solutions.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file netlink.c
 * @brief Netlink socket management and interface/address iteration.
 */

#include <sys/socket.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "ifconfig.h"

#define NL_BUFSIZE 16384

static int nlfd = -1;
static uint32_t nl_seq = 0;

int
nl_open(void)
{
	struct sockaddr_nl sa;

	nlfd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (nlfd < 0)
		return -1;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	if (bind(nlfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(nlfd);
		nlfd = -1;
		return -1;
	}

	return 0;
}

static void
nl_parse_rtattr(struct rtattr *rta, int len, struct rtattr **tb, int max)
{
	memset(tb, 0, sizeof(struct rtattr *) * (max + 1));
	for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
		if (rta->rta_type <= max)
			tb[rta->rta_type] = rta;
}

static int
nl_dump(int type, sa_family_t family, void (*cb)(struct nlmsghdr *, void *),
    void *arg)
{
	struct {
		struct nlmsghdr nlh;
		struct rtgenmsg gen;
	} req;
	char buf[NL_BUFSIZE];
	struct nlmsghdr *nlh;
	ssize_t n;

	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
	req.nlh.nlmsg_type = type;
	req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.nlh.nlmsg_seq = ++nl_seq;
	req.gen.rtgen_family = family;

	if (send(nlfd, &req, req.nlh.nlmsg_len, 0) < 0)
		return -1;

	for (;;) {
		n = recv(nlfd, buf, sizeof(buf), 0);
		if (n < 0)
			return -1;
		for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, n);
		    nlh = NLMSG_NEXT(nlh, n)) {
			if (nlh->nlmsg_type == NLMSG_DONE)
				return 0;
			if (nlh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *nlerr = NLMSG_DATA(nlh);
				errno = -nlerr->error;
				return -1;
			}
			cb(nlh, arg);
		}
	}
}

struct link_ctx {
	void (*cb)(const struct ifinfomsg *, struct rtattr *const[], void *);
	void *arg;
};

static void
link_msg(struct nlmsghdr *nlh, void *arg)
{
	struct link_ctx *ctx = arg;
	struct ifinfomsg *ifi;
	struct rtattr *tb[IFLA_MAX + 1];

	if (nlh->nlmsg_type != RTM_NEWLINK)
		return;
	ifi = NLMSG_DATA(nlh);
	nl_parse_rtattr(IFLA_RTA(ifi), IFLA_PAYLOAD(nlh), tb, IFLA_MAX);
	ctx->cb(ifi, (struct rtattr *const *)tb, ctx->arg);
}

int
nl_foreach_link(void (*cb)(const struct ifinfomsg *, struct rtattr *const[],
		    void *),
    void *arg)
{
	struct link_ctx ctx = { cb, arg };
	return nl_dump(RTM_GETLINK, AF_UNSPEC, link_msg, &ctx);
}

struct addr_ctx {
	void (*cb)(const struct ifaddrmsg *, struct rtattr *const[], void *);
	void *arg;
};

static void
addr_msg(struct nlmsghdr *nlh, void *arg)
{
	struct addr_ctx *ctx = arg;
	struct ifaddrmsg *ifa;
	struct rtattr *tb[IFA_MAX + 1];

	if (nlh->nlmsg_type != RTM_NEWADDR)
		return;
	ifa = NLMSG_DATA(nlh);
	nl_parse_rtattr(IFA_RTA(ifa), IFA_PAYLOAD(nlh), tb, IFA_MAX);
	ctx->cb(ifa, (struct rtattr *const *)tb, ctx->arg);
}

int
nl_foreach_addr(void (*cb)(const struct ifaddrmsg *, struct rtattr *const[],
		    void *),
    void *arg)
{
	struct addr_ctx ctx = { cb, arg };
	return nl_dump(RTM_GETADDR, AF_UNSPEC, addr_msg, &ctx);
}
