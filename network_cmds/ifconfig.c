#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <netinet/in.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>

static int fd;
static struct ifreq ifreqs[16];

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
		printf("%s", ifr->ifr_name);
		if (ifr->ifr_addr.sa_family == AF_INET) {
			char addrbuf[17];
			inet_ntop(ifr->ifr_addr.sa_family,
			    &((struct sockaddr_in *)&ifr->ifr_addr)->sin_addr,
			    addrbuf, sizeof(addrbuf));
			printf(" inet %s\n", addrbuf);
		}
	}
}

int
main(int argc, char *argv[])
{
	if ((fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) == -1)
		err(EXIT_FAILURE, "socket");

	list_all();
}
