/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Wed Jan 29 2025.
 */

#include <ddk/DKAxis.h>
#include <ddk/DKPCIDevice.h>
#include <ddk/DKUSBDevice.h>
#include <ddk/DKUSBHub.h>
#include <ddk/reg/usb.h>
#include <kdk/executive.h>
#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include <kdk/vm.h>

#include "vm/vmp.h"
#include "xhci_reg.h"

@class XHCIController;
@class XHCIRootHub;

#define XHCI_RING_SIZE 256
#define XHCI_EVENT_RING_SIZE 256
#define XHCI_CMD_RING_SIZE 256

TAILQ_HEAD(req_tailq, req);

struct ring {
	XHCIController *controller;

	kspinlock_t lock;

	/* for non-event rings only */
	struct ring *event_ring;
	struct device *port;

	uint8_t address;
	uint16_t max_packet_size;

	dk_endpoint_direction_t dir;

	enum ring_type {
		kRingTypeControl = 0,
		kRingTypeBulk = 2,
		kRingTypeInterrupt = 3,
		kRingTypeCommand = 4,
		kRingTypeEvent = 5,
	} type;

	volatile struct xhci_trb *trbs;
	paddr_t paddr;
	uint32_t size;
	uint32_t cycle_bit;
	uint32_t enqueue_idx;
	uint32_t dequeue_idx;

	/* For event rings, in-flight; for others, pending. */
	struct req_tailq cmds;
	/* For event rings, process completions; for others, start pendings. */
	kdpc_t dpc;
};

struct protocol {
	uint8_t major;
	uint8_t minor;
	uint8_t slot_type;
	uint8_t port_count;
	uint8_t port_offset;
	XHCIRootHub *hub;
};

struct device {
	/* 1-based port numbers here */
	uint8_t root_port;
	uint8_t parent_hub_port;
	uint8_t parent_hub_slot;
	uint32_t route_string;
	uint8_t tier;
	uint8_t slot;
	vm_page_t *context_page;
	volatile struct xhci_input_ctx *input_ctx;
	paddr_t input_ctx_phys;
	volatile struct xhci_device_ctx *device_ctx;
	paddr_t device_ctx_phys;
	struct ring *ep0_ring;
};

/*!
 * A pending or in-flight command.
 */
struct req {
	TAILQ_ENTRY(req) link;
	struct ring *ring;
	paddr_t trb_paddr;

	union {
		struct xhci_trb trb;
		struct {
			const void *packet;
			size_t length;
			void *out;
			size_t out_length;
		} ctrl;
	};

	struct {
		uint8_t code;
		uint8_t slot_id;
	} result;

	void (*callback)(DKUSBController *, struct req *, void *);
	void *context;
};

@interface XHCIController : DKUSBController <DKPCIDeviceMatching> {
	DKPCIDevice *m_pciDevice;

	vaddr_t m_mmioBase;
	volatile struct xhci_host_cap_regs *m_capRegs;
	volatile struct xhci_host_op_regs *m_opRegs;
	volatile struct xhci_host_rt_regs *m_rtRegs;
	volatile leu32_t *m_doorBells;

	struct ring *m_cmdRing;
	struct ring *m_eventRing;
	struct xhci_erst_entry *m_erstTable;
	struct xhci_dcbaa_entry *m_dcbaa;
	size_t m_maxSlots;
	paddr_t m_erstTablePhys;
	paddr_t m_dcbaaPhys;

	struct intr_entry m_intrEntry;

	struct protocol *m_protocols;
	size_t m_nProtocols;

	struct port *m_ports;

	kthread_t *m_controllerThread;

	/* Device/hub tree state. */
	TAILQ_TYPE_HEAD(, DKUSBHub) m_allHubs;
	kevent_t m_reenumerationEvent;
}

+ (void)load;

+ (uint8_t)probeWithMatchData:(DKPCIMatchData *)matchData;

@end

@interface XHCIRootHub : DKUSBHub {
	struct protocol *m_protocol;
}

- (instancetype)initWithController:(XHCIController *)controller
			  protocol:(struct protocol *)protocol;

@end

@interface XHCIController ()

- (struct ring *)createRingWithType:(enum ring_type)type
			       size:(size_t)numTrbs
			    address:(uint8_t)address
		      maxPacketSize:(uint16_t)maxPacketSize
			  eventRing:(struct ring *)eventRing;
- (void)dispatchPendingRequestsOnRing:(struct ring *)ring;
- (void)queueRequest:(struct req *)cmd toRing:(struct ring *)ring;
- (void)synchronousRequest:(struct req *)cmd toRing:(struct ring *)ring;
- (bool)ringHasSpace:(struct ring *)ring needed:(unsigned)needed;

- (void)ringCommandDoorbell;

@end

static void event_dpc(void *);
static void dispatch_dpc(void *);

static uint8_t
endpoint_index(uint8_t endpoint, dk_endpoint_direction_t dir)
{
	return (endpoint * 2) + (dir == kDKEndpointDirectionIn ? 1 : 0);
}

@implementation XHCIController

+ (void)load
{
	[DKPCIDevice registerMatchingClass:self];
}

+ (uint8_t)probeWithMatchData:(DKPCIMatchData *)matchData
{
	if (matchData->class == 0xc && matchData->subclass == 0x3 &&
	    matchData->prog_if == 0x30) {
		return 127;
	}

	return 0;
}

- (instancetype)initWithPCIDevice:(DKPCIDevice *)pciDevice
{
	if ((self = [super init])) {
		m_pciDevice = pciDevice;
		m_name = strdup("xHCIController");
	}

	return self;
}

