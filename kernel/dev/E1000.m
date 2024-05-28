#include "ddk/virtio_pcireg.h"
#include "dev/E1000.h"
#include "dev/amd64/IOAPIC.h"
#include "kdk/amd64.h"
#include "kdk/kmem.h"
#include "kdk/nanokern.h"
#include "kdk/object.h"
#include "kdk/vm.h"

/* placeholder... */
typedef struct pkt_buf {
	paddr_t data;
	uint16_t length;
	struct pkt_buf *next;
} pkt_buf_t;

@interface E1000 (Private)
- (instancetype)initWithPCIBus:(PCIBus *)provider
			  info:(struct pci_dev_info *)info;
@end

#define PROVIDER ((PCIBus *)m_provider)
#define MAC_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC_ARGS(MAC) MAC[0], MAC[1], MAC[2], MAC[3], MAC[4], MAC[5]

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

	kE1000RegTDBAL = 0x38000,
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

uint16_t
ntohs(uint16_t in)
{
	return (in >> 8) | (in << 8);
}

- (void)completeProcessingOfRxIndex:(size_t)index
{
	struct rx_desc *desc = &m_rxDescs[index];

	kassert(index == m_rxNextTail);

	m_rxNextTail = (m_rxNextTail + 1) % E1000_NDESCS;
	desc->length = 2048;
	desc->status = 0;
	e1000_write(m_reg, kE1000RegRDT, index);
}

static void
processPacket(void *data, size_t len, size_t index, void *arg)
{
	struct ethframe {
		uint8_t dst[6];
		uint8_t src[6];
		uint16_t type;
	} *header = data;

	kprintf("(DEST " MAC_FMT " SRC " MAC_FMT " TYPE %x)\n",
	    MAC_ARGS(header->dst), MAC_ARGS(header->src), ntohs(header->type));

	/* In the future this will be asynchronously invoked later! */
	[(E1000 *)arg completeProcessingOfRxIndex:index];
}

- (void)rxDeferredProcessing
{
	ke_spinlock_acquire_nospl(&m_rxLock);
	while (m_rxDescs[m_rxHead].status & 0x1) {
		struct rx_desc *desc = &m_rxDescs[m_rxHead];

		processPacket((void *)P2V(desc->address), desc->length,
		    m_rxHead, self);

		m_rxHead = (m_rxHead + 1) % E1000_NDESCS;
	}
	ke_spinlock_release_nospl(&m_rxLock);
}

- (BOOL)tryTransmitPacket:(pkt_buf_t *)pkt
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
		/* can become one part of pkt */
		desc->address = pkt->data;
		desc->length = pkt->length;
		desc->status = 0;
		/* RS everything, EOP last */
		desc->command = (i == (reqDescs - 1)) ? 0x9 : 0x1;
		m_txTail = (m_txTail + 1) % E1000_NDESCS;
	}

	e1000_write(m_reg, kE1000RegTDT, m_txTail);

	return YES;
}

- (void)submitPacket:(pkt_buf_t *)pkt
{
	ke_spinlock_acquire_nospl(&m_txLock);

	if (m_txPendingHead == NULL && [self tryTransmitPacket:pkt])
		return;

	if (m_txPendingHead == NULL) {
		m_txPendingHead = m_txPendingTail = pkt;
	} else {
		m_txPendingTail->next = pkt;
		m_txPendingTail = pkt;
	}
	pkt->next = NULL;

	ke_spinlock_release_nospl(&m_txLock);
	e1000_write(m_reg, kE1000RegIMS, kE1000IcrTXDW);
}

- (void)txDeferredProcessing
{
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
}

- (void)linkDeferredProcessing
{
	uint32_t dstat = e1000_read(m_reg, kE1000RegSTATUS);
	kprintf("LSC! Stat = %x\n", dstat);
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
	    kE1000TctlEN | kE1000TctlPSP | kE1000TctlRTLC);
	e1000_write(m_reg, kE1000RegRCTL,
	    kE1000RctlEN | kE1000RctlBAM | kE1000RctlSZ_2048 | kE1000RctlSECRC);

	r = [IOApic handleGSI:m_pciInfo.gsi
		  withHandler:e1000_handler
		     argument:self
		isLowPolarity:m_pciInfo.lopol
	      isEdgeTriggered:m_pciInfo.edge
		   atPriority:kIPLHigh
			entry:&m_intxEntry];
	if (r < 0) {
		DKDevLog(self, "Failed to allocate interrupt handler: %d\n", r);
		[self release];
		return nil;
	}

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
