#include "lwip/tcpip.h"

static void
tcpip_running(void *)
{
	kprintf("net_init: Keyronex Sockets for Kernel, version 2\n");
}

void
net_init(void)
{
	tcpip_init(tcpip_running, NULL);
}