- (struct ring *)createRingWithType:(enum ring_type)type
			       size:(size_t)numTrbs
			    address:(uint8_t)address
		      maxPacketSize:(uint16_t)maxPacketSize
			  eventRing:(struct ring *)eventRing
{
	struct ring *ring = kmem_alloc(sizeof(*ring));
	volatile struct xhci_trb *linkTRB;
	vaddr_t vaddr;
	paddr_t paddr;

	if (ring == NULL)
		return NULL;

	if (type != kRingTypeEvent && eventRing == NULL)
		kfatal("Non-event rings must have an associated event ring\n");

	memset(ring, 0, sizeof(*ring));

	size_t ringBytes = numTrbs * sizeof(struct xhci_trb);

	if (dk_allocate_and_map(&vaddr, &paddr, ringBytes) != 0) {
		kmem_free(ring, sizeof(*ring));
		return NULL;
	}

	ring->controller = self;
	ke_spinlock_init(&ring->lock);
	ring->address = address;
	ring->max_packet_size = maxPacketSize;
	ring->type = type;
	ring->event_ring = eventRing;
	ring->trbs = (volatile struct xhci_trb *)vaddr;
	ring->size = (uint32_t)numTrbs;
	ring->cycle_bit = 1;
	ring->enqueue_idx = 0;
	ring->dequeue_idx = 0;
	ring->paddr = paddr;
	TAILQ_INIT(&ring->cmds);

	if (ring->type == kRingTypeEvent)
		ke_dpc_init(&ring->dpc, event_dpc, ring);
	else
		ke_dpc_init(&ring->dpc, dispatch_dpc, ring);

	memset((void *)ring->trbs, 0, ringBytes);

	if (ring->type != kRingTypeEvent) {
		linkTRB = &ring->trbs[numTrbs - 1];
		linkTRB->params = to_leu64(ring->paddr);
		linkTRB->status = to_leu32(0);
		linkTRB->control = to_leu32(
		    (TRB_TYPE_LINK << 10) | (1 << 1) | ring->cycle_bit);
	}

	return ring;
}

- (int)setup
{
	int r;
	size_t dcbaaSize;
	uint32_t maxScratchpadBuffers;
	uint64_t erdp;
	volatile struct xhci_interrupt_regs *ir;

	m_maxSlots = from_leu32(m_capRegs->HCSPARAMS1) & 0xFF;
	if (!m_maxSlots) {
		kprintf("XHCI: Invalid maxSlots = 0\n");
		return -1;
	}

	dcbaaSize = (m_maxSlots + 1) * sizeof(struct xhci_dcbaa_entry);

	r = dk_allocate_and_map((vaddr_t *)&m_dcbaa, &m_dcbaaPhys, dcbaaSize);
	if (r != 0) {
		kprintf("XHCI: Failed to allocate DCBAA\n");
		return -1;
	}

	memset(m_dcbaa, 0, dcbaaSize);

	maxScratchpadBuffers = XHCI_HCS2_MAX_SCRATCHPAD(from_leu32(m_capRegs->HCSPARAMS2));
	if (maxScratchpadBuffers > 0) {
		vaddr_t scratchBufsVaddr;
		paddr_t scratchBufsPaddr;
		leu64_t *scratchBufs;

		r = dk_allocate_and_map(&scratchBufsVaddr, &scratchBufsPaddr,
		    sizeof(leu64_t) * maxScratchpadBuffers);
		if (r != 0) {
			kprintf("XHCI: Failed to allocate scratchpad "
				"buffers\n");
			return -1;
		}

		scratchBufs = (leu64_t *)scratchBufsVaddr;

		for (uint32_t i = 0; i < maxScratchpadBuffers; i++) {
			vm_page_t *page;

			r = vm_page_alloc(&page, 0, kPageUseKWired, false);
			if (r != 0) {
				kprintf("XHCI: Failed to allocate scratchpad "
					"buffer page\n");
				return -1;
			}

			scratchBufs[i] = to_leu64(vm_page_paddr(page));
		}

		m_dcbaa[0].dev_context_ptr = to_leu64(scratchBufsPaddr);
	}

	m_eventRing = [self createRingWithType:kRingTypeEvent
					  size:XHCI_EVENT_RING_SIZE
				       address:0
				 maxPacketSize:8
				     eventRing:NULL];
	if (m_eventRing == NULL)
		return -1;

	m_cmdRing = [self createRingWithType:kRingTypeCommand
					size:XHCI_CMD_RING_SIZE
				     address:0
			       maxPacketSize:8
				   eventRing:m_eventRing];
	if (m_cmdRing == NULL)
		return -1;

	r = dk_allocate_and_map((vaddr_t *)&m_erstTable, &m_erstTablePhys,
	    sizeof(struct xhci_erst_entry));
	if (r != 0) {
		kprintf("XHCI: Failed to allocate ERST\n");
		return -1;
	}

	memset(m_erstTable, 0, sizeof(struct xhci_erst_entry));

	m_erstTable[0].ring_segment_base = to_leu64(m_eventRing->paddr);
	m_erstTable[0].ring_segment_size = to_leu32(XHCI_EVENT_RING_SIZE);

	m_opRegs->DCBAAP = to_leu64(m_dcbaaPhys);

	m_opRegs->CRCR = to_leu64(m_cmdRing->paddr |
	    (m_cmdRing->cycle_bit & 1));

	ir = &m_rtRegs->IR[0];
	ir->ERSTSZ = to_leu32(1);
	ir->ERSTBA = to_leu64(m_erstTablePhys);

	erdp = m_eventRing->paddr;
	ir->ERDP_lo = to_leu32((uint32_t)erdp);
	ir->ERDP_hi = to_leu32((uint32_t)(erdp >> 32));

	ir->IMAN = to_leu32(XHCI_IMAN_IE | XHCI_IMAN_IP);

	return 0;
}

