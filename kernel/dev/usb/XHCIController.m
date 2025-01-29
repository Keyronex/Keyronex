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

#define XHCI_RING_SIZE 256
#define XHCI_EVENT_RING_SIZE 256
#define XHCI_CMD_RING_SIZE 256

/* Ring */
struct xhci_ring {
	volatile struct xhci_trb *trbs;
	uintptr_t paddr;
	uint32_t size;
	uint32_t cycle_bit;
	uint32_t enqueue_idx;
	uint32_t dequeue_idx;
};

@interface XHCIController : DKDevice <DKPCIDeviceMatching> {
	DKPCIDevice *m_pciDevice;

	vaddr_t m_mmioBase;
	volatile struct xhci_host_cap_regs *m_capRegs;
	volatile struct xhci_host_op_regs *m_opRegs;
	volatile struct xhci_host_rt_regs *m_rtRegs;
	volatile leu32_t *m_doorBells;

	struct xhci_ring *m_cmdRing;
	struct xhci_ring *m_eventRing;
	struct xhci_erst_entry *m_erstTable;
	struct xhci_dcbaa_entry *m_dcbaa;
	size_t m_maxSlots;
	paddr_t m_erstTablePhys;
	paddr_t m_dcbaaPhys;
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

/**
 * Create a new ring structure of the given size (in TRBs).
 * Returns a pointer to a newly allocated struct xhci_ring on success,
 * or NULL on failure.
 */
- (struct xhci_ring *)createRingWithSize:(size_t)numTrbs
{
	struct xhci_ring *ring = kmem_alloc(sizeof(*ring));
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

- (void)start
{
	DKPCIBarInfo barInfo = [m_pciDevice barInfo:0];
	int r;

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

	m_opRegs->CONFIG = to_leu32((uint32_t)m_maxSlots);

	cmd = from_leu32(m_opRegs->USBCMD);
	cmd |= (XHCI_CMD_RUN | XHCI_CMD_INTE);
	m_opRegs->USBCMD = to_leu32(cmd);

	while (from_leu32(m_opRegs->USBSTS) & XHCI_STS_HCH)
		asm("pause");

	kprintf("XHCI Controller set up\n");

}

@end
