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

	kdprintf("Created Raw Socket\n");

	return 0;
}

int
sock_raw_ioctl(vnode_t *vn, unsigned long op, void *arg)
{
	struct ifreq *req = arg;

	switch (op) {
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
			kdprintf("no such device <%s>\n",req->ifr_ifrn.ifrn_name);
			return -ENODEV;
		}

		req->ifr_addr.sa_family = ARPHRD_ETHER;
		memcpy(req->ifr_addr.sa_data, netif->hwaddr,
		    sizeof(netif->hwaddr));
		
		return 0;
	}

	case SIOCGIFADDR: {
		struct netif *netif = netif_find(req->ifr_ifrn.ifrn_name);
		struct sockaddr_in *inaddr =
		    (struct sockaddr_in *)&req->ifr_addr;

		if (netif == NULL)
			return -ENODEV;

		inaddr->sin_family = AF_INET;
		inaddr->sin_port = 0;
		inaddr->sin_addr.s_addr = netif_ip4_addr(netif)->addr;

		return 0;
	}

	default:
		kfatal("sock_raw ioctl %lx\n", op);
	}
}

struct socknodeops raw_soops = {
	.create = sock_raw_create,
	.ioctl = sock_raw_ioctl,
};
