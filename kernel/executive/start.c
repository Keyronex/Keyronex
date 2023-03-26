/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Feb 24 2023.
 */

#include <sys/socket.h>

#include <stdint.h>

#include "abi-bits/in.h"
#include "dev/virtio_net.h"
#include "ex_private.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "lwip/tcpip.h"

ethread_t init_thread;

/* acpipc/acpipc.cc */
void acpipc_autoconf(void *rsdp);

static void
tcpip_running(void *unused)
{
	kdprintf("KeySock version 1\n");
}

struct dhcphdr {
	uint8_t opcode;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	in_addr_t ciaddr;
	in_addr_t yiaddr;
	in_addr_t siaddr;
	in_addr_t giaddr;
	uint8_t chaddr[16];
	char sname[64];
	char file[128];
	uint32_t magic_cookie;
	uint8_t options[0];
};

void
test_sockets(void)
{
#if 0
	int r;
	vnode_t *sockvn;

	int sock_udp_bind(vnode_t * vn, const struct sockaddr *nam,
	    socklen_t addr_len);

	r = udp_create(AF_INET, 0, &sockvn);
	if (r != 0) {
		kfatal("udp_create failed: %d\n", r);
	}

	kdprintf("UDP CREATED\n\n");

	struct sockaddr_in sin;
	sin.sin_addr.s_addr = inet_addr("0.0.0.0");
	sin.sin_port = 67;
	sin.sin_family = AF_INET;

	r = sock_udp_bind(sockvn, (struct sockaddr *)&sin, sizeof(sin));
	if (r != 0) {
		kfatal("udp_bind failed: %d\n", r);
	}

	struct dhcphdr hdr = { 0 };
	hdr.opcode = 1;
	hdr.htype = 1;
	hdr.hlen = ETHER_ADDR_LEN;
	// 52:54:00:12:34:56
	hdr.chaddr[0] = 0x52;
	hdr.chaddr[1] = 0x54;
	hdr.chaddr[2] = 0x00;
	hdr.chaddr[3] = 0x12;
	hdr.chaddr[4] = 0x23;
	hdr.chaddr[5] = 0x56;
	strcpy(hdr.sname, "Keyronex");
	hdr.flags = lwip_htons(0x8000);
	hdr.magic_cookie = lwip_htonl(0x63825363);

	int sock_udp_sendto(vnode_t * vn, void *buf, size_t len,
	    const struct sockaddr *nam, socklen_t addr_len);

	sin.sin_addr.s_addr = inet_addr("255.255.255.255");
	sin.sin_port = 67;
	sin.sin_family = AF_INET;

	r = sock_udp_sendto(sockvn, &hdr, sizeof(hdr), (struct sockaddr *)&sin,
	    sizeof(sin));
	if (r != 0) {
		kfatal("udp_sendto failed: %d\n", r);
	}

	kdprintf("UDP_SENDTO RETURNED %d\n\n", r);
#endif
}

void
init_thread_start(void *rsdp)
{
	/*! first we maun setup the device tmpfs */
	// setup device tmpfs

	tcpip_init(tcpip_running, NULL);
	acpipc_autoconf(rsdp);
	test_sockets();

#if 0
	vnode_t *vn;
	int r = vfs_lookup(root_vnode, &vn, "/atestdir/hello.txt", 0, NULL);

	kdprintf("lookup of root vnode (%p) yielded result %d vnode %p\n",
	    root_vnode, r, vn);

	char * buf  = kmem_alloc(256);
	memset(buf, 0x0, 256);
	r = vn->ops->read(vn, buf, 255, 0);

	kdprintf("read of /atestdir/hello.txt yielded result %d:\n%s\n", r,
	    buf);
#endif

	int test_tcpserver(void);
	test_tcpserver();

	int psx_init(void);
	psx_init();

	/* become some sort of worker thread? */
	for (;;)
		asm("pause");
}

void
ex_init(void *arg)
{
	ps_create_system_thread(&init_thread, "executive initialisation",
	    init_thread_start, arg);
	ki_thread_start(&init_thread.kthread);
}