/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Mar 06 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file netlinksubr.h
 * @brief NetLink kernel-internal stuff.
 */

#ifndef ECX_SYS_NETLINKSUBR_H
#define ECX_SYS_NETLINKSUBR_H

#include <sys/stream.h>

struct nlmsghdr;

typedef int (*nl_handler_fn)(queue_t *wq, mblk_t *mp, struct nlmsghdr *nlh);

void nl_register_protocol(int protocol, nl_handler_fn handler);

void nl_send_error(queue_t *wq, struct nlmsghdr *orig_nlh, int error);
void nl_send_done(queue_t *wq, uint32_t seq, uint32_t pid);

#endif /* ECX_SYS_NETLINKSUBR_H */