- (void)ringCommandDoorbell
{
	m_doorBells[0] = to_leu32(0);
}

static inline paddr_t
enqueue_trb(struct ring *ring, uint64_t params, uint32_t status,
    uint32_t control)
{
	volatile struct xhci_trb *trb = &ring->trbs[ring->enqueue_idx];
	paddr_t trb_paddr;

	memset((void *)trb, 0, sizeof(*trb));
	trb->params = to_leu64(params);
	trb->status = to_leu32(status);
	trb->control = to_leu32(control | (ring->cycle_bit & 1));

	trb_paddr = ring->paddr + (ring->enqueue_idx * sizeof(struct xhci_trb));

	ring->enqueue_idx++;
	if (ring->enqueue_idx == ring->size - 1) {
		/* skip the link TRB */
		ring->enqueue_idx = 0;
		ring->cycle_bit ^= 1;
	}

	return trb_paddr;
}

- (void)enqueueOneCommandToRing:(struct req *)cmd
{
	struct ring *ring = cmd->ring;

	if (ring->type == kRingTypeCommand) {
		cmd->trb_paddr = enqueue_trb(ring, from_leu64(cmd->trb.params),
		    0, from_leu32(cmd->trb.control));
		[self ringCommandDoorbell];
		return;
	} else if (ring->type == kRingTypeControl) {
		struct device *dev = cmd->ring->port;

		enqueue_trb(ring, *((uint64_t *)cmd->ctrl.packet), 8,
		    (TRB_TYPE_SETUP_STAGE << 10) | TRB_FLAGS_IDT);

		uint32_t dataControl = (TRB_TYPE_DATA_STAGE << 10);
		if (cmd->ctrl.length > 0) {
			dk_usb_setup_packet_t *setupPacket =
			    (dk_usb_setup_packet_t *)cmd->ctrl.packet;

			if (setupPacket->bmRequestType & 0x80)
				/* IN transfer */
				dataControl |= (1 << 16);

			(void)enqueue_trb(ring, (uint64_t)V2P(cmd->ctrl.out),
			    (uint32_t)cmd->ctrl.out_length, dataControl);
		} else {
			/* dummy data TRB */
			(void)enqueue_trb(ring, 0, 0, dataControl);
		}

		cmd->trb_paddr = enqueue_trb(ring, 0, 0,
		    (TRB_TYPE_STATUS_STAGE << 10) | TRB_FLAGS_IOC);

		m_doorBells[dev->slot] = to_leu32(0x00000001);
	} else if (ring->type == kRingTypeInterrupt) {
		struct device *dev = cmd->ring->port;

		cmd->trb_paddr = enqueue_trb(ring, from_leu64(cmd->trb.params),
		    from_leu32(cmd->trb.status), from_leu32(cmd->trb.control));
		m_doorBells[dev->slot] = to_leu32(cmd->ring->address);
	} else {
		kfatal("Implement me!\n");
	}
}

- (void)dispatchPendingRequestsOnRing:(struct ring *)ring
{
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&ring->event_ring->lock);
	ke_spinlock_acquire_nospl(&ring->lock);

	while (true) {
		struct req *cmd = TAILQ_FIRST(&ring->cmds);
		size_t needed = 0;

		if (cmd == NULL)
			break;

		switch (ring->type) {
		case kRingTypeCommand:
			needed = 1;
			break;

		case kRingTypeInterrupt:
			needed = 1;
			break;

		case kRingTypeControl:
			needed = 3;
			break;

		default:
			kfatal("Other rings do not dispatch requests\n");
		}

		if (![self ringHasSpace:ring needed:needed])
			break;

		TAILQ_REMOVE(&ring->cmds, cmd, link);

		TAILQ_INSERT_TAIL(&ring->event_ring->cmds, cmd, link);
		[self enqueueOneCommandToRing:cmd];
	}

	ke_spinlock_release_nospl(&ring->lock);
	ke_spinlock_release(&ring->event_ring->lock, ipl);
}

static void
dispatch_dpc(void *arg)
{
	struct ring *ring = (struct ring *)arg;
	[ring->controller dispatchPendingRequestsOnRing:ring];
}

- (bool)ringHasSpace:(struct ring *)ring needed:(unsigned)needed
{
	unsigned used;
	unsigned usable = ring->size - 1; /* -1 for the link TRB. */

	if (ring->enqueue_idx >= ring->dequeue_idx)
		used = ring->enqueue_idx - ring->dequeue_idx;
	else
		used = usable - (ring->dequeue_idx - ring->enqueue_idx);

	unsigned free = usable - used;

	return (free >= needed);
}

- (int)requestDevice:(dk_usb_device_t)handle
	      packet:(const void *)packet
	      length:(size_t)length
		 out:(void *)dataOut
	   outLength:(size_t)outLength
{
	struct device *port = (struct device *)handle;
	struct ring *ep0_ring = port->ep0_ring;
	struct req *req = kmem_alloc(sizeof(*req));

	req->ctrl.packet = packet;
	req->ctrl.length = length;
	req->ctrl.out = dataOut;
	req->ctrl.out_length = outLength;
	req->ring = ep0_ring;
	req->callback = NULL;

	[self synchronousRequest:req toRing:ep0_ring];

	return req->result.code == 1 ? 0 : -1;
}

