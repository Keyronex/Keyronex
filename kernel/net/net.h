#ifndef KRX_NET_NET_H
#define KRX_NET_NET_H

#if 0
#include "dev/DKNIC.h"
#endif
#include "kdk/kern.h"

struct pbuf;
struct socknode;

#define LOCK_LWIP() ke_spinlock_acquire(&lwip_lock)
#define UNLOCK_LWIP(IPL) ke_spinlock_release(&lwip_lock, ipl)
#define LOCK_LWIP_NOSPL() ke_spinlock_acquire_nospl(&lwip_lock)
#define UNLOCK_LWIP_NOSPL() ke_spinlock_release_nospl(&lwip_lock)

#if 0
void ksk_packet_in(struct pbuf_rx *pbuf);
#endif

extern kspinlock_t lwip_lock;

#endif /* KRX_NET_NET_H */
