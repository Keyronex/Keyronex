#if 0
#include "dev/DKNIC.h"
#endif
#include "kdk/dev.h"
#include "kdk/kern.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "kdk/kmem.h"
#include "net/net.h"
#include "netif/ethernet.h"

#define RCV_BUF_SIZE 65536

kdpc_t lwip_timer_dpc;
ktimer_t lwip_timer;
kspinlock_t lwip_lock = KSPINLOCK_INITIALISER;

kdpc_t ingress_dpc;
kspinlock_t ingress_lock = KSPINLOCK_INITIALISER;
TAILQ_HEAD(, pbuf_rx) ingress_queue = TAILQ_HEAD_INITIALIZER(ingress_queue);

#if 0
static void
ingress_callback(void *)
{
	while (true) {
		struct pbuf_rx *pbuf;
		err_t err;

		ke_spinlock_acquire_nospl(&ingress_lock);
		pbuf = TAILQ_FIRST(&ingress_queue);
		if (pbuf == NULL) {
			ke_spinlock_release_nospl(&ingress_lock);
			return;
		}
		TAILQ_REMOVE(&ingress_queue, pbuf, queue_entry);
		ke_spinlock_release_nospl(&ingress_lock);

		ke_spinlock_acquire_nospl(&lwip_lock);
		err = ethernet_input(&pbuf->pbuf.pbuf, pbuf->netif);
		ke_spinlock_release_nospl(&lwip_lock);
		if (err != ERR_OK) {
			pbuf_free(&pbuf->pbuf.pbuf);
			kprintf("KeySock: packet dropped\n");
		}
	}
}
#endif

void
ksp_reset_timer(uint32_t abs)
{
	uintptr_t now = cpus[0]->nanos / NS_PER_MS;
	uintptr_t rel = abs - now;
	ke_timer_set(&lwip_timer, rel * NS_PER_MS);
}

static void
lwip_timer_expiry(void *)
{
	ke_spinlock_acquire_nospl(&lwip_lock);
	sys_check_timeouts();
	ke_spinlock_release_nospl(&lwip_lock);
	ke_timer_set(&lwip_timer, sys_timeouts_sleeptime() * NS_PER_MS);
}

void
net_init(void)
{
	ke_timer_init(&lwip_timer);
	lwip_timer.dpc = &lwip_timer_dpc;
	lwip_timer_dpc.cpu = NULL;
	lwip_timer_dpc.callback = lwip_timer_expiry;

#if 0 /* DK refactoring */
	ingress_dpc.callback = ingress_callback;
#endif

	lwip_init();

	kprintf("net_init: Keyronex Sockets for Kernel, version 2\n");
}

#if 0 /* DK refactoring */
void
ksk_packet_in(struct pbuf_rx *pbuf)
{
	kassert(splget() >= kIPLDPC);

	ke_spinlock_acquire_nospl(&ingress_lock);
	TAILQ_INSERT_TAIL(&ingress_queue, pbuf, queue_entry);
	ke_spinlock_release_nospl(&ingress_lock);
	ke_dpc_enqueue(&ingress_dpc);
}
#endif

typedef void (*tcpip_callback_fn)(void *ctx);

err_t
tcpip_try_callback(tcpip_callback_fn function, void *ctx)
{
	kprintf("tcpip_try_callback\n");
	return ERR_OK;
}
