/*
 * Copyright (c) 2026 Cloudarox Solutions.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ifconfig.h
 * @brief Shared types and prototypes for ifconfig.
 */

#ifndef IFCONFIG_H
#define IFCONFIG_H

#include <sys/socket.h>
#include <sys/queue.h>

#include <linux/rtnetlink.h>
#include <net/if.h>

#include <stddef.h>
#include <stdint.h>

/* collected information  from an RTM_NEWLINK messages. */
struct if_entry {
	TAILQ_ENTRY(if_entry) tqentry;
	char		 name[IFNAMSIZ];
	int		 index;
	unsigned int	 flags;	/* IFF_* */
	int		 mtu;
	uint16_t	 type;	/* ARPHRD_* */
	uint8_t		 hwaddr[8];
	size_t		 hwaddrlen;
};
TAILQ_HEAD(iflist_head, if_entry);

/* collected information from an RTM_NEWADDR message. */
struct ifa_entry {
	TAILQ_ENTRY(ifa_entry) tqentry;
	int		 index;	/* match if_entry::index */
	sa_family_t	 family;
	uint8_t		 prefixlen;
	uint8_t		 flags;
	uint8_t		 scope;
	uint8_t		 addr[16];
	uint8_t		 bcast[16];
	int		 has_bcast;
};
TAILQ_HEAD(ifalist_head, ifa_entry);

struct afhandler {
	const char	*af_name;
	void		(*af_status)(const struct ifa_entry *);
};

#endif /* IFCONFIG_H */
