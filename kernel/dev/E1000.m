#include "ddk/virtio_pcireg.h"
#include "dev/9pSockTransport.h"
#include "dev/E1000.h"
#include "kdb/kdb_udp.h"
#include "kdk/kmem.h"
#include "kdk/kern.h"
#include "kdk/object.h"
#include "kdk/vm.h"
#include "net/keysock_dev.h"

#if defined(__amd64__)
#include "kdk/amd64.h"
#include "platform/amd64/IOAPIC.h"
#elif defined (__aarch64__)
#include "platform/aarch64-virt/GICv2Distributor.h"
#endif

@interface
E1000 (Private)
- (instancetype)initWithPCIBus:(PCIBus *)provider
			  info:(struct pci_dev_info *)info;
@end

#define PROVIDER ((PCIBus *)m_provider)

enum e1000_reg {
	kE1000RegSTATUS = 0x8,
	kE1000RegICR = 0xc0,
	kE1000RegIMS = 0xd0,
	kE1000RegIMC = 0xd8,

	kE1000RegRCTL = 0x100,
	kE1000RegTCTL = 0x400,

	kE1000RegRDBAL = 0x2800,
	kE1000RegRDBAH = 0x2804,
	kE1000RegRDLEN = 0x2808,
	kE1000RegRDH = 0x2810,
	kE1000RegRDT = 0x2818,
	kE1000RegRDTR = 0x2820,

	kE1000RegTDBAL = 0x3800,
	kE1000RegTDBAH = 0x3804,
	kE1000RegTDLEN = 0x3808,
	kE1000RegTDH = 0x3810,
	kE1000RegTDT = 0x3818,

	kE1000RegMTA = 0x5200,

	kE1000RegRAL = 0x5400,
	kE1000RegRAH = 0x5404,
};

enum e1000_tctl {
	kE1000TctlEN = 0x2,	    /*!< enable */
	kE1000TctlPSP = 0x8,	    /*!< pad short packet */
	kE1000TctlRTLC = 0x1000000, /*!< retransmit on late collission */

	kE1000TctlRRTHRESH_Shift = 28, /*!< read request threshold (bits 30:29).
					* must be 0b11 for i219!!
					* NOT tested on older E1000s! */
};

enum e1000_rctl {
	kE1000RctlEN = 0x2,
	kE1000RctlBAM = 0x8000,
	kE1000RctlSZ_2048 = 0x0,
	kE1000RctlSECRC = 0x04000000,
};

enum e1000_icr {
	kE1000IcrTXDW = 0x1,
	kE1000IcrLSC = 0x4,
	kE1000IcrRXT0 = 0x80,
};

typedef struct rx_desc {
	volatile uint64_t address;
	volatile uint16_t length;
	volatile uint16_t checksum;
	volatile uint8_t status;
	volatile uint8_t errors;
	volatile uint16_t vlan;
} __attribute__((__packed__)) rx_desc_t;

typedef struct tx_desc {
	volatile uint64_t address;
	volatile uint16_t length;
	volatile uint8_t cso;
	volatile uint8_t command;
	volatile uint8_t status;
	volatile uint8_t css;
	volatile uint16_t vlan;
} __attribute__((__packed__)) tx_desc_t;

static int counter = 0;

@implementation E1000

+ (BOOL)probeWithPCIBus:(PCIBus *)provider info:(struct pci_dev_info *)info
{
	[[self alloc] initWithPCIBus:provider info:info];
	return YES;
}

static uint32_t
e1000_read(vaddr_t base, int reg)
{
	return *(volatile uint32_t *)(base + reg);
}

static void
e1000_write(vaddr_t base, int reg, uint32_t val)
{
	*(volatile uint32_t *)(base + reg) = val;
}

static int
e1000_read_mac(vaddr_t base, uint8_t *out)
{
	uint32_t val;

	val = e1000_read(base, kE1000RegRAL);
	if (val == 0)
		return -1;

	out[0] = val & 0xff;
	out[1] = (val >> 8) & 0xff;
	out[2] = (val >> 16) & 0xff;
	out[3] = (val >> 24) & 0xff;

	val = e1000_read(base, kE1000RegRAH);
	out[4] = val & 0xff;
	out[5] = (val >> 8) & 0xff;

	return 0;
}

- (void)disableInterrupts
{
	e1000_write(m_reg, kE1000RegIMC, 0xffffffff);
	e1000_read(m_reg, kE1000RegICR);
}

