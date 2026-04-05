/*
 * Copyright (c) 2026 Cloudarox Solutions.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ifconfig.c
 * @brief Interface configuration.
 */

#define _DEFAULT_SOURCE

#include <sys/socket.h>
#include <sys/ioctl.h>

#include <linux/rtnetlink.h>
#include <net/if.h>
#include <net/if_arp.h>

#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ifconfig.h"

extern struct afhandler af_inet, af_inet6;

static struct afhandler	*af[AF_MAX] = {
	[AF_INET] = &af_inet,
	[AF_INET6] = &af_inet6,
};

static void
collect_link(const struct ifinfomsg *ifi, struct rtattr *const tb[], void *arg)
{
	struct iflist_head *iflist = arg;
	struct if_entry *ife;

	ife = calloc(1, sizeof(*ife));
	if (ife == NULL)
		err(1, "calloc");

	ife->index = ifi->ifi_index;
	ife->flags = ifi->ifi_flags;
	ife->type = ifi->ifi_type;

	if (tb[IFLA_IFNAME] != NULL)
		strlcpy(ife->name, RTA_DATA(tb[IFLA_IFNAME]),
		    sizeof(ife->name));

	if (tb[IFLA_MTU] != NULL)
		ife->mtu = *(int *)RTA_DATA(tb[IFLA_MTU]);

	if (tb[IFLA_ADDRESS] != NULL) {
		ife->hwaddrlen = RTA_PAYLOAD(tb[IFLA_ADDRESS]);
		if (ife->hwaddrlen > sizeof(ife->hwaddr))
			ife->hwaddrlen = sizeof(ife->hwaddr);
		memcpy(ife->hwaddr, RTA_DATA(tb[IFLA_ADDRESS]),
		    ife->hwaddrlen);
	}

	TAILQ_INSERT_TAIL(iflist, ife, tqentry);
}

static void
collect_addr(const struct ifaddrmsg *ifa, struct rtattr *const tb[], void *arg)
{
	struct ifalist_head *ifalist = arg;
	struct ifa_entry *ifae;
	int addrlen;

	switch (ifa->ifa_family) {
	case AF_INET:
		addrlen = 4;
		break;
	case AF_INET6:
		addrlen = 16;
		break;
	default:
		return;
	}

	ifae = calloc(1, sizeof(*ifae));
	if (ifae == NULL)
		err(1, "calloc");

	ifae->index = ifa->ifa_index;
	ifae->family = ifa->ifa_family;
	ifae->prefixlen = ifa->ifa_prefixlen;
	ifae->flags = ifa->ifa_flags;
	ifae->scope = ifa->ifa_scope;

	if (tb[IFA_LOCAL] != NULL)
		memcpy(ifae->addr, RTA_DATA(tb[IFA_LOCAL]), addrlen);
	else if (tb[IFA_ADDRESS] != NULL)
		memcpy(ifae->addr, RTA_DATA(tb[IFA_ADDRESS]), addrlen);

	if (tb[IFA_BROADCAST] != NULL) {
		ifae->has_bcast = 1;
		memcpy(ifae->bcast, RTA_DATA(tb[IFA_BROADCAST]), addrlen);
	}

	TAILQ_INSERT_TAIL(ifalist, ifae, tqentry);
}

static const struct {
	unsigned int	flag;
	const char	*name;
} iff_str[] = {
	{ IFF_UP,		"UP"		},
	{ IFF_LOOPBACK,		"LOOPBACK"	},
	{ IFF_RUNNING,		"RUNNING"	},
	{ 0, NULL },
};

static void
print_flags(unsigned int flags)
{
	bool first = true;
	size_t i;

	printf("flags=%x<", flags);
	for (i = 0; iff_str[i].name != NULL; i++) {
		if ((flags & iff_str[i].flag) == 0)
			continue;
		if (!first)
			putchar(',');
		printf("%s", iff_str[i].name);
		first = false;
	}
	putchar('>');
}

static void
print_hwaddr(const struct if_entry *ife)
{
	size_t i;

	switch (ife->type) {
	case ARPHRD_ETHER:
	case ARPHRD_IEEE80211:
		printf("\tether");
		for (i = 0; i < ife->hwaddrlen; i++)
			printf("%c%02x", i == 0 ? ' ' : ':',
			    ife->hwaddr[i]);
		putchar('\n');
		break;
	default:
		break;
	}
}

