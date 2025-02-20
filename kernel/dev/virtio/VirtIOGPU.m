#include <stddef.h>

#include "ddk/DKDevice.h"
#include "ddk/DKFramebuffer.h"
#include "ddk/DKUtilities.h"
#include "ddk/virtio_gpu.h"
#include "ddk/virtioreg.h"
#include "dev/FBTerminal.h"
#include "dev/virtio/VirtIOGPU.h"
#include "kdk/endian.h"
#include "kdk/kern.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vm.h"

struct virtio_gpu_req {
	TAILQ_ENTRY(virtio_gpu_req) queue_link;
	uint16_t first_desc_id;
	kevent_t completion;
};

@interface
VirtIOGPU (Private)
- (void)flush;
- (int)submitCommand:(void *)cmdVaddr
	      ofSize:(size_t)cmdSize
	withResponse:(void *)respVaddr
	      ofSize:(size_t)respSize
	     request:(struct virtio_gpu_req *)existingRequest;
@end

/*! pre-allocated framebuffer memory from bootloader */
void *fb_base = NULL;

static void
flush_timer_dpc_handler(void *arg)
{
	VirtIOGPU *self = arg;
	[self flush];
	ke_timer_set(&self->m_flushTimer, NS_PER_S / 32);
}

@implementation VirtIOGPU

- (instancetype)initWithTransport:(DKVirtIOTransport *)transport
{
	int r;
	vm_page_t *page;

	self = [super init];

	m_name = "virtio-gpu";
	m_transport = transport;
	TAILQ_INIT(&in_flight_reqs);

	[transport resetDevice];

	if (![m_transport exchangeFeaturesMandatory:VIRTIO_F_VERSION_1
					   optional:NULL]) {
		DKDevLog(self, "Featuure exchange failed\n");
		return nil;
	}

	r = [m_transport setupQueue:&m_commandQueue index:0];
	if (r != 0) {
		DKDevLog(self, "Failed to setup command queue: %d", r);
		return nil;
	}

	r = [m_transport setupQueue:&m_cursorQueue index:1];
	if (r != 0) {
		DKDevLog(self, "Failed to setup cursor queue: %d", r);
		return nil;
	}

	r = [m_transport enableDevice];
	if (r != 0) {
		DKDevLog(self, "Failed to enable device: %d\n", r);
	}

	r = vm_page_alloc(&page, vm_bytes_to_order(1024 * 768 * 4),
	    kPageUseKWired, false);
	if (r != 0)
		kfatal("Failed to allocate framebuffer memory: %d\n", r);

	fb_base = (void *)vm_page_paddr(page);

	m_info.address = (paddr_t)fb_base;
	m_info.pitch = 1024 * 4;
	m_info.width = 1024;
	m_info.height = 768;

	return self;
}