- (void)initReceive
{
	m_rxDescs = kmem_alloc(sizeof(rx_desc_t) * E1000_NDESCS);

	for (int i = 0; i < 128; ++i)
		e1000_write(m_reg, kE1000RegMTA + 4 * i, 0x0);

	for (size_t i = 0; i < E1000_NDESCS; i++) {
		rx_desc_t *desc = &m_rxDescs[i];

		desc->address = vm_page_paddr(m_packet_buf_pages[i / 2]) +
		    (i % 2) * 2048;
		desc->length = 2048;
		desc->status = 0;
	}

	e1000_write(m_reg, kE1000RegRDBAL, V2P(m_rxDescs) & 0xffffffff);
	e1000_write(m_reg, kE1000RegRDBAH, V2P(m_rxDescs) >> 32);
	e1000_write(m_reg, kE1000RegRDLEN, E1000_NDESCS * sizeof(rx_desc_t));
	e1000_write(m_reg, kE1000RegRDH, 0);
	e1000_write(m_reg, kE1000RegRDT, E1000_NDESCS - 1);
	e1000_write(m_reg, kE1000RegRDTR, 0);

	m_rxHead = 0;
	m_rxNextTail = 0;
}

- (void)initTransmit
{
	m_txDescs = kmem_alloc(sizeof(tx_desc_t) * E1000_NDESCS);

	for (int i = 0; i < 128; ++i)
		e1000_write(m_reg, kE1000RegMTA + 4 * i, 0x00000000);

	for (size_t i = 0; i < E1000_NDESCS; i++) {
		tx_desc_t *desc = &m_txDescs[i];

		desc->address = vm_page_paddr(m_packet_buf_pages[127 + i / 2]) +
		    (i % 2) * 2048;
		desc->length = 0;
		desc->cso = 0;
		desc->command = 0;
		desc->status = 0;
		desc->css = 0;
		desc->vlan = 0;
	}

	e1000_write(m_reg, kE1000RegTDBAL, V2P(m_txDescs) & 0xffffffff);
	e1000_write(m_reg, kE1000RegTDBAH, V2P(m_txDescs) >> 32);
	e1000_write(m_reg, kE1000RegTDLEN, E1000_NDESCS * sizeof(tx_desc_t));
	e1000_write(m_reg, kE1000RegTDH, 0);
	e1000_write(m_reg, kE1000RegTDT, 0);
	m_txHead = 0;
	m_txTail = 0;
}

- (void)enableInterrupts
{
	e1000_write(m_reg, kE1000RegIMS,
	    kE1000IcrTXDW | kE1000IcrLSC | kE1000IcrRXT0);
}

- (void)completeProcessingOfRxIndex:(size_t)index locked:(BOOL)isLocked
{
	struct rx_desc *desc;
	ipl_t ipl = kIPL0;

	if (!isLocked)
		ipl = ke_spinlock_acquire(&m_rxLock);

	desc = &m_rxDescs[index];

	if (index != m_rxNextTail) {
		kprintf("For FUCK sake, Index != rxNextTail!! %zu != %zu!\n",
		    index, m_rxNextTail);
		for (;;)
			;
	}
	kassert(index == m_rxNextTail);

	m_rxNextTail = (m_rxNextTail + 1) % E1000_NDESCS;
	desc->length = 2048;
	desc->status = 0;
	e1000_write(m_reg, kE1000RegRDT, index);

	if (!isLocked)
		ke_spinlock_release(&m_rxLock, ipl);
}

- (BOOL)debuggerPoll
{
	struct rx_desc *desc = &m_rxDescs[m_rxHead];
	void *data = (void *)P2V(desc->address);
	size_t id = m_rxHead;

	while (!(m_rxDescs[m_rxHead].status & 0x1))
		;

	memcpy(kdb_udp_rx_pbuf.payload, data, desc->length);
	kdb_udp_rx_pbuf.len = kdb_udp_rx_pbuf.tot_len = desc->length;

	m_rxHead = (m_rxHead + 1) % E1000_NDESCS;
	[self completeProcessingOfRxIndex:id locked:YES];

	return YES;
}

- (void)rxDeferredProcessing
{
	ke_spinlock_acquire_nospl(&m_rxLock);
	while (m_rxDescs[m_rxHead].status & 0x1) {
		struct rx_desc *desc = &m_rxDescs[m_rxHead];
		void *data = (void *)P2V(desc->address);
		size_t id = m_rxHead;

		m_rxHead = (m_rxHead + 1) % E1000_NDESCS;

		[super queueReceivedDataForProcessing:data
					       length:desc->length
						   id:id];
	}
	ke_spinlock_release_nospl(&m_rxLock);
}

