#ifndef KRX_NET_NET_H
#define KRX_NET_NET_H

#include "dev/DKNIC.h"
#include "kdk/nanokern.h"

struct pbuf;

#define LOCK_LWIP() ke_spinlock_acquire(&lwip_lock)
#define UNLOCK_LWIP(IPL) ke_spinlock_release(&lwip_lock, ipl)

void ksk_packet_in(struct pbuf_rx *pbuf);

extern kspinlock_t lwip_lock;

#endif /* KRX_NET_NET_H */
