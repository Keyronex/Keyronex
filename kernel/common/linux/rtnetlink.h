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

#define RTM_NEWROUTE		24

#define RT_TABLE_UNSPEC		0
#define RT_TABLE_MAIN		254

#define RTPROT_BOOT		3
#define RTPROT_STATIC		4

#define RT_SCOPE_UNIVERSE	0

#define RTN_UNICAST		1

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

#endif /* ECX_LINUX_RTNETLINK_H */
