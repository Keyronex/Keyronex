/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#ifndef KRX_KEYSOCK_SOCKFS_H
#define KRX_KEYSOCK_SOCKFS_H

#include <sys/socket.h>

#include <stdint.h>

#include "executive/epoll.h"
#include "kdk/vfs.h"
#include "lwip/inet.h"

enum chpoll_kind;

/*!
 * Base type of a node in the socket filesystem representing a KeySock socket.
 *
 * Protocols extend this struct.
 *
 * The lifetime of the socket is by default the lifetime of the vnode describing
 * it. The #vnode pointer is weak. The exception is when a socket is on an
 * accept queue. In that case the reference to the socket is considered to also
 * keep alive the reference to the vnode.
 *
 * (l) => #lock
 * (p) => #polllist->lock
 * (~) => invariant from creation
 */
struct socknode {
	/*! (per-socket) Associated vnode; weak unless socket on accept queue */
	vnode_t *vnode;
	/*! (~) Socket operations vector. */
	struct socknodeops *sockops;
	/*! (p) State */
	enum sockstate {
		kSockInitialised,
		kSockListening,
		kSockConnected,
		kSockMax,
	} state;
	int domain;   /* (~) */
	int type;     /* (~) */
	int protocol; /* (~) */

	/*! Lock. */
	kspinlock_t lock;

	/*! () Poll list. */
	struct polllist polllist;

	/*! (l) Event indicating current readability. */
	kevent_t read_evobj;
	/*! (l to set) Event to wait on for accept() */
	kevent_t accept_evobj;
	/*! (l) Queue of sockets awaiting accept() */
	STAILQ_HEAD(, socknode) accept_stailq;
	/* Linkage in another socket's #accept_stailq */
	STAILQ_ENTRY(socknode) accept_stailq_entry;
};

/*!
 * Socket node operations. Most are entered unlocked.
 * Those annoteted "unlocks" are entered with spinlock held and exit unlocked.
 */
struct socknodeops {
	int (*create)(krx_out vnode_t **out_vn, int domain, int type,
	    int protocol);
	int (*close)(vnode_t *vn);
	int (*accept)(vnode_t *vn, krx_out struct sockaddr *addr,
	    krx_inout socklen_t *addrlen);
	int (*bind)(vnode_t *vn, const struct sockaddr *nam, socklen_t namlen);
	int (*ioctl)(vnode_t *vn, unsigned long op, void *arg);
	int (*listen)(vnode_t *vn, uint8_t backlog);
	int (*connect)(vnode_t *vn, const struct sockaddr *nam,
	    socklen_t addr_len);
	int (*chpoll)(vnode_t *vn, struct pollhead *, enum chpoll_kind);
	int (*pair)(vnode_t *vn, vnode_t *vn2);
	int (*recv)(vnode_t *vn, void *buf, size_t nbyte, int flags,
	    ipl_t ipl); /* (unlocks) */
	int (*send)(vnode_t *vn, void *buf, size_t nbyte);
	int (*sendmsg)(vnode_t *vn, struct msghdr *msg, int flags);
};

/*!
 * Packet structure used for raw, TCP, UDP, and Unix sockets alike.
 */
struct packet {
	/*! Linkage in queue. */
	STAILQ_ENTRY(packet) stailq_entry;
	/*! Byte size of the main data body of the packet. */
	uint32_t size;
	/*! Bytes already read. */
	uint32_t offset;
	/*! Main data body. */
	uint8_t *data;
	/*! Reference count. */
	uint32_t refcount;
};

/*!
 * Packet simplequeue.
 */
STAILQ_HEAD(packet_stailq, packet);

void packet_queue_init(struct packet_stailq *queue);
void packet_add_to_queue(struct packet_stailq *queue,
    struct packet *packet);

int sock_accept(vnode_t *vn, krx_out struct sockaddr *addr,
    krx_inout socklen_t *addrlen, krx_out vnode_t **out);
int sock_create(int domain, int type, int protocol, vnode_t **out);
int sock_bind(vnode_t *vn, const struct sockaddr *addr, socklen_t addrlen);
int sock_connect(vnode_t *vn, const struct sockaddr *addr, socklen_t addrlen);
int sock_listen(vnode_t *vn, uint8_t backlog);
int sock_pair(vnode_t *vn1, vnode_t *vn2);
int sock_recvmsg(vnode_t *vn, struct msghdr *msg, int flags);
int sock_sendmsg(vnode_t *vn, struct msghdr *msg, int flags);
int sock_getsockopt(vnode_t *vn, int layer, int number, void *__restrict buffer,
    socklen_t *__restrict size);
int sock_setsockopt(vnode_t *vn, int layer, int number, void *buffer,
    socklen_t size);

/* internal functions */
/*! @brief Unack an IP/port from a sockaddr. */
int addr_unpack_ip(const struct sockaddr *nam, socklen_t namlen,
    krx_out ip_addr_t *ip_out, krx_out uint16_t *port_out);
/*! @brief Pack an IP/port into a sockaddr. */
int addr_pack_ip(krx_out struct sockaddr *addr, krx_inout socklen_t *addrlen,
    ip_addr_t *ip, uint16_t port);

/*! @brief Initialise a new socket node.*/
int sock_init(struct socknode *sock, struct socknodeops *ops);
/*! @brief Raise an event with a socket. */
int sock_event_raise(struct socknode *sock, int events);

extern const char *sockstate_str[];
extern struct socknodeops packet_soops;
extern struct socknodeops raw_soops;
extern struct socknodeops tcp_soops;
extern struct socknodeops unix_soops;
extern struct socknodeops udp_soops;
extern struct vnops sock_vnops;

#endif /* KRX_KEYSOCK_SOCKFS_H */
