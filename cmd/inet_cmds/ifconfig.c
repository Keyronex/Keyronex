/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Jan 11 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ifconfig.c
 * @brief Network interface configuration tool.
 */

#include <sys/ioctl.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIOCSIFADDR 0x8916
#define SIOCSIFNETMASK 0x891C
#define SIOCSIFNAMEBYMUXID 0x89A0

#define I_PLINK 0x4003

static int
usage(void)
{
	fprintf(stderr,
	    "Usage:\n"
	    "  ifconfig <interface>\n"
	    "  ifconfig <interface> plumb\n"
	    "  ifconfig <interface> <addr> [netmask <mask-hex>]\n"
	    "    e.g. ifconfig en0 192.168.1.10 netmask ffffff00\n");
	return EXIT_FAILURE;
}

static int
do_ioctl(int fd, const char *ifname, unsigned long req, struct ifreq *ifr)
{
	memset(ifr, 0, sizeof(*ifr));
	strncpy(ifr->ifr_name, ifname, sizeof(ifr->ifr_name) - 1);
	return ioctl(fd, req, ifr);
}

static const struct flag_name {
	int flag;
	const char *name;
} flag_names[] = {
	{ IFF_UP, "UP" },
	{ IFF_BROADCAST, "BROADCAST" },
	{ IFF_LOOPBACK, "LOOPBACK" },
	{ IFF_RUNNING, "RUNNING" },
};

static void
print_flags(short int flags)
{
	bool first = true;

	printf("flags=%x", flags);

	for (size_t i = 0; i < sizeof(flag_names) / sizeof(flag_names[0]);
	    i++) {
		if (flags & flag_names[i].flag) {
			if (first) {
				printf("<");
				first = false;
			} else {
				printf(",");
			}
			printf("%s", flag_names[i].name);
		}
	}
	if (!first)
		printf(">");
}

static int
print(const char *ifname)
{
	int ipfd;
	struct ifreq ifr;

	ipfd = open("/dev/ip", 0);
	if (ipfd < 0)
		err(EXIT_FAILURE, "failed to open /dev/ip");

	printf("%s:\t", ifname);

	if (do_ioctl(ipfd, ifname, SIOCGIFFLAGS, &ifr) >= 0) {
		print_flags(ifr.ifr_flags);
		printf("\n");
	}

	if (do_ioctl(ipfd, ifname, SIOCGIFADDR, &ifr) >= 0) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
		printf("\tinet %s ", inet_ntoa(sin->sin_addr));
	}

	if (do_ioctl(ipfd, ifname, SIOCGIFNETMASK, &ifr) >= 0) {
		struct sockaddr_in *sin =
		    (struct sockaddr_in *)&ifr.ifr_netmask;
		printf("netmask %x ", ntohl(sin->sin_addr.s_addr));
	}

	printf("\n");

	close(ipfd);

	return 0;
}

static int
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

	printf("ifconfig: plumbed interface %s\n", ifname);

	close(iffd);
	close(ipfd);

	return 0;
}

static int setaddr(const char *ifname, const char *addrstr,
    const char *netmaskstr)
{
	int ipfd;
	struct ifreq ifr;
	struct sockaddr_in *sin;

	ipfd = open("/dev/ip", 0);
	if (ipfd < 0)
		err(EXIT_FAILURE, "failed to open /dev/ip");

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);

	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	if (inet_aton(addrstr, &sin->sin_addr) == 0) {
		fprintf(stderr, "ifconfig: invalid address %s\n", addrstr);
		close(ipfd);
		return EXIT_FAILURE;
	}

	if (ioctl(ipfd, SIOCSIFADDR, &ifr) < 0) {
		err(EXIT_FAILURE, "SIOCSIFADDR failed on %s", ifname);
	}

	if (netmaskstr != NULL) {
		sin = (struct sockaddr_in *)&ifr.ifr_netmask;
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = htonl(strtoul(netmaskstr, NULL, 16));

		if (ioctl(ipfd, SIOCSIFNETMASK, &ifr) < 0) {
			err(EXIT_FAILURE, "SIOCSIFNETMASK failed on %s",
			    ifname);
		}
	}

	close(ipfd);

	return 0;
}

int
main(int argc, char *argv[])
{
	if (argc == 2) {
		return print(argv[1]);
	} else if (argc == 3) {
		if (strcmp(argv[2], "plumb") == 0)
			return plumb(argv[1]);
		else
			return setaddr(argv[1], argv[2], NULL);
	} else if (argc == 5) {
		if (strcmp(argv[3], "netmask") == 0)
			return setaddr(argv[1], argv[2], argv[4]);
		else
			return usage();
	} else {
		return usage();
	}
}