- (BOOL)tryTransmitPacket:(struct pbuf *)pkt
{
	size_t reqDescs, availDescs;

	reqDescs = 1; /* can become pkt phys breaks */
	availDescs = (m_txTail >= m_txHead) ?
	    (E1000_NDESCS - m_txTail + m_txHead - 1) :
	    (m_txHead - m_txTail - 1);

	if (reqDescs > availDescs)
		return NO;

	for (int i = 0; i < reqDescs; i++) {
		tx_desc_t *desc = &m_txDescs[m_txTail];
		uint8_t cmd;

		pbuf_copy_partial(pkt, (void *)P2V(desc->address), 2048, 0);
		desc->length = pkt->tot_len;
		desc->status = 0;

		/* RS everything, EOP last */
		cmd = (1 << 1) /* insert FCS */ | (1 << 3) /* report status */;
		if (i == (reqDescs - 1))
			cmd |= 0x1; /* EOP */

		desc->command = cmd;
		desc->cso = 0;
		desc->css = 0;
		desc->vlan = 0;
		m_txTail = (m_txTail + 1) % E1000_NDESCS;
	}

	e1000_write(m_reg, kE1000RegTDT, m_txTail);

	return YES;
}

- (void)submitPacket:(struct pbuf *)pkt
{
	ke_spinlock_acquire_nospl(&m_txLock);

	if (m_txPendingHead == NULL && [self tryTransmitPacket:pkt]) {
		ke_spinlock_release_nospl(&m_txLock);
		return;
	}

#if 0
	if (m_txPendingHead == NULL) {
		m_txPendingHead = m_txPendingTail = pkt;
	} else {
		m_txPendingTail->next = pkt;
		m_txPendingTail = pkt;
	}
	pkt->next = NULL;

	ke_spinlock_release_nospl(&m_txLock);
#endif
}

- (void)debuggerTransmit:(struct pbuf *)pbuf
{
	BOOL sent = [self tryTransmitPacket:pbuf];

	kassert(sent == YES);

	while (m_txDescs[m_txHead].status & 0x0)
		;

	while (m_txDescs[m_txHead].status & 0x1) {
		m_txDescs[m_txHead].status = 0;
		m_txHead = (m_txHead + 1) % E1000_NDESCS;
	}
}

- (void)txDeferredProcessing
{
	while (m_txDescs[m_txHead].status & 0x1) {
		m_txDescs[m_txHead].status = 0;
		m_txHead = (m_txHead + 1) % E1000_NDESCS;
	}

#if 0
	pkt_buf_t *prev = NULL;
	pkt_buf_t *current = m_txPendingHead;

	ke_spinlock_acquire_nospl(&m_txLock);

	while (m_txDescs[m_txHead].status & 0x1) {
		m_txDescs[m_txHead].status = 0;
		m_txHead = (m_txHead + 1) % E1000_NDESCS;
	}

	while (current != NULL) {
		if ([self tryTransmitPacket:current]) {
			if (prev == NULL)
				m_txPendingHead = current->next;
			else
				prev->next = current->next;

			if (current->next == NULL)
				m_txPendingHead = prev;

		} else {
			/* no space left - wait for another TXDW interrupt */
			break;
		}

		prev = current;
		current = current->next;
	}

	ke_spinlock_release_nospl(&m_txLock);
#endif
}

- (void)linkDeferredProcessing
{
	uint32_t dstat = e1000_read(m_reg, kE1000RegSTATUS);
	if (dstat & 2)
		[super setLinkUp:YES speed:1000 fullDuplex:dstat & 1];
	else
		[super setLinkUp:NO speed:0 fullDuplex:NO];
}

- (BOOL)handleInterrupt
{
	uint32_t stat = e1000_read(m_reg, kE1000RegICR);

	if (stat & kE1000IcrLSC)
		ke_dpc_enqueue(&m_linkDpc);

	if (stat & kE1000IcrRXT0)
		ke_dpc_enqueue(&m_rxDpc);

	if (stat & kE1000IcrTXDW)
		ke_dpc_enqueue(&m_txDpc);

	return YES;
}

static bool
e1000_handler(md_intr_frame_t *, void *arg)
{
	E1000 *self = arg;
	return [self handleInterrupt];
}

