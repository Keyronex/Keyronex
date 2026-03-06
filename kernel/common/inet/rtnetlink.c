/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Mar 06 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file rtnetlink.c
 * @brief NetLink routing protocol.
 */

#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/netlinksubr.h>
#include <sys/stream.h>
#include <sys/strsubr.h>

#include <net/if.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdatomic.h>


static int
rtnl_handler(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh)
{
	switch (nlh->nlmsg_type) {

	default:
		kdprintf("rtnl_handler: unknown type %d\n", nlh->nlmsg_type);
		return -EOPNOTSUPP;
	}
}

void
rtnetlink_init(void)
{
	nl_register_protocol(NETLINK_ROUTE, rtnl_handler);
}
