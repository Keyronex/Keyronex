/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Wed Jan 29 2025.
 */

#include <ddk/DKPCIDevice.h>
#include <kdk/kern.h>
#include <kdk/libkern.h>
#include <kdk/vm.h>

#include "kdk/kmem.h"
#include "vm/vmp.h"
#include "xhci_reg.h"

@class XHCIController;

#define XHCI_RING_SIZE 256
#define XHCI_EVENT_RING_SIZE 256
#define XHCI_CMD_RING_SIZE 256

struct ring {
	volatile struct xhci_trb *trbs;
	uintptr_t paddr;
	uint32_t size;
	uint32_t cycle_bit;
	uint32_t enqueue_idx;
	uint32_t dequeue_idx;
};

struct protocol {
	uint8_t major;
	uint8_t minor;
	uint8_t slot_type;
	uint8_t port_count;
	uint8_t port_offset;
};

enum port_state {
	kPortStateNotConnected,
	kPortStateResetting,
	kPortStateEnablingSlot,
	kPortStateAddressingDevice,
	kPortStateAddressed,
};

struct cmd {
	TAILQ_ENTRY(cmd) link;
	struct xhci_trb trb;
	paddr_t trb_paddr;

	void (*callback)(XHCIController *, struct cmd *, uint8_t code,
	    uint8_t slot_id, void *);
	void *context;
};

struct port {
	uint8_t port_num;
	struct protocol *protocol;
	enum port_state state;
	struct cmd cmd;

	uint8_t slot;

	vm_page_t *context_page;
	volatile struct xhci_input_ctx *input_ctx;
	paddr_t input_ctx_phys;
	volatile struct xhci_device_ctx *device_ctx;
	paddr_t device_ctx_phys;

	struct ring *ep0_ring;
};

@interface XHCIController : DKDevice <DKPCIDeviceMatching> {
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
	kdpc_t m_dpc;

	struct protocol *m_protocols;
	size_t m_nProtocols;

	size_t m_nPorts;
	struct port *m_ports;

	TAILQ_HEAD(, cmd) m_pendingCmds;
	TAILQ_HEAD(, cmd) m_inflightCmds;
	kspinlock_t m_cmdQueueLock;
}

+ (void)load;

+ (uint8_t)probeWithMatchData:(DKPCIMatchData *)matchData;

@end

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
		m_name = strdup("xhciController");
	}

	return self;
}

static int
dk_allocate_and_map(vaddr_t *out_vaddr, paddr_t *out_paddr, size_t size)
{
	vm_page_t *page;
	int r;

	r = vm_page_alloc(&page, vm_bytes_to_order(size), kPageUseKWired,
	    false);
	if (r != 0)
		return r;

	r = vm_ps_map_physical_view(&kernel_procstate, out_vaddr,
	    PGROUNDUP(size), vm_page_paddr(page), kVMRead | kVMWrite,
	    kVMRead | kVMWrite, false);
	if (r != 0) {
		vm_page_delete(page);
		vm_page_release(page);
		return r;
	}
	*out_paddr = vm_page_paddr(page);
	return 0;
}

