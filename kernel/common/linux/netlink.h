/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Thu Mar 05 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file netlink.h
 * @brief NetLink
 */

#ifndef ECX_LINUX_NETLINK_H
#define ECX_LINUX_NETLINK_H

#include <sys/socket.h>

#include <stdint.h>

#define NETLINK_ROUTE 0

struct sockaddr_nl {
	uint8_t		nl_len;
	sa_family_t	nl_family;
	uint16_t	nl_pad;
	uint32_t	nl_pid;
	uint32_t	nl_groups;
};

struct nlmsghdr {
	uint32_t	nlmsg_len;
	uint16_t	nlmsg_type;
	uint16_t	nlmsg_flags;
	uint32_t	nlmsg_seq;
	uint32_t	nlmsg_pid;
};

#define NLM_F_REQUEST		0x1
#define NLM_F_MULTI		0x2
#define NLM_F_ACK		0x4

#define NLM_F_ROOT		0x100
#define NLM_F_MATCH		0x200
#define NLM_F_DUMP		(NLM_F_ROOT | NLM_F_MATCH)

#define NLM_F_REPLACE		0x100
#define NLM_F_EXCL		0x200
#define NLM_F_CREATE		0x400

#define NLMSG_NOOP	0x1
#define NLMSG_ERROR	0x2
#define NLMSG_DONE	0x3

struct nlmsgerr {
	int	error;
	struct	nlmsghdr msg;
};

#define NLMSG_ALIGNTO		4
#define NLMSG_ALIGN(len) \
    (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_HDRLEN \
    NLMSG_ALIGN(sizeof(struct nlmsghdr))
#define NLMSG_LENGTH(len)	((len) + NLMSG_HDRLEN)
#define NLMSG_SPACE(len)	NLMSG_ALIGN(NLMSG_LENGTH(len))
#define NLMSG_DATA(nlh)	 \
    ((void *)((char *)(nlh) + NLMSG_HDRLEN))
#define NLMSG_PAYLOAD(nlh,len)	((int)((nlh)->nlmsg_len) - NLMSG_SPACE((len)))
#define NLMSG_OK(nlh, len) \
    ((len) >= (int)sizeof(struct nlmsghdr) && \
	(nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && \
	(nlh)->nlmsg_len <= (len))
#define NLMSG_NEXT(nlh, len) \
    ((len) -= NLMSG_ALIGN((nlh)->nlmsg_len), \
    	(struct nlmsghdr *)((char *)(nlh) + NLMSG_ALIGN((nlh)->nlmsg_len)))

#endif /* ECX_LINUX_NETLINK_H */
