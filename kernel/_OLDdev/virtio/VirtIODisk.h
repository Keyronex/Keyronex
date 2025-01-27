#ifndef KRX_DEV_VIRTIODISK_H
#define KRX_DEV_VIRTIODISK_H

#include "ddk/DKDevice.h"
#include "ddk/DKVirtIOTransport.h"

@interface VirtIODisk : DKDevice <DKVirtIODelegate> {
    @public
	struct virtio_queue m_ioQueue;

	/*! I/O packets known to be valid but waiting tryStartPackets(). */
	TAILQ_HEAD(, iop) pending_packets;

	/*! Base of requests array. */
	struct vioblk_req *m_requests;
	/*! Request freelist. */
	TAILQ_HEAD(, vioblk_req) free_reqs;
	/*! Virtio requests currently running. */
	TAILQ_HEAD(, vioblk_req) in_flight_reqs;
}

@end

#endif /* KRX_DEV_VIRTIODISK_H */