- (struct ring *)createRingWithSize:(size_t)numTrbs
{
	struct ring *ring = kmem_alloc(sizeof(*ring));
	volatile struct xhci_trb *linkTRB;
	vaddr_t vaddr;
	paddr_t paddr;

	if (ring == NULL)
		return NULL;

	memset(ring, 0, sizeof(*ring));

	size_t ringBytes = numTrbs * sizeof(struct xhci_trb);

	if (dk_allocate_and_map(&vaddr, &paddr, ringBytes) != 0) {
		kmem_free(ring, sizeof(*ring));
		return NULL;
	}

	ring->trbs = (volatile struct xhci_trb *)vaddr;
	ring->size = (uint32_t)numTrbs;
	ring->cycle_bit = 1;
	ring->enqueue_idx = 0;
	ring->dequeue_idx = 0;
	ring->paddr = paddr;

	memset((void *)ring->trbs, 0, ringBytes);

	linkTRB = &ring->trbs[numTrbs - 1];
	linkTRB->params = to_leu64(ring->paddr);
	linkTRB->status = to_leu32(0);
	linkTRB->control = to_leu32((TRB_TYPE_LINK << 10) | (1 << 1) |
	    ring->cycle_bit);

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

	m_cmdRing = [self createRingWithSize:XHCI_CMD_RING_SIZE];
	if (m_cmdRing == NULL)
		return -1;

	m_eventRing = [self createRingWithSize:XHCI_EVENT_RING_SIZE];
	if (m_eventRing == NULL)
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

- (void)enqueueOneCommandToRing:(struct cmd *)cmd
{
	struct ring *ring = m_cmdRing;
	volatile struct xhci_trb *trb = &ring->trbs[ring->enqueue_idx];

	memset((void *)trb, 0, sizeof(*trb));

	trb->params = cmd->trb.params;
	trb->status = to_leu32(0);
	trb->control = to_leu32(from_leu32((cmd->trb.control)) |
	    (ring->cycle_bit & 1));

	cmd->trb_paddr = ring->paddr +
	    (ring->enqueue_idx * sizeof(struct xhci_trb));

	ring->enqueue_idx++;
	if (ring->enqueue_idx == ring->size - 1) {
		ring->enqueue_idx = 0;
		ring->cycle_bit ^= 1;
	}

	[self ringCommandDoorbell];
}

- (void)drainPendingCommands
{
	ipl_t ipl = ke_spinlock_acquire(&m_cmdQueueLock);

	while (true) {
		struct cmd *cmd = TAILQ_FIRST(&m_pendingCmds);
		if (cmd == NULL)
			break;

		if (![self ringHasSpace:m_cmdRing needed:1])
			break;

		TAILQ_REMOVE(&m_pendingCmds, cmd, link);
		[self enqueueOneCommandToRing:cmd];
		TAILQ_INSERT_TAIL(&m_inflightCmds, cmd, link);
	}

	ke_spinlock_release(&m_cmdQueueLock, ipl);
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

- (void)completeCommandTRBAtPAddr:(paddr_t)paddr
			 withCode:(uint32_t)code
			   slotID:(uint32_t)slotID
{
	ipl_t ipl = ke_spinlock_acquire(&m_cmdQueueLock);
	struct cmd *cmd;

	TAILQ_FOREACH (cmd, &m_inflightCmds, link) {
		if (cmd->trb_paddr == paddr) {
			/* completions should be in-order */
			kassert(paddr == m_cmdRing->paddr +
			    (m_cmdRing->dequeue_idx * sizeof(struct xhci_trb)));
			TAILQ_REMOVE(&m_inflightCmds, cmd, link);
			m_cmdRing->dequeue_idx++;
			ke_spinlock_release(&m_cmdQueueLock, ipl);

			cmd->callback(self, cmd, code, slotID, cmd->context);
			return;
		}
	}

	kfatal("XHCI: TRB completion for unknown command\n");
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
		case TRB_TYPE_EVENT_PORT_STATUS: {
			uint64_t params = from_leu64(trb->params);
			uint8_t portNum = (params >> 24) & 0xff;
			[self updatePortState:&m_ports[portNum - 1]];
			break;
		}

		case TRB_TYPE_EVENT_CMD_COMPLETE: {
			uint32_t code = (from_leu32(trb->status) >> 24) & 0xFF;
			uint32_t slotId = (from_leu32(trb->control) >> 24) &
			    0xFF;
			paddr_t paddr = from_leu64(trb->params);

			[self completeCommandTRBAtPAddr:paddr
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
xhci_dpc(void *arg)
{
	XHCIController *self = arg;
	[self deferredProcessing];
	[self drainPendingCommands];
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

	ke_dpc_enqueue(&self->m_dpc);

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

static inline void
xhci_clear_port_changes(volatile struct xhci_port_regs *pregs, uint32_t mask)
{
	uint32_t ps = from_leu32(pregs->PORTSC) & ~XHCI_PORTSC_PED;
	ps |= mask;
	pregs->PORTSC = to_leu32(ps);
}

- (void)startPortReset:(struct port *)port
{
	volatile struct xhci_port_regs *pregs =
	    &m_opRegs->ports[port->port_num];
	uint32_t ps = from_leu32(pregs->PORTSC);

	kprintf("XHCI: port %u: Starting reset\n", port->port_num);

	ps |= XHCI_PORTSC_PR;
	pregs->PORTSC = to_leu32(ps);
}

- (void)postCommand:(struct cmd *)cmd
{
	ipl_t ipl = ke_spinlock_acquire(&m_cmdQueueLock);

	TAILQ_INSERT_TAIL(&m_pendingCmds, cmd, link);

	ke_spinlock_release(&m_cmdQueueLock, ipl);

	ke_dpc_enqueue(&m_dpc);
}

static void
address_device_done(XHCIController *self, struct cmd *cmd, uint8_t code,
    uint8_t slot_id, void *context)
{
	kfatal("Results: code %d slot_id %d\n", code, slot_id);
}

- (void)setupDeviceContextForPort:(struct port *)port
{
	volatile struct xhci_input_ctx *input_ctx = port->input_ctx;
	volatile struct xhci_slot_ctx *slot_ctx = &input_ctx->slot;
	volatile struct xhci_ep_ctx *ep0_ctx = &input_ctx->ep[0];
	struct ring *ep0_ring = port->ep0_ring;
	uint32_t route_string = 0; /* root hub port */

	input_ctx->ctrl.drop_flags = to_leu32(0);
	input_ctx->ctrl.add_flags = to_leu32(0x3); /* Slot and EP0 contexts */

	slot_ctx->field1 = to_leu32(SLOT_CTX_00_SET_ROUTE_STRING(route_string) |
	    SLOT_CTX_00_SET_CONTEXT_ENTRIES(1));
	slot_ctx->field2 = to_leu32(
	    SLOT_CTX_04_SET_ROOT_HUB_PORT(port->port_num + 1));
	slot_ctx->field3 = to_leu32(0);
	slot_ctx->field4 = to_leu32(0);

	ep0_ctx->field1 = to_leu32(0);
	ep0_ctx->field2 = to_leu32(
	    (3 << 1) | (4 << 3)); /* CErr 3, EPType ctrl */
	ep0_ctx->dequeue_ptr = to_leu64(ep0_ring->paddr | ep0_ring->cycle_bit);
	ep0_ctx->tx_info = to_leu32(8 << 16); /* average TRB Length = 8 */

	port->cmd.callback = address_device_done;

	port->cmd.trb.params = to_leu64(port->input_ctx_phys);
	port->cmd.trb.status = to_leu32(0);
	port->cmd.trb.control = to_leu32((TRB_TYPE_CMD_ADDRESS_DEV << 10) |
	    (port->slot << 24));

	[self postCommand:&port->cmd];
}

static void
port_enable_slot_done(XHCIController *self, struct cmd *cmd, uint8_t code,
    uint8_t slot_id, void *context)
{
	struct port *port = context;

	if (code != 1) {
		kprintf("XHCI: Port %u: slot assignment failed (%d)\n",
		    port->port_num, code);
		kfatal("Todo: handle slot assignment failure\n");
		return;
	}

	kprintf("XHCI: Port %u: slot assigned (%d)\n", port->port_num, slot_id);

	port->slot = slot_id;
	port->state = kPortStateAddressingDevice;

	[self setupDeviceContextForPort:port];
}

- (void)enableSlotForPort:(struct port *)port
{
	port->cmd.callback = port_enable_slot_done;
	port->cmd.context = port;
	port->cmd.trb.params = to_leu64(0);
	port->cmd.trb.status = to_leu32(0);
	port->cmd.trb.control = to_leu32((TRB_TYPE_CMD_ENABLE_SLOT << 10) |
	    ((uint32_t)port->protocol->slot_type << 16));
	kprintf("POSTING COMMAND!\n");
	[self postCommand:&port->cmd];
}

- (void)updatePortState:(struct port *)port
{
	volatile struct xhci_port_regs *pregs =
	    &m_opRegs->ports[port->port_num];
	uint32_t portsc = from_leu32(pregs->PORTSC);

	bool ccs = (portsc & XHCI_PORTSC_CCS) != 0; /* Current Connect Status */
	bool ped = (portsc & XHCI_PORTSC_PED) != 0; /* Port Enabled */
	bool pr = (portsc & XHCI_PORTSC_PR) != 0;   /* Port Reset */
	bool plsInU0 = ((portsc >> 5) & 0xF) == 0;  /* Link state = U0? */

	bool csc = (portsc & XHCI_PORTSC_CSC) != 0; /* Connect Status Chane */
	bool prc = (portsc & XHCI_PORTSC_PRC) != 0; /* Port Reset Chane */
	bool pec = (portsc & XHCI_PORTSC_PEC) != 0; /* Port Enable Change */

	switch (port->state) {

	/* waiting for CSC */
	case kPortStateNotConnected:
		if (ccs) {
			if (csc)
				xhci_clear_port_changes(pregs, XHCI_PORTSC_CSC);

			port->state = kPortStateResetting;
			[self startPortReset:port];
		} else {
			kprintf("XHCI: spurious 1\n");
		}
		break;

	/* waiting for PR to complete */
	case kPortStateResetting:
		if (!pr) {
			kprintf("XHCI: Port %u: reset done, portsc=0x%x\n",
			    port->port_num, portsc);

			if (prc) {
				xhci_clear_port_changes(pregs, XHCI_PORTSC_PRC);
			}

			if (ped) {
				kprintf("XHCI: Port %u: now enabled\n",
				    port->port_num);
				port->state = kPortStateEnablingSlot;
				[self enableSlotForPort:port];
			} else {
				kprintf(
				    "XHCI: Port %u: reset done but not enabled\n",
				    port->port_num);
				port->state = kPortStateNotConnected;
			}
		}
		break;

	default:
		kfatal("XHCI: port state change in unrecognised state\n");
		break;
	}

	if (!ccs && port->state != kPortStateNotConnected) {
		kprintf("XHCI: Port %u: device disconnected\n", port->port_num);
		port->state = kPortStateNotConnected;
	}

	if (pec) {
		xhci_clear_port_changes(pregs, XHCI_PORTSC_PEC);
	}
}

- (void)enumeratePorts
{
	for (size_t i = 0; i < m_nPorts; i++) {
		struct port *port = &m_ports[i];
		struct protocol *protocol = port->protocol;
		volatile struct xhci_port_regs *regs = &m_opRegs->ports[i];

		kassert(protocol != NULL);

		if (from_leu32(regs->PORTSC) & XHCI_PORTSC_CCS)
			[self updatePortState:port];
	}
}

- (void)start
{
	DKPCIBarInfo barInfo = [m_pciDevice barInfo:0];
	int r;

	TAILQ_INIT(&m_pendingCmds);
	TAILQ_INIT(&m_inflightCmds);
	ke_spinlock_init(&m_cmdQueueLock);

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
	m_dpc.cpu = NULL;
	m_dpc.callback = xhci_dpc;
	m_dpc.arg = self;

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

	m_nPorts = from_leu32(m_capRegs->HCSPARAMS1) >> 24;
	m_ports = kmem_alloc(sizeof(struct port) * m_nPorts);

	for (size_t i = 0; i < m_nProtocols; i++) {
		struct protocol *protocol = &m_protocols[i];
		for (size_t j = 0; j < protocol->port_count; j++) {
			struct port *port;

			kassert(protocol->port_offset + j < m_nPorts);

			port = &m_ports[protocol->port_offset + j];

			port->port_num = protocol->port_offset + j;
			port->protocol = protocol;
			port->state = kPortStateNotConnected;

			r = vm_page_alloc(&port->context_page, 0, kPageUseKWired,
			    false);
			if (r != 0) {
				kprintf("XHCI: Failed to allocate context page\n");
				return;
			}

			port->input_ctx_phys = vm_page_paddr(port->context_page);
			port->device_ctx_phys = port->input_ctx_phys +
			    ROUNDUP(sizeof(struct xhci_input_ctx), 64);

			port->input_ctx = (volatile struct xhci_input_ctx *)
			    P2V(port->input_ctx_phys);
			port->device_ctx = (volatile struct xhci_device_ctx *)
			    P2V(port->device_ctx_phys);

			memset((void *)port->input_ctx, 0,
			    sizeof(struct xhci_input_ctx));
			memset((void *)port->device_ctx, 0,
			    sizeof(struct xhci_device_ctx));

			port->ep0_ring = [self
			    createRingWithSize:XHCI_RING_SIZE];
		}
	}

	m_opRegs->CONFIG = to_leu32((uint32_t)m_maxSlots);

	cmd = from_leu32(m_opRegs->USBCMD);
	cmd |= (XHCI_CMD_RUN | XHCI_CMD_INTE);
	m_opRegs->USBCMD = to_leu32(cmd);

	while (from_leu32(m_opRegs->USBSTS) & XHCI_STS_HCH)
		asm("pause");

	kprintf("XHCI Controller set up\n");

	[self enumeratePorts];
}

@end