static void
print_if(const struct if_entry *ife, const struct ifalist_head *ifalist)
{
	const struct ifa_entry *ifae;

	printf("%s: ", ife->name);
	print_flags(ife->flags);
	printf(" mtu %d\n", ife->mtu);

	if (ife->hwaddrlen > 0)
		print_hwaddr(ife);

	TAILQ_FOREACH(ifae, ifalist, tqentry) {
		if (ifae->index != ife->index)
			continue;
		if (af[ifae->family] != NULL)
			af[ifae->family]->af_status(ifae);
	}
}

static void
print_allif(const struct iflist_head *iflist,
    const struct ifalist_head *ifalist)
{
	const struct if_entry *ife;

	TAILQ_FOREACH(ife, iflist, tqentry)
		print_if(ife, ifalist);
}

#if defined(__keyronex__)

#define I_PLINK 0x4003

#define SIOCSIFNAMEBYMUXID 0x89A0

static void
plumb(const char *ifname)
{
	struct ifreq ioc;
	int ipfd, iffd, muxid;
	char path[64];

	snprintf(path, sizeof(path), "/dev/%s", ifname);

	ipfd = open("/dev/ip", 0);
	if (ipfd < 0)
		err(EXIT_FAILURE, "failed to open /dev/ip");

	iffd = open(path, O_RDWR);
	if (iffd < 0)
		err(EXIT_FAILURE, "failed to open %s", path);

	muxid = ioctl(ipfd, I_PLINK, iffd);
	if (muxid < 0)
		err(EXIT_FAILURE, "I_PLINK failed on %s", path);

	memset(&ioc.ifr_name, 0, sizeof(ioc.ifr_name));
	strncpy(ioc.ifr_name, ifname, sizeof(ioc.ifr_name) - 1);
	ioc.ifr_ifindex = muxid;

	if (ioctl(ipfd, SIOCSIFNAMEBYMUXID, &ioc) < 0)
		err(EXIT_FAILURE, "SIOCSIFNAMEBYMUXID failed");

	printf("plumbed interface %s\n", ifname);

	close(iffd);
	close(ipfd);
}
#endif

static void
usage(void)
{
	fprintf(stderr, "usage: ifconfig [-a] [interface]\n");
	exit(EXIT_FAILURE);
}

bool
streq(const char *a, const char *b)
{
	return strcmp(a, b) == 0;
}

int
main(int argc, const char *argv[])
{
	struct iflist_head iflist = TAILQ_HEAD_INITIALIZER(iflist);
	struct ifalist_head ifalist = TAILQ_HEAD_INITIALIZER(ifalist);

	if (nl_open() < 0)
		err(EXIT_FAILURE, "nl_open");

	if (nl_foreach_link(collect_link, &iflist) < 0)
		err(EXIT_FAILURE, "RTM_GETLINK");
	if (nl_foreach_addr(collect_addr, &ifalist) < 0)
		err(EXIT_FAILURE, "RTM_GETADDR");

	if (argc == 1) {
		/* ifconfig */
		print_allif(&iflist, &ifalist);
		return 0;
	}

	if (argc == 2 && streq(argv[1], "-a")) {
		print_allif(&iflist, &ifalist);
		return 0;
	}

	if (argc == 2) {
		/* ifconfig <iface> */
		const char *name = argv[1];
		struct if_entry *ife;

		TAILQ_FOREACH(ife, &iflist, tqentry) {
			if (streq(ife->name, name)) {
				print_if(ife, &ifalist);
				return 0;
			}
		}
		errx(EXIT_FAILURE, "%s: no such interface", name);
	}

	for (size_t i = 2; i < argc; i++) {
		const char *tok = argv[i];
		if (streq(tok, "inet")) {
			i++;
			/*
			 * parse e.g.
			 * 192.168.178.1 [netmask 255.255.255.0]
			 * 192.168.177.1/24
			 */
			errx(EXIT_FAILURE, "inet not implemented yet");
		} else if (streq(tok, "inet6")) {
			i++;
			errx(EXIT_FAILURE, "inet6 not implemented yet");
		} else if (streq(tok, "up")) {
			errx(EXIT_FAILURE, "up not implemented yet");
		} else if (streq(tok, "down\n")) {
			errx(EXIT_FAILURE, "down not implemented yet");
#if defined(__keyronex__)
		} else if (streq(tok, "plumb")) {
			plumb(argv[1]);
#endif
		} else {
			/* implicit IPv4 address probably */
			errx(EXIT_FAILURE, "inet not implemented yet");
		}
	}

	usage();
}
