#ifndef KRX_DEV_DKVIRTIOGPU_H
#define KRX_DEV_DKVIRTIOGPU_H

#include "ddk/DKDevice.h"
#include "ddk/DKFramebuffer.h"
#include "ddk/DKVirtIOMMIODevice.h"

@interface VirtIOGPU : DKFramebuffer <DKVirtIODelegate> {
    @public
	TAILQ_HEAD(, virtio_gpu_req) in_flight_reqs;
	struct virtio_queue m_commandQueue;
	struct virtio_queue m_cursorQueue;
	ktimer_t m_flushTimer;

	struct virtio_gpu_req *m_transferReq;
	struct virtio_gpu_transfer_to_host_2d *m_transferRequest;
	struct virtio_gpu_ctrl_hdr *m_transferResponse;
	struct virtio_gpu_req *m_flushReq;
	struct virtio_gpu_resource_flush *m_flushRequest;
	struct virtio_gpu_ctrl_hdr *m_FlushResponse;
}

+ (BOOL)probeWithProvider:(DKVirtIOMMIODevice *)provider;

- (instancetype)initWithProvider:(DKVirtIOMMIODevice *)provider;

@end

#endif /* KRX_DEV_DKVIRTIOGPU_H */