- (void)completeTRBOnEventRing:(struct ring *)eventRing
		       atPAddr:(paddr_t)eventTrbPaddr
		      withCode:(uint32_t)code
			slotID:(uint32_t)slotID
{
	struct ring *origRing;
	ipl_t ipl = ke_spinlock_acquire(&eventRing->lock);
	struct req *cmd = NULL;

	TAILQ_FOREACH (cmd, &eventRing->cmds, link) {
		if (cmd->trb_paddr == eventTrbPaddr)
			break;
	}

	if (cmd == NULL) {
		ke_spinlock_release(&eventRing->lock, ipl);
		kfatal("XHCI: Transfer event for unknown"
		       "command (TRB paddr: 0x%zx)\n",
		    eventTrbPaddr);
	}

	TAILQ_REMOVE(&eventRing->cmds, cmd, link);
	ke_spinlock_release_nospl(&eventRing->lock);

	origRing = cmd->ring;

	ke_spinlock_acquire_nospl(&origRing->lock);
	if (origRing->type == kRingTypeControl) {
		origRing->dequeue_idx += 3;
	} else {
		kassert(origRing->type == kRingTypeBulk ||
		    origRing->type == kRingTypeInterrupt ||
		    origRing->type == kRingTypeCommand);
		origRing->dequeue_idx += 1;
	}

	if (origRing->dequeue_idx >= origRing->size - 1) {
		volatile struct xhci_trb *linkTRB;

		origRing->dequeue_idx %= (origRing->size - 1);

		linkTRB = &origRing->trbs[origRing->size - 1];
		linkTRB->control = to_leu32((TRB_TYPE_LINK << 10) | (1 << 1) |
		    (origRing->cycle_bit ^ 1));
	}

	ke_spinlock_release(&origRing->lock, ipl);

	cmd->result.code = code;
	cmd->result.slot_id = slotID;

	cmd->callback(self, cmd, cmd->context);

	ke_dpc_enqueue(&origRing->dpc);
}

- (void)deferredProcessing
{
	volatile struct xhci_interrupt_regs *ir = &m_rtRegs->IR[0];
	struct ring *eventRing = m_eventRing;

	for (;;) {
		volatile struct xhci_trb *trb =
		    &eventRing->trbs[eventRing->dequeue_idx];
		uint32_t control = from_leu32(trb->control);
		uint32_t trbCycle = control & 0x1; /* bit 0 = cycle bit. */
		uint32_t trbType = (control >> 10) & 0x3F;
		uint64_t erdp;

		if (trbCycle != eventRing->cycle_bit)
			break;

		switch (trbType) {
		case TRB_TYPE_EVENT_TRANSFER: {
			uint32_t code = (from_leu32(trb->status) >> 24) & 0xFF;
			uint32_t slotId = (from_leu32(trb->control) >> 24) &
			    0xFF;
			paddr_t paddr = from_leu64(trb->params);

			[self completeTRBOnEventRing:eventRing
					     atPAddr:paddr
					    withCode:code
					      slotID:slotId];

			break;
		}

		case TRB_TYPE_EVENT_PORT_STATUS: {
			uint64_t params = from_leu64(trb->params);
			uint8_t portNum = (params >> 24) & 0xff;

			for (size_t i = 0; i < m_nProtocols; i++) {
				struct protocol *proto = &m_protocols[i];

				if (portNum > proto->port_offset &&
				    portNum <= proto->port_offset +
					    proto->port_count) {
					[proto->hub requestReenumeration];
				}
			}
			break;
		}

		case TRB_TYPE_EVENT_CMD_COMPLETE: {
			uint32_t code = (from_leu32(trb->status) >> 24) & 0xFF;
			uint32_t slotId = (from_leu32(trb->control) >> 24) &
			    0xFF;
			paddr_t paddr = from_leu64(trb->params);

			[self completeTRBOnEventRing:eventRing
					     atPAddr:paddr
					    withCode:code
					      slotID:slotId];
			break;
		}

		default:
			kprintf("XHCI: Unhandled event TRB type %u\n", trbType);
			break;
		}

		eventRing->dequeue_idx++;
		if (eventRing->dequeue_idx == eventRing->size) {
			eventRing->dequeue_idx = 0;
			eventRing->cycle_bit ^= 1;
		}

		erdp = (uint64_t)eventRing->paddr +
		    (eventRing->dequeue_idx * sizeof(struct xhci_trb));
		ir->ERDP_lo = to_leu32((uint32_t)erdp | (1 << 3));
		ir->ERDP_hi = to_leu32((uint32_t)(erdp >> 32));
	}
}

static void
event_dpc(void *arg)
{
	struct ring *ring = arg;
	[ring->controller deferredProcessing];
}

static bool
xhci_interrupt(md_intr_frame_t *frame, void *arg)
{
	XHCIController *self = arg;
#if 0 /* ? inapplicable with MSI-X */
	volatile struct xhci_interrupt_regs *ir = &self->m_rtRegs->IR[0];
	uint32_t status;

	status = from_leu32(ir->IMAN);
	if (!(status & XHCI_IMAN_IP)) {
		kprintf("IMAN not set\n");
		return false;
	}

	ir->IMAN = to_leu32(status);
#endif

	ke_dpc_enqueue(&self->m_eventRing->dpc);

	return true;
}

