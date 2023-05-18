/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu May 18 2023.
 */

#include <sys/queue.h>

#include <netinet/if_ether.h>
#include <netpacket/packet.h>

#include <kdk/kernel.h>
#include <kdk/kmem.h>
#include <linux/if_ether.h>

#include "keysock/sockfs.h"
#include "lwip/netifapi.h"
#include "lwip/pbuf.h"

struct packet_packet {
	struct packet packet;
};

struct sock_packet {
	struct socknode socknode;
	TAILQ_ENTRY(sock_packet) packetsock_tailq_entry;

	/* Protocol of frames that should be received; network byte order */
	int protocol;

	/*! Receive queue. */
	struct packet_stailq rx_queue;
};

#define VNTOPKT(VN) ((struct sock_packet *)(VN)->data)

kspinlock_t packetsocks_lock = KSPINLOCK_INITIALISER;
TAILQ_HEAD(packetsock_tailq, sock_packet) packetsocks = TAILQ_HEAD_INITIALIZER(
    packetsocks);

int
packet_deliver_to_sockets(struct pbuf *p)
{
	struct ethhdr *hdr = p->payload;
	struct sock_packet *sock;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&packetsocks_lock);
	TAILQ_FOREACH (sock, &packetsocks, packetsock_tailq_entry) {
		if (sock->protocol == ETH_P_ALL ||
		    sock->protocol == hdr->h_proto) {
			struct packet_packet *packet = kmem_alloc(
			    sizeof(*packet));
			packet->packet.data = kmem_alloc(p->tot_len);
			pbuf_copy_partial(p, packet->packet.data, p->tot_len,
			    0);
			packet->packet.offset = 0;
			packet->packet.refcount = 1;
			packet->packet.size = p->tot_len;
			kdprintf("DELIVERING ON PACKET SOCKET...\n");
			ke_spinlock_acquire_nospl(&sock->socknode.lock);
			packet_add_to_queue(&sock->rx_queue, &packet->packet);
			ke_event_signal(&sock->socknode.read_evobj);
			sock_event_raise(&sock->socknode, EPOLLIN);
			ke_spinlock_release_nospl(&sock->socknode.lock);
		}
	}
	ke_spinlock_release(&packetsocks_lock, ipl);

	return 0;
}

static int
sock_packet_create(krx_out vnode_t **vnode, int domain, int type, int protocol)
{
	struct sock_packet *sock;
	ipl_t ipl;
	int r;

	sock = kmem_alloc(sizeof(*sock));
	r = sock_init(&sock->socknode, &packet_soops);
	kassert(r == 0);

	sock->protocol = protocol;
	packet_queue_init(&sock->rx_queue);

	*vnode = sock->socknode.vnode;

	ipl = ke_spinlock_acquire(&packetsocks_lock);
	TAILQ_INSERT_TAIL(&packetsocks, sock, packetsock_tailq_entry);
	ke_spinlock_release(&packetsocks_lock, ipl);

	kdprintf("Created Packet Socket\n");

	return 0;
}

static int
sock_packet_bind(vnode_t *vn, const struct sockaddr *nam, socklen_t namlen)
{
	struct sockaddr_ll *sll = (struct sockaddr_ll *)nam;

	kassert(namlen == sizeof(*sll));
	kassert(nam->sa_family == AF_PACKET);

	return 0;
}

static int
sock_packet_recv(vnode_t *vn, void *buf, size_t nbyte, int flags, ipl_t ipl)
{
	struct sock_packet *sock = VNTOPKT(vn);
	struct packet_packet *pkt;
	struct ether_header *hdr;
	struct sockaddr_ll sll;
	size_t nread;

	kassert(!(flags & MSG_PEEK));

	/* note: entered spinlocked */

	kassert(!STAILQ_EMPTY(&sock->rx_queue));
	pkt = (struct packet_packet *)STAILQ_FIRST(&sock->rx_queue);

	nread = MIN2(pkt->packet.size - sizeof(*hdr), nbyte);
	kassert(nread > 0);

	if (pkt->packet.size < (pkt->packet.size - sizeof(*hdr))) {
		kassert(pkt->packet.offset < pkt->packet.size);
		/* append MSG_TRUNC.... */
	}

	STAILQ_REMOVE_HEAD(&sock->rx_queue, stailq_entry);
	if (STAILQ_EMPTY(&sock->rx_queue))
		ke_event_clear(&sock->socknode.read_evobj);

	ke_spinlock_release(&sock->socknode.lock, ipl);

	hdr = (void *)pkt->packet.data;
	memcpy(sll.sll_addr, hdr->ether_shost, sizeof(hdr->ether_shost));
	/* set other things appropriately... */

	memcpy(buf, pkt->packet.data + sizeof(*hdr), nread);

	return nread;
}

int
sock_packet_sendmsg(vnode_t *vn, struct msghdr *msg, int flags)
{
	struct sockaddr_ll *sll = msg->msg_name;
	struct pbuf *pb;
	struct netif *netif;
	struct ether_header *hdr;
	void *data;

	kassert(sll != NULL && msg->msg_namelen == sizeof(*sll));

	netif = netif_get_by_index(sll->sll_ifindex);
	kassert(netif != NULL);

	data = kmem_alloc(sizeof(*hdr) + msg->msg_iov[0].iov_len);
	pb = pbuf_alloc_reference(data, sizeof(*hdr) + msg->msg_iov[0].iov_len,
	    PBUF_REF);

	hdr = data;
	memcpy(hdr->ether_shost, netif->hwaddr, 6);
	memcpy(hdr->ether_dhost, sll->sll_addr, 6);
	hdr->ether_type = sll->sll_protocol;

	memcpy(data + sizeof(*hdr), msg->msg_iov[0].iov_base,
	    msg->msg_iov[0].iov_len);

	netif->linkoutput(netif, pb);

	return msg->msg_iov[0].iov_len;
}

struct socknodeops packet_soops = {
	.create = sock_packet_create,
	.bind = sock_packet_bind,
	.recv = sock_packet_recv,
	.sendmsg = sock_packet_sendmsg,
};
