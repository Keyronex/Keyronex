#ifndef KRX_NET_NET_H
#define KRX_NET_NET_H

#include "dev/DKNIC.h"
#include "kdk/kern.h"

struct pbuf;
struct socknode;

#define LOCK_LWIP() ke_spinlock_acquire(&lwip_lock)
#define UNLOCK_LWIP(IPL) ke_spinlock_release(&lwip_lock, ipl)
#define LOCK_LWIP_NOSPL() ke_spinlock_acquire_nospl(&lwip_lock)
#define UNLOCK_LWIP_NOSPL() ke_spinlock_release_nospl(&lwip_lock)

void ksk_packet_in(struct pbuf_rx *pbuf);

extern kspinlock_t lwip_lock;

#endif /* KRX_NET_NET_H */