- (void)processCapabilities
{
	uint16_t off = (from_leu32(m_capRegs->HCCPARAMS1) >> 16) << 2;

	if (off == 0) {
		kprintf("XHCI: Extended capabilities pointer is 0\n");
		return;
	}

	while (true) {
		volatile leu32_t *capReg = (volatile leu32_t *)(m_mmioBase +
		    off);
		uint32_t cap = from_leu32(*capReg);
		uint8_t capId;
		uint8_t capNext;

		capId = cap & 0xFF;
		capNext = (cap >> 8) & 0xFF;

		if (capId == 0)
			break;

		if (capId == 2) {
			uint8_t major = (cap >> 24) & 0xFF;
			uint8_t minor = (cap >> 16) & 0xFF;
			uint32_t portInfo = from_leu32(capReg[2]);
			uint8_t portOffset = portInfo & 0xFF;
			uint8_t portCount = (portInfo >> 8) & 0xFF;
			uint32_t slotInfo = from_leu32(capReg[3]);
			uint8_t slotType = slotInfo & 0xF;

			m_protocols = kmem_realloc(m_protocols,
			    sizeof(struct protocol) * m_nProtocols,
			    sizeof(struct protocol) * (m_nProtocols + 1));

			m_protocols[m_nProtocols].major = major;
			m_protocols[m_nProtocols].minor = minor;
			m_protocols[m_nProtocols].port_offset = portOffset - 1;
			m_protocols[m_nProtocols].port_count = portCount;
			m_protocols[m_nProtocols].slot_type = slotType;

			kprintf(
			    "XHCI: Protocol %u.%u, %u ports starting at %u, slot type %u\n",
			    major, minor, portCount, portOffset, slotType);

			m_nProtocols++;
		}

		if (capNext == 0)
			break;

		off += capNext << 2;
	}
}

- (void)queueRequest:(struct req *)cmd toRing:(struct ring *)ring
{
	ipl_t ipl = ke_spinlock_acquire(&ring->lock);
	cmd->ring = ring;
	TAILQ_INSERT_TAIL(&ring->cmds, cmd, link);
	ke_spinlock_release(&ring->lock, ipl);

	ke_dpc_enqueue(&ring->dpc);
}

- (void)controllerThread
{
	while (true) {
		DKUSBHub *hub, *save;

		ke_wait(&m_reenumerationEvent, "xhci controller thread wait", 0,
		    0, -1);
		ke_event_clear(&m_reenumerationEvent);

		TAILQ_FOREACH_SAFE (hub, &m_allHubs, m_controllerHubsLink,
		    save) {
			[hub enumerate];
		}
	}
}

static void
controller_thread(void *arg)
{
	XHCIController *self = (XHCIController *)arg;
	[self controllerThread];
	ps_exit_this_thread();
}

- (void)start
{
	DKPCIBarInfo barInfo = [m_pciDevice barInfo:0];
	int r;

	[m_pciDevice setBusMastering:true];
	[m_pciDevice setMemorySpace:true];
	[m_pciDevice setInterrupts:false];

	r = vm_ps_map_physical_view(&kernel_procstate, &m_mmioBase,
	    PGROUNDUP(barInfo.size), barInfo.base, kVMRead | kVMWrite,
	    kVMRead | kVMWrite, false);
	kassert(r == 0);

	m_capRegs = (volatile struct xhci_host_cap_regs *)m_mmioBase;
	m_opRegs = (volatile struct xhci_host_op_regs *)(m_mmioBase +
	    m_capRegs->CAPLENGTH);
	m_rtRegs = (volatile struct xhci_host_rt_regs *)(m_mmioBase +
	    from_leu32(m_capRegs->RTSOFF));
	m_doorBells = (volatile leu32_t *)(m_mmioBase +
	    from_leu32(m_capRegs->DBOFF));

	[self processCapabilities];

	uint32_t cmd = from_leu32(m_opRegs->USBCMD);
	if (cmd & XHCI_CMD_RUN) {
		cmd &= ~XHCI_CMD_RUN;
		m_opRegs->USBCMD = to_leu32(cmd);
		while (!(from_leu32(m_opRegs->USBSTS) & XHCI_STS_HCH))
			asm("pause");
	}

	m_opRegs->USBCMD = to_leu32(XHCI_CMD_HCRST);
	while (from_leu32(m_opRegs->USBSTS) & XHCI_STS_CNR)
		asm("pause");

	r = [self setup];
	if (r != 0) {
		kprintf("XHCI: setup failed\n");
		return;
	}

	m_intrEntry.handler = xhci_interrupt;
	m_intrEntry.arg = self;

	r = [m_pciDevice setMSIx:true];
	if (r != 0) {
		kprintf("XHCI: failed to enable MSI-X\n");
		return;
	}

	r = [m_pciDevice allocateLeastLoadedMSIxInterruptForEntry:&m_intrEntry];
	if (r != 0) {
		kprintf("XHCI: failed to allocate MSI-X interrupt\n");
		return;
	}

	m_opRegs->CONFIG = to_leu32((uint32_t)m_maxSlots);

	cmd = from_leu32(m_opRegs->USBCMD);
	cmd |= (XHCI_CMD_RUN | XHCI_CMD_INTE);
	m_opRegs->USBCMD = to_leu32(cmd);

	while (from_leu32(m_opRegs->USBSTS) & XHCI_STS_HCH)
		asm("pause");

	TAILQ_INIT(&m_allHubs);
	ke_event_init(&m_reenumerationEvent, false);

	for (size_t i = 0; i < m_nProtocols; i++) {
		XHCIRootHub *hub = [[XHCIRootHub alloc]
		    initWithController:self
			      protocol:&m_protocols[i]];

		m_protocols[i].hub = hub;

		[self attachChild:hub onAxis:gDeviceAxis];
		[hub start];
	}

	ps_create_kernel_thread(&m_controllerThread, "xhci controller thread",
	    controller_thread, self);
	ke_thread_resume(m_controllerThread);
}

