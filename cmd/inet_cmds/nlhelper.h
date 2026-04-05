/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Apr 05 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file nlhelper.h
 * @brief Netlink helpers
 */

#ifndef ECX_INET_CMDS_NLHELPER_H
#define ECX_INET_CMDS_NLHELPER_H

int nl_open(void);
int nl_exchange(struct nlmsghdr *req);
int nl_foreach_link(void (*)(const struct ifinfomsg *,
     struct rtattr * const [], void *), void *);
int nl_foreach_addr(void (*)(const struct ifaddrmsg *,
     struct rtattr * const [], void *), void *);
int nl_foreach_route(void (*)(const struct rtmsg *,
     struct rtattr * const [], void *), void *);

#endif /* ECX_INET_CMDS_NLHELPER_H */