- (void)start
{
	/* display_info */
	struct virtio_gpu_ctrl_hdr *hdr = kmem_alloc(sizeof(*hdr));
	struct virtio_gpu_resp_display_info *resp = (void *)vm_kalloc(1, 0);
	hdr->type = to_leu32(VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
	hdr->flags = to_leu32(0);

	memset(resp, 0x12, sizeof(*resp));

	[self submitCommand:hdr
		     ofSize:sizeof(*hdr)
	       withResponse:resp
		     ofSize:sizeof(*resp)
		    request:NULL];

	kassert(from_leu32(resp->hdr.type) == VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
	for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
		if (!from_leu32(resp->pmodes[i].enabled))
			continue;
		DKDevLog(self, "Scanout %u: %ux%u (flags 0x%x))\n", i,
		    from_leu32(resp->pmodes[i].r.width),
		    from_leu32(resp->pmodes[i].r.height),
		    from_leu32(resp->pmodes[i].flags));
	}

	/* resource create */
	struct virtio_gpu_resource_create_2d *req_create = kmem_alloc(
	    sizeof(*req_create));
	struct virtio_gpu_ctrl_hdr *resp_create = kmem_alloc(
	    sizeof(*resp_create));
	req_create->hdr.type = to_leu32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
	req_create->hdr.flags = to_leu32(0);
	req_create->resource_id = to_leu32(1);
	req_create->format = to_leu32(VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM);
	req_create->width = to_leu32(1024);
	req_create->height = to_leu32(768);

	[self submitCommand:req_create
		     ofSize:sizeof(*req_create)
	       withResponse:resp_create
		     ofSize:sizeof(*resp_create)
		    request:NULL];
	kassert(from_leu32(resp_create->type) == VIRTIO_GPU_RESP_OK_NODATA);

	/* attach backing */
	struct virtio_gpu_resource_attach_backing *req_attach = kmem_alloc(
	    sizeof(*req_attach));
	struct virtio_gpu_ctrl_hdr *resp_attach = kmem_alloc(
	    sizeof(*resp_create));
	req_attach->hdr.type = to_leu32(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
	req_attach->hdr.flags = to_leu32(0);
	req_attach->resource_id = to_leu32(1);
	req_attach->nr_entries = to_leu32(1);
	req_attach->addr = to_leu64((uint64_t)(uintptr_t)m_info.address);
	req_attach->length = to_leu32(1024 * 768 * 4);
	req_attach->padding = to_leu32(0);

	[self submitCommand:req_attach
		     ofSize:sizeof(*req_attach)
	       withResponse:resp_attach
		     ofSize:sizeof(*resp_attach)
		    request:NULL];
	kassert(from_leu32(resp_attach->type) == VIRTIO_GPU_RESP_OK_NODATA);

	/* link scanout */
	struct virtio_gpu_set_scanout *req_set = kmem_alloc(sizeof(*req_set));
	struct virtio_gpu_ctrl_hdr *resp_set = kmem_alloc(sizeof(*resp_set));
	req_set->hdr.type = to_leu32(VIRTIO_GPU_CMD_SET_SCANOUT);
	req_set->hdr.flags = to_leu32(0);
	req_set->resource_id = to_leu32(1);
	req_set->scanout_id = to_leu32(0);
	req_set->r.x = to_leu32(0);
	req_set->r.y = to_leu32(0);
	req_set->r.width = to_leu32(1024);
	req_set->r.height = to_leu32(768);

	[self submitCommand:req_set
		     ofSize:sizeof(*req_set)
	       withResponse:resp_set
		     ofSize:sizeof(*resp_set)
		    request:NULL];
	kassert(from_leu32(resp_set->type) == VIRTIO_GPU_RESP_OK_NODATA);

	/* set up transfer and flush inflight reqs, requests, and responses */
	m_transferReq = kmem_alloc(sizeof(*m_transferReq));
	m_transferRequest = kmem_alloc(sizeof(*m_transferRequest));
	m_transferResponse = kmem_alloc(sizeof(*m_transferResponse));
	m_flushReq = kmem_alloc(sizeof(*m_flushReq));
	m_flushRequest = kmem_alloc(sizeof(*m_flushRequest));
	m_FlushResponse = kmem_alloc(sizeof(*m_FlushResponse));

	m_transferRequest->hdr.type = to_leu32(
	    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
	m_transferRequest->hdr.flags = to_leu32(0);
	m_transferRequest->resource_id = to_leu32(1);
	m_transferRequest->padding = to_leu32(0);
	m_transferRequest->offset = to_leu64(0);
	m_transferRequest->r.x = to_leu32(0);
	m_transferRequest->r.y = to_leu32(0);
	m_transferRequest->r.width = to_leu32(1024);
	m_transferRequest->r.height = to_leu32(768);

	m_flushRequest->hdr.type = to_leu32(VIRTIO_GPU_CMD_RESOURCE_FLUSH);
	m_flushRequest->hdr.flags = to_leu32(0);
	m_flushRequest->resource_id = to_leu32(1);
	m_flushRequest->padding = to_leu32(0);
	m_flushRequest->r.x = to_leu32(0);
	m_flushRequest->r.y = to_leu32(0);
	m_flushRequest->r.width = to_leu32(1024);
	m_flushRequest->r.height = to_leu32(768);

	ke_timer_init(&m_flushTimer);

	m_flushTimer.dpc = &m_flushDpc;
	m_flushDpc.callback = flush_timer_dpc_handler;
	m_flushDpc.arg = self;
	m_flushDpc.cpu = NULL;
	ke_timer_set(&m_flushTimer, NS_PER_S / 4);

	[[FBTerminal alloc] initWithFramebuffer:self];
}

- (int)submitCommand:(void *)cmdVaddr
	      ofSize:(size_t)cmdSize
	withResponse:(void *)respVaddr
	      ofSize:(size_t)respSize
	     request:(struct virtio_gpu_req *)existingRequest
{
	uint32_t descs[2];
	struct virtio_gpu_req *req;
	uint64_t cmdPaddr = (paddr_t)V2P(cmdVaddr),
		 respPaddr = (paddr_t)V2P(respVaddr);
	ipl_t ipl;

	if (existingRequest == NULL)
		req = kmem_alloc(sizeof(*req));
	else
		req = existingRequest;

	ipl = ke_spinlock_acquire(&m_commandQueue.spinlock);

	descs[0] = [m_transport allocateDescNumOnQueue:&m_commandQueue];
	descs[1] = [m_transport allocateDescNumOnQueue:&m_commandQueue];

	m_commandQueue.desc[descs[0]].len = to_leu32(cmdSize);
	m_commandQueue.desc[descs[0]].flags = to_leu16(VRING_DESC_F_NEXT);
	m_commandQueue.desc[descs[0]].next = to_leu16(descs[1]);
	m_commandQueue.desc[descs[0]].addr = to_leu64(cmdPaddr);

	m_commandQueue.desc[descs[1]].len = to_leu32(respSize);
	m_commandQueue.desc[descs[1]].flags = to_leu16(VRING_DESC_F_WRITE);
	m_commandQueue.desc[descs[1]].addr = to_leu64(respPaddr);

	req->first_desc_id = descs[0];
	ke_event_init(&req->completion, false);
	TAILQ_INSERT_HEAD(&in_flight_reqs, req, queue_link);

	[m_transport submitDescNum:descs[0] toQueue:&m_commandQueue];
	[m_transport notifyQueue:&m_commandQueue];

	ke_spinlock_release(&m_commandQueue.spinlock, ipl);

	if (existingRequest == NULL)
		ke_wait(&req->completion, "virtioev", false, false, -1);

	return 0;
}

- (void)processUsedDescriptor:(volatile struct vring_used_elem *)e
		      onQueue:(struct virtio_queue *)queue
{
	volatile struct vring_desc *din, *dout;
	struct virtio_gpu_req *req;

	TAILQ_FOREACH (req, &in_flight_reqs, queue_link) {
		if (req->first_desc_id == le32_to_native(e->id))
			break;
	}

	if (!req || req->first_desc_id != le32_to_native(e->id))
		kfatal("viogpu completion without a request:"
		       "\n\tdescriptor id: %u\n",
		    le32_to_native(e->id));

	TAILQ_REMOVE(&in_flight_reqs, req, queue_link);

	din = &QUEUE_DESC_AT(&m_commandQueue, le32_to_native(e->id));
	dout = &QUEUE_DESC_AT(&m_commandQueue, from_leu16(din->next));
	kassert(from_leu16(dout->flags) & VRING_DESC_F_WRITE);

	[m_transport freeDescNum:from_leu16(din->next) onQueue:&m_commandQueue];
	[m_transport freeDescNum:le32_to_native(e->id) onQueue:&m_commandQueue];

#if DEBUG_VIOGPU == 1
	DKDevLog(self, "done cmd yielding %zu bytes\n",
	    le32_to_native(dout->len));
#endif

	ke_event_signal(&req->completion);
}

void
processVirtQueue(struct virtio_queue *queue, id delegate)
{
	uint16_t i;

	for (i = queue->last_seen_used;
	     i != from_leu16(queue->used->idx) % queue->length;
	     i = (i + 1) % queue->length) {
		volatile struct vring_used_elem *e =
		    &queue->used->ring[i % queue->length];
		[delegate processUsedDescriptor:e onQueue:queue];
	}

	queue->last_seen_used = i;
}

- (void)additionalDeferredProcessingForQueue:(virtio_queue_t *)queue
{
	/* epsilon */
}

- (void)flush
{
	[self submitCommand:m_transferRequest
		     ofSize:sizeof(*m_transferRequest)
	       withResponse:m_transferResponse
		     ofSize:sizeof(*m_transferResponse)
		    request:m_transferReq];
	[self submitCommand:m_flushRequest
		     ofSize:sizeof(*m_flushRequest)
	       withResponse:m_FlushResponse
		     ofSize:sizeof(*m_FlushResponse)
		    request:m_flushReq];
	void fbterminal_printstats(void);
	fbterminal_printstats();
}

@end