static void
synch_callback(DKUSBController *controller, struct req *cmd, void *context)
{
	ke_event_signal((kevent_t *)context);
}

- (void)synchronousRequest:(struct req *)cmd toRing:(struct ring *)ring
{
	kevent_t event;
	ke_event_init(&event, false);
	cmd->callback = synch_callback;
	cmd->context = &event;
	[self queueRequest:cmd toRing:ring];
	ke_wait(&event, "xhci synchronous request", 0, 0, -1);
}

- (int)setupEndpointForDevice:(dk_usb_device_t)devHandle
		     endpoint:(uint8_t)endpoint
			 type:(dk_endpoint_type_t)type
		    direction:(dk_endpoint_direction_t)dir
		maxPacketSize:(uint16_t)maxPacket
		     interval:(uint8_t)interval
	       endpointHandle:(out dk_usb_endpoint_t *)endpointHandle
{
	struct device *dev = (struct device *)devHandle;
	struct ring *ring;
	enum ring_type ring_type;
	uint8_t epNum = endpoint_index(endpoint, dir);
	volatile struct xhci_ep_ctx *epCtx;
	struct req cmd;

	switch (type) {
	case kDKEndpointTypeBulk:
		ring_type = kRingTypeBulk;
		break;

	case kDKEndpointTypeInterrupt:
		ring_type = kRingTypeInterrupt;
		break;

	default:
		kprintf("Unsupported endpoint type %u\n", type);
		return -1;
	}

	ring = [self createRingWithType:ring_type
				   size:XHCI_RING_SIZE
				address:epNum
			  maxPacketSize:maxPacket
			      eventRing:m_eventRing];

	ring->port = dev;
	ring->dir = dir;
	ring->address = epNum;

	if (dir == kDKEndpointDirectionIn)
		ring_type += 4;

	epCtx = &dev->input_ctx->ep[epNum - 1];

	epCtx->field1 = to_leu32(interval << 16);
	epCtx->field2 = to_leu32((3 << 1) | ((uint32_t)ring_type << 3));
	epCtx->dequeue_ptr = to_leu64(ring->paddr | ring->cycle_bit);
	epCtx->tx_info = to_leu32(maxPacket << 16);

	uint32_t addFlag = 1 << epNum;
	dev->input_ctx->ctrl.add_flags = to_leu32(1 | addFlag);

	memset(&cmd, 0, sizeof(cmd));
	cmd.trb.params = to_leu64(dev->input_ctx_phys);
	cmd.trb.status = to_leu32(0);

	cmd.trb.control = to_leu32((TRB_TYPE_CMD_CONFIG_EP << 10) |
	    (dev->slot << 24));
	[self synchronousRequest:&cmd toRing:m_cmdRing];
	if (cmd.result.code != 1) {
		kprintf("Configure Endpoint command failed (code %u)\n",
		    cmd.result.code);
		return -1;
	}

	*endpointHandle = (dk_usb_endpoint_t)ring;

	return 0;
}

- (int)allocateTransfer:(out dk_usb_transfer_t *)transfer
{
	struct req *req = kmem_alloc(sizeof(*req));
	*transfer = req;
	return 0;
}

- (void)submitTransfer:(dk_usb_transfer_t)transfer
	      endpoint:(dk_usb_endpoint_t)endpoint
		buffer:(paddr_t)buffer
		length:(size_t)length
	      callback:(void (*)(DKUSBController *, dk_usb_transfer_t,
			   void *))callback
		 state:(void *)state
{
	struct req *req = transfer;

	req->ring = endpoint;

	req->trb.params = to_leu64(buffer);
	req->trb.status = to_leu32(length);
	req->trb.control = to_leu32((TRB_TYPE_NORMAL << 10) | TRB_FLAGS_IOC);

	req->callback = (void (*)(DKUSBController *, struct req *,
	    void *))callback;
	req->context = state;

	[self queueRequest:req toRing:endpoint];
}

- (void)addChildHub:(DKUSBHub *)hub
{
	TAILQ_INSERT_TAIL(&m_allHubs, hub, m_controllerHubsLink);
}

