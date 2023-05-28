/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu May 18 2023.
 */

#include <sys/ioctl.h>

#include <asm/sockios.h>

#include <net/if.h>
#include <net/if_arp.h>

#include <kdk/kmem.h>

#include "keysock/sockfs.h"
#include "lwip/netifapi.h"
#include "lwip/raw.h"
#include "lwip/tcpip.h"

struct sock_raw {
	struct socknode socknode;
	struct raw_pcb *raw_pcb;

	/*! Receive queue. */
	struct packet_stailq rx_queue;
};

#define VNTORAW(VN) ((struct sock_raw *)(VN)->data)

uint8_t
sock_raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p,
    const ip_addr_t *addr)
{
	kfatal("Please Implement\n");
}

int
sock_raw_create(krx_out vnode_t **vnode, int domain, int type, int protocol)
{
	struct sock_raw *sock;
	int r;

	sock = kmem_alloc(sizeof(*sock));
	r = sock_init(&sock->socknode, &raw_soops);

	kassert(r == 0);

	LOCK_TCPIP_CORE();
	sock->raw_pcb = raw_new_ip_type(type, protocol);
	UNLOCK_TCPIP_CORE();
	if (sock->raw_pcb == NULL) {
		/* TODO: needs to free vnode too */
		kmem_free(sock, sizeof(*sock));
		return -ENOBUFS;
	}

	packet_queue_init(&sock->rx_queue);

	raw_recv(sock->raw_pcb, sock_raw_recv_cb, sock);

	*vnode = sock->socknode.vnode;

	return 0;
}

static int
sock_raw_close(vnode_t *vn)
{
	struct sock_raw *sock = VNTORAW(vn);
	ipl_t ipl;

	LOCK_TCPIP_CORE();
	raw_remove(sock->raw_pcb);
	UNLOCK_TCPIP_CORE();

	/* acquire the spinlock to ensure all packet deliveries are finished */
	ipl = ke_spinlock_acquire(&sock->socknode.lock);
	ke_spinlock_release(&sock->socknode.lock, ipl);

	/* at this point */

	kmem_free(sock, sizeof(*sock));

	return 0;
}

int
sock_raw_ioctl(vnode_t *vn, unsigned long op, void *arg)
{
	struct ifreq *req = arg;

	switch (op) {
	case SIOCGIFCONF: {
		size_t i = 0;
		struct netif *netif;
		struct ifconf *ifc = arg;
		struct ifreq *ifr = ifc->ifc_req;

		LOCK_TCPIP_CORE();
		NETIF_FOREACH(netif)
		{
			if (((i * sizeof(struct ifreq)) +
				sizeof(struct ifreq)) > ifc->ifc_len)
				break;

			netif_index_to_name(netif_get_index(netif),
			    ifr[i].ifr_name);
			if (netif_ip4_addr(netif) == IP4_ADDR_ANY)
				ifr[i].ifr_addr.sa_family = AF_UNSPEC;
			else {
				ifr[i].ifr_addr.sa_family = AF_INET;
				memcpy(ifr[i].ifr_addr.sa_data,
				    netif_ip4_addr(netif),
				    sizeof(struct in_addr));
			}
			i++;
		}
		UNLOCK_TCPIP_CORE();

		ifc->ifc_len = i * sizeof(struct ifreq);

		return 0;
	}

	case SIOCGIFINDEX: {
		err_t err;
		uint8_t index;

		err = netifapi_netif_name_to_index(req->ifr_name, &index);
		if (err != ERR_OK)
			return -err_to_errno(err);

		req->ifr_ifindex = index;

		return 0;
	}

	case SIOCGIFHWADDR: {
		struct netif *netif = netif_find(req->ifr_ifrn.ifrn_name);

		if (netif == NULL) {
			kdprintf("no such device <%s>\n",
			    req->ifr_ifrn.ifrn_name);
			return -ENODEV;
		}

		req->ifr_addr.sa_family = ARPHRD_ETHER;
		memcpy(req->ifr_addr.sa_data, netif->hwaddr,
		    sizeof(netif->hwaddr));

		return 0;
	}

	case SIOCGIFADDR: {
		struct netif *netif = netif_find(req->ifr_ifrn.ifrn_name);
		struct sockaddr_in *inaddr = (void *)&req->ifr_addr;

		if (netif == NULL)
			return -ENODEV;

		inaddr->sin_family = AF_INET;
		inaddr->sin_port = 0;
		inaddr->sin_addr.s_addr = netif_ip4_addr(netif)->addr;

		return 0;
	}

	case SIOCGIFNETMASK: {
		struct netif *netif = netif_find(req->ifr_ifrn.ifrn_name);
		struct sockaddr_in *inaddr = (void *)&req->ifr_addr;

		if (netif == NULL)
			return -ENODEV;

		inaddr->sin_family = AF_INET;
		inaddr->sin_port = 0;
		inaddr->sin_addr.s_addr = netif_ip4_netmask(netif)->addr;

		return 0;
	}

	case SIOCGIFGATEWAY: {
		struct netif *netif = netif_find(req->ifr_ifrn.ifrn_name);
		struct sockaddr_in *inaddr = (void *)&req->ifr_addr;

		if (netif == NULL)
			return -ENODEV;

		inaddr->sin_family = AF_INET;
		inaddr->sin_port = 0;
		inaddr->sin_addr.s_addr = netif_ip4_gw(netif)->addr;

		return 0;
	}

	case SIOCGIFSTATS: {
		struct netif *netif = netif_find(req->ifr_ifrn.ifrn_name);
		struct krx_if_statistics *stats = (void *)req->ifr_data;
		struct stats_mib2_netif_ctrs *ctrs = &netif->mib2_counters;

		if (netif == NULL)
			return -ENODEV;

		stats->rx_bytes = ctrs->ifinoctets;
		stats->rx_packets = ctrs->ifinnucastpkts + ctrs->ifinucastpkts;
		stats->rx_drops = ctrs->ifindiscards;
		stats->rx_errors = ctrs->ifinerrors;

		stats->tx_bytes = ctrs->ifoutoctets;
		stats->tx_packets = ctrs->ifoutnucastpkts +
		    ctrs->ifoutucastpkts;
		stats->tx_drops = ctrs->ifoutdiscards;
		stats->tx_errors = ctrs->ifouterrors;

		return 0;
	}

	default:
		kfatal("sock_raw ioctl %lx\n", op);
	}
}

struct socknodeops raw_soops = {
	.create = sock_raw_create,
	.close = sock_raw_close,
	.ioctl = sock_raw_ioctl,
};