static void
rx_dpc(void *arg)
{
	E1000 *self = arg;
	[self rxDeferredProcessing];
}

static void
tx_dpc(void *arg)
{
	E1000 *self = arg;
	[self txDeferredProcessing];
}

static void
link_dpc(void *arg)
{
	E1000 *self = arg;
	[self linkDeferredProcessing];
}

- (void)dealloc
{
	for (int i = 0; i < elementsof(m_packet_buf_pages); i++) {
		if (m_packet_buf_pages[i] == NULL)
			break;

		vm_page_delete(m_packet_buf_pages[i]);
		vm_page_release(m_packet_buf_pages[i]);
	}

	if (m_txDescs != NULL)
		kmem_free(m_txDescs, sizeof(tx_desc_t) * E1000_NDESCS);

	if (m_rxDescs != NULL)
		kmem_free(m_rxDescs, sizeof(tx_desc_t) * E1000_NDESCS);

	[super dealloc];
}

- (instancetype)initWithPCIBus:(PCIBus *)provider
			  info:(struct pci_dev_info *)info
{
	int r;

	self = [super initWithProvider:provider];
	kmem_asprintf(obj_name_ptr(self), "e1000-%u", counter++);

	[super setupForQueueLength:E1000_NDESCS rxBufSize:2048];

	m_pciInfo = *info;
	m_rxDpc.arg = self;
	m_rxDpc.callback = rx_dpc;
	m_rxDpc.cpu = NULL;

	m_txDpc.arg = self;
	m_txDpc.callback = tx_dpc;
	m_txDpc.cpu = NULL;

	m_linkDpc.arg = self;
	m_linkDpc.callback = link_dpc;
	m_linkDpc.cpu = NULL;

	[PCIBus enableBusMasteringForInfo:info];
	[PCIBus setMemorySpaceForInfo:info enabled:false];

	m_reg = [PCIBus getBar:0 forInfo:info];
	if (m_reg == 0) {
		DKDevLog(self, "Failed to get bar 0\n");
		[self release];
		return nil;
	}

	m_reg = P2V(m_reg);

	[PCIBus setMemorySpaceForInfo:info enabled:true];

	r = e1000_read_mac(m_reg, m_mac);
	if (r != 0) {
		DKDevLog(self, "Failed to read MAC address\n");
		[self release];
		return nil;
	}

	[self disableInterrupts];

	for (int i = 0; i < elementsof(m_packet_buf_pages); i++) {
		r = vm_page_alloc(&m_packet_buf_pages[i], 0, kPageUseKWired,
		    false);
		if (r != 0) {
			DKDevLog(self, "Failed to allocate packet buffers\n");
			[self release];
			return nil;
		}
	}

	[self initReceive];
	[self initTransmit];

	e1000_write(m_reg, kE1000RegTCTL,
	    0b11 << kE1000TctlRRTHRESH_Shift | kE1000TctlEN | kE1000TctlPSP |
		kE1000TctlRTLC);
	e1000_write(m_reg, kE1000RegRCTL,
	    kE1000RctlEN | kE1000RctlBAM | kE1000RctlSZ_2048 | kE1000RctlSECRC);

#if defined(__amd64__)
	r = [IOApic handleGSI:m_pciInfo.gsi
		  withHandler:e1000_handler
		     argument:self
		isLowPolarity:m_pciInfo.lopol
	      isEdgeTriggered:m_pciInfo.edge
		   atPriority:kIPLHigh
			entry:&m_intxEntry];
#elif defined(__aarch64__)
	r = [GICv2Distributor handleGSI:m_pciInfo.gsi
		  withHandler:e1000_handler
		     argument:self
		isLowPolarity:m_pciInfo.lopol
	      isEdgeTriggered:m_pciInfo.edge
		   atPriority:kIPLHigh
			entry:&m_intxEntry];
#else
	r = -1;
#endif
	if (r < 0) {
		DKDevLog(self, "Failed to allocate interrupt handler: %d\n", r);
		[self release];
		return nil;
	}

	[super setupNetif];
	[self linkDeferredProcessing];

	[self enableInterrupts];
	[PCIBus setInterruptsEnabled:YES forInfo:&m_pciInfo];

	[self registerDevice];
	DKLogAttachExtra(self, "Ethernet address " MAC_FMT, MAC_ARGS(m_mac));

	return self;
}

- (void)capabilityEnumeratedAtOffset:(voff_t)capOffset
{
}

@end