- (int)getRootHubPort:(size_t)port status:(usb_port_status_and_change_t *)stat
{
	volatile struct xhci_port_regs *pregs = &m_opRegs->ports[port];
	uint32_t ps = from_leu32(pregs->PORTSC);

	stat->status = 0;
	stat->change = 0;

	switch (XHCI_PORTSC_SPEED(ps)) {
	case 0:
		/* super speed */
		break;

	case 1:
		stat->status |= PORT_STATUS_FULL_SPEED;
		break;

	case 2:
		stat->status |= PORT_STATUS_LOW_SPEED;
		break;

	case 3:
		stat->status |= PORT_STATUS_HIGH_SPEED;
		break;

	default:
		stat->status |= PORT_STATUS_OTHER_SPEED;
		break;
	}

	if (ps & XHCI_PORTSC_CCS)
		stat->status |= PORT_STATUS_CURRENT_CONNECT;
	if (ps & XHCI_PORTSC_PED)
		stat->status |= PORT_STATUS_ENABLE_DISABLE;
	if (ps & XHCI_PORTSC_PR)
		stat->status |= PORT_STATUS_RESET;
	if (ps & XHCI_PORTSC_OCA)
		stat->status |= PORT_STATUS_OVER_CURRENT;
	if (ps & stat->status) {
		if (stat->status & PORT_STATUS_OTHER_SPEED)
			stat->status |= PORT_STATUS_PORT_POWER_SS;
		else
			stat->status |= PORT_STATUS_POWER;
	}

	if (ps & XHCI_PORTSC_CSC)
		stat->change |= PORT_CHANGE_CONNECT_STATUS;
	if (ps & XHCI_PORTSC_PEC)
		stat->change |= PORT_CHANGE_ENABLE_DISABLE;
	if (ps & XHCI_PORTSC_PRC)
		stat->change |= PORT_CHANGE_RESET;
	if (ps & XHCI_PORTSC_WRC)
		stat->change |= PORT_CHANGE_RESET_BH;
	if (ps & XHCI_PORTSC_PLC)
		stat->change |= PORT_CHANGE_LINK_STATE;

	return 0;
}

- (int)setRootHubPortFeature:(size_t)port feature:(uint16_t)feature
{
	volatile struct xhci_port_regs *pregs = &m_opRegs->ports[port];
	uint32_t ps = from_leu32(pregs->PORTSC);

	ps &= ~XHCI_PORTSC_CMD_BITS_CLEAR;

	switch (feature) {
	case PORT_ENABLE:
		ps |= XHCI_PORTSC_PED;
		break;

	case PORT_RESET:
		ps |= XHCI_PORTSC_PR;
		break;

	case PORT_POWER:
		ps |= XHCI_PORTSC_PP;
		break;

	default:
		return -1;
	}

	pregs->PORTSC = to_leu32(ps);

	return 0;
}

- (int)clearPortFeature:(size_t)port feature:(uint16_t)feature
{
	volatile struct xhci_port_regs *pregs = &m_opRegs->ports[port];
	uint32_t ps = from_leu32(pregs->PORTSC);

	ps &= ~XHCI_PORTSC_CMD_BITS_CLEAR;

	switch (feature) {
	case PORT_ENABLE:
		ps &= ~XHCI_PORTSC_PED;
		break;

	case PORT_POWER:
		ps &= ~XHCI_PORTSC_PP;
		break;

	case C_PORT_CONNECTION:
		ps |= XHCI_PORTSC_CSC;
		break;

	case C_PORT_ENABLE:
		ps |= XHCI_PORTSC_PEC;
		break;

	case C_PORT_RESET:
		ps |= XHCI_PORTSC_PRC;
		break;

	default:
		return -1;
	}

	pregs->PORTSC = to_leu32(ps);

	return 0;
}

- (void)requestReenumeration
{
	ke_event_signal(&m_reenumerationEvent);
}

- (int)enableSlotForDevice:(struct device *)dev
{
	struct req cmd;
	uint8_t slot_type = -1;

	for (size_t i = 0; i < m_nProtocols; i++)
		if (m_protocols[i].port_offset <= dev->root_port &&
		    dev->root_port <
			m_protocols[i].port_offset + m_protocols[i].port_count)
			slot_type = m_protocols[i].slot_type;

	kassert(slot_type != (uint8_t)-1);

	cmd.trb.params = to_leu64(0);
	cmd.trb.status = to_leu32(0);
	cmd.trb.control = to_leu32((TRB_TYPE_CMD_ENABLE_SLOT << 10) |
	    ((uint32_t)slot_type << 16));

	[self synchronousRequest:&cmd toRing:m_cmdRing];
	if (cmd.result.code != 1)
		kfatal("Enable slot failed\n");

	dev->slot = cmd.result.slot_id;

	return 0;
}

- (int)addressDevice:(struct device *)dev
{
	volatile struct xhci_input_ctx *input_ctx = dev->input_ctx;
	volatile struct xhci_slot_ctx *slot_ctx = &input_ctx->slot;
	volatile struct xhci_ep_ctx *ep0_ctx = &input_ctx->ep[0];
	struct ring *ep0_ring = dev->ep0_ring;
	struct req cmd;

	input_ctx->ctrl.drop_flags = to_leu32(0);
	input_ctx->ctrl.add_flags = to_leu32(0x3); /* Slot and EP0 contexts */

	slot_ctx->field1 = to_leu32(
	    SLOT_CTX_00_SET_ROUTE_STRING(dev->route_string) |
	    SLOT_CTX_00_SET_CONTEXT_ENTRIES(1));
	slot_ctx->field2 = to_leu32(
	    SLOT_CTX_04_SET_ROOT_HUB_PORT(dev->root_port + 1));
	slot_ctx->field3 = to_leu32(0);
	slot_ctx->field4 = to_leu32(0);

	ep0_ctx->field1 = to_leu32(0);
	ep0_ctx->field2 = to_leu32((3 << 1) | (4 << 3)); /* CErr 3, EPType ctrl */
	ep0_ctx->dequeue_ptr = to_leu64(ep0_ring->paddr | ep0_ring->cycle_bit);
	ep0_ctx->tx_info = to_leu32(8 << 16); /* average TRB Length = 8 */

	cmd.callback = NULL;

	cmd.trb.params = to_leu64(dev->input_ctx_phys);
	cmd.trb.status = to_leu32(0);
	cmd.trb.control = to_leu32((TRB_TYPE_CMD_ADDRESS_DEV << 10) |
	    (dev->slot << 24));

	[self synchronousRequest:&cmd toRing:m_cmdRing];
	if (cmd.result.code != 1)
		kfatal("Address device failed\n");

	return 0;
}

