/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Thu Mar 05 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file rtnetlink.h
 * @brief NetLink for routing
 */

#ifndef ECX_LINUX_RTNETLINK_H
#define ECX_LINUX_RTNETLINK_H

#include <stdint.h>

enum rtm_type_id {
	RTM_NEWLINK = 16,
	RTM_DELLINK = 17,
	RTM_GETLINK = 18,
	RTM_SETLINK = 19,

	RTM_NEWADDR = 20,
	RTM_DELADDR = 21,
	RTM_GETADDR = 22,

	RTM_NEWROUTE = 24,
	RTM_DELROUTE = 25,
	RTM_GETROUTE = 26,

	RTM_NEWNEIGH = 28,
	RTM_DELNEIGH = 29,
	RTM_GETNEIGH = 30,
};

enum rt_class_t {
	RT_TABLE_UNSPEC = 0,
	RT_TABLE_MAIN = 254,
};

enum rt_prot {
	RTPROT_UNSPEC = 0,
	RTPROT_REDIRECT = 1,
	RTPROT_KERNEL = 2,
	RTPROT_BOOT = 3,
	RTPROT_STATIC = 4,
};

enum rt_scope_t {
	RT_SCOPE_UNIVERSE = 0,
	RT_SCOPE_LINK = 253,
	RT_SCOPE_HOST = 254,
	RT_SCOPE_NOWHERE = 255,
};

enum rt_type_t {
	RTN_UNSPEC = 0,
	RTN_UNICAST = 1,
	RTN_LOCAL = 2,
};

#define RTA_DST 		1
#define RTA_SRC 		2
#define RTA_IIF 		3
#define RTA_OIF			4
#define RTA_GATEWAY		5
#define RTA_PRIORITY		6
#define RTA_PREFSRC		7
#define RTA_TABLE		15

struct rtmsg {
	unsigned char	rtm_family;
	unsigned char	rtm_dst_len;
	unsigned char	rtm_src_len;
	unsigned char	rtm_tos;
	unsigned char	rtm_table;
	unsigned char	rtm_protocol;
	unsigned char	rtm_scope;
	unsigned char	rtm_type;
	unsigned int	rtm_flags;
};

struct rtattr {
	uint16_t	rta_len;
	uint16_t	rta_type;
};

struct ifaddrmsg {
	uint8_t		ifa_family;
	uint8_t		ifa_prefixlen;
	uint8_t		ifa_flags;
	uint8_t		ifa_scope;
	uint32_t	ifa_index;
};

enum ifa_type {
	IFA_UNSPEC,
	IFA_ADDRESS,
	IFA_LOCAL,
	__IFA_MAX,
};

#define IFA_MAX ((enum ifa_type)(__IFA_MAX - 1))


#define RTA_ALIGNTO	4
#define RTA_ALIGN(len)	(((len) + RTA_ALIGNTO - 1) & ~(RTA_ALIGNTO - 1))
#define RTA_OK(rta, remaining)	\
    ((remaining) >= (int)sizeof(struct rtattr) &&  \
	(rta)->rta_len >= sizeof(struct rtattr) && \
	(rta)->rta_len <= (remaining))
#define RTA_LENGTH(len)	(RTA_ALIGN(sizeof(struct rtattr)) + (len))
#define RTA_SPACE(len)	RTA_ALIGN(RTA_LENGTH(len))
#define RTA_PAYLOAD(rta) ((int)((rta)->rta_len) - RTA_LENGTH(0))
#define RTA_DATA(rta)	\
    ((void *)((char *)(rta) + RTA_ALIGN(sizeof(struct rtattr))))
#define RTA_NEXT(rta, remaining) \
    ((remaining) -= RTA_ALIGN((rta)->rta_len), \
     (struct rtattr *)((char *)(rta) + RTA_ALIGN((rta)->rta_len)))

#define IFA_RTA(i) \
    ((struct rtattr *)(((char *)(i)) + NLMSG_ALIGN(sizeof(struct ifaddrmsg))))
#define IFA_PAYLOAD(n) NLMSG_PAYLOAD((n), sizeof(struct ifaddrmsg))

#endif /* ECX_LINUX_RTNETLINK_H */
