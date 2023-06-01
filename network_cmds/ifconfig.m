#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>

#include <KernelProtocols/KXNetworking.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fd;
static struct ifreq ifreqs[16];

static void
usage(const char *progname)
{
	errx(EXIT_FAILURE,
	    "Usage:\n"
	    "%s\n"
	    "%s <interface>\n"
	    "%s <interface> <ip> <netmask> <gw>\n",
	    progname, progname, progname);
}

static void
list_if(const char *name)
{
	struct ifreq ifr;
	struct krx_if_statistics stats;

	strcpy(ifr.ifr_name, name);

	printf("%s:\n", name);

	if (ioctl(fd, SIOCGIFHWADDR, &ifr) == -1)
		err(EXIT_FAILURE, "ioctl (SIOCGIFHWADDR)");
	if (ifr.ifr_hwaddr.sa_family == ARPHRD_ETHER) {
		const uint8_t *mac = (uint8_t *)ifr.ifr_hwaddr.sa_data;
		printf("\tether: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0],
		    mac[1], mac[2], mac[3], mac[4], mac[5]);
	}

	if (ioctl(fd, SIOCGIFADDR, &ifr) == -1)
		err(EXIT_FAILURE, "ioctl (SIOCGIFCONF)");

	if (ifr.ifr_addr.sa_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
		char addrbuf[17];

		inet_ntop(sin->sin_family, &sin->sin_addr, addrbuf,
		    sizeof(addrbuf));
		printf("\tinet %s", addrbuf);

		if (ioctl(fd, SIOCGIFNETMASK, &ifr) == -1)
			err(EXIT_FAILURE, "ioctl (SIOCGIFNETMASK)");
		if (sin->sin_addr.s_addr != 0) {
			inet_ntop(sin->sin_family, &sin->sin_addr, addrbuf,
			    sizeof(addrbuf));
			printf(" netmask %s", addrbuf);
		}

		if (ioctl(fd, SIOCGIFGATEWAY, &ifr) == -1)
			err(EXIT_FAILURE, "ioctl (SIOCGIFGATEWAY)");
		if (sin->sin_addr.s_addr != 0) {
			inet_ntop(sin->sin_family, &sin->sin_addr, addrbuf,
			    sizeof(addrbuf));
			printf(" gw %s", addrbuf);
		}

		printf("\n");
	}

	ifr.ifr_data = (char *)&stats;
	if (ioctl(fd, SIOCGIFSTATS, &ifr) == -1)
		err(EXIT_FAILURE, "ioctl (SIOCGIFSTATS)");

	printf("\tRX packets %lu bytes %lu errors %lu drops %lu\n",
	    stats.rx_packets, stats.rx_bytes, stats.rx_errors, stats.rx_drops);
	printf("\tTX packets %lu bytes %lu errors %lu drops %lu\n",
	    stats.tx_packets, stats.tx_bytes, stats.tx_errors, stats.tx_drops);
}

static void
list_all(void)
{
	struct ifconf ifc;

	ifc.ifc_len = sizeof(ifreqs);
	ifc.ifc_buf = (char *)ifreqs;

	if (ioctl(fd, SIOCGIFCONF, &ifc) == -1)
		err(EXIT_FAILURE, "ioctl (SIOCGIFCONF)");

	for (int i = 0; i * sizeof(struct ifreq) < ifc.ifc_len; i++) {
		struct ifreq *ifr = &ifreqs[i];
		list_if(ifr->ifr_name);
	}
}

int
main(int argc, char *argv[])
{
	if ((fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) == -1)
		err(EXIT_FAILURE, "socket");

	if (argc == 1) {
		list_all();
	} else if (argc == 2) {
		list_if(argv[1]);
	} else {
		if (argc != 5)
			usage(argv[0]);
	}
}