- (int)setupDeviceContextForDeviceOnPort:(size_t)port
			 ofHubWithHandle:(dk_usb_device_t)hub
			    deviceHandle:(out dk_usb_device_t *)handle
{
	struct device *dev;
	int r;
	dev = kmem_alloc(sizeof(*dev));

	if (hub != NULL) {
		struct device *hubDev = (struct device *)hub;

		dev->tier = hubDev->tier + 1;
		kassert(dev->tier <= 5);

		dev->parent_hub_slot = hubDev->slot;
		dev->parent_hub_port = port;
		dev->route_string = hubDev->route_string |
		    ((port & 0xF) << ((dev->tier - 1) << 2));
		dev->root_port = hubDev->root_port;
	} else {
		dev->tier = 0;
		dev->route_string = 0;
		dev->parent_hub_slot = 0;
		dev->parent_hub_port = 0;
		dev->root_port = port;
	}

	r = vm_page_alloc(&dev->context_page, 0, kPageUseKWired, false);
	if (r != 0) {
		kprintf("XHCI: Failed to allocate context page\n");
		return -1;
	}

	dev->input_ctx_phys = vm_page_paddr(dev->context_page);
	dev->device_ctx_phys = dev->input_ctx_phys +
	    ROUNDUP(sizeof(struct xhci_input_ctx), 64);

	dev->input_ctx = (volatile struct xhci_input_ctx *)P2V(
	    dev->input_ctx_phys);
	dev->device_ctx = (volatile struct xhci_device_ctx *)P2V(
	    dev->device_ctx_phys);

	memset((void *)dev->input_ctx, 0, sizeof(struct xhci_input_ctx));
	memset((void *)dev->device_ctx, 0, sizeof(struct xhci_device_ctx));

	dev->ep0_ring = [self createRingWithType:kRingTypeControl
					    size:XHCI_RING_SIZE
					 address:0
				   maxPacketSize:8
				       eventRing:m_eventRing];
	dev->ep0_ring->port = dev;

	r = [self enableSlotForDevice:dev];
	r = [self addressDevice:dev];

	*handle = (dk_usb_device_t)dev;

	return 0;
}

- (int)reconfigureDevice:(dk_usb_device_t)devHandle
	    asHubWithTTT:(uint32_t)ttt
		     mtt:(uint32_t)mtt
{
	struct device *dev = devHandle;
	volatile struct xhci_input_ctx *input_ctx = dev->input_ctx;
	volatile struct xhci_slot_ctx *slot_ctx = &input_ctx->slot;
	struct req cmd;
	uint32_t num_ports = 1;

	input_ctx->ctrl.add_flags = to_leu32(0x1); /* Slot context only */
	input_ctx->ctrl.drop_flags = to_leu32(0);

	slot_ctx->field1 = to_leu32(
	    SLOT_CTX_00_SET_ROUTE_STRING(dev->route_string) |
	    SLOT_CTX_00_SET_CONTEXT_ENTRIES(1));

	slot_ctx->field2 = to_leu32(
	    SLOT_CTX_04_SET_ROOT_HUB_PORT(dev->root_port + 1) |
	    SLOT_CTX_04_SET_NUM_PORTS(num_ports));

	slot_ctx->field4 = to_leu32(0);

	cmd.callback = NULL;
	cmd.trb.params = to_leu64(dev->input_ctx_phys);
	cmd.trb.status = to_leu32(0);
	cmd.trb.control = to_leu32((TRB_TYPE_CMD_CONFIG_EP << 10) |
	    (dev->slot << 24));

	[self synchronousRequest:&cmd toRing:m_cmdRing];

	if (cmd.result.code != 1)
		kfatal("Configure endpoint failed\n");

	return 0;
}

@end

#define M_CONTROLLER ((XHCIController *)m_controller)

@implementation XHCIRootHub

- (instancetype)initWithController:(XHCIController *)controller
			  protocol:(struct protocol *)protocol
{
	if ((self = [super init])) {
		m_nPorts = protocol->port_count;

		m_controller = controller;
		m_protocol = protocol;
		kmem_asprintf(&m_name, "xHCIRootHubUSB%u.%u", protocol->major,
		    protocol->minor);

		[M_CONTROLLER addChildHub:self];
	}

	return self;
}

- (int)getPortStatus:(size_t)port status:(usb_port_status_and_change_t *)status
{
	return [M_CONTROLLER getRootHubPort:m_protocol->port_offset + port
				     status:status];
}

- (int)setPortFeature:(size_t)port feature:(uint16_t)feature
{
	return
	    [M_CONTROLLER setRootHubPortFeature:m_protocol->port_offset + port
					feature:feature];
}

- (int)clearPortFeature:(size_t)port feature:(uint16_t)feature
{
	return [M_CONTROLLER clearPortFeature:m_protocol->port_offset + port
				      feature:feature];
}

- (int)setupDeviceContextForPort:(size_t)port
		    deviceHandle:(out dk_usb_device_t *)handle
{
	return [M_CONTROLLER
	    setupDeviceContextForDeviceOnPort:m_protocol->port_offset + port
			      ofHubWithHandle:NULL
				 deviceHandle:handle];
}

- (dk_usb_device_t)devHandle
{
	return NULL;
}

@end
