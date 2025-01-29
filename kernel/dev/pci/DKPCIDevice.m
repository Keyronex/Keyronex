/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Fri Nov 01 2024.
 */

#include <ddk/DKPCIDevice.h>
#include <ddk/reg/pci.h>
#include <kdk/kmem.h>
#include <ddk/DKAxis.h>

#define UNPACK_ADDRESS(addr) \
	(addr).segment, (addr).bus, (addr).slot, (addr).function

struct match_entry {
	LIST_ENTRY(match_entry) qlink;
	Class<DKPCIDeviceMatching> matchingClass;
};

static LIST_HEAD(, match_entry) match_list = LIST_HEAD_INITIALIZER(&match_list);
static kmutex_t match_list_lock = KMUTEX_INITIALIZER(match_list_lock);

@implementation DKPCIDevice

+ (void)registerMatchingClass:(Class<DKPCIDeviceMatching>)matchingClass
{
	struct match_entry *entry = kmem_alloc(sizeof(*entry));
	entry->matchingClass = matchingClass;
	ke_wait(&match_list_lock, __PRETTY_FUNCTION__, 0, 0, -1);
	LIST_INSERT_HEAD(&match_list, entry, qlink);
	ke_mutex_release(&match_list_lock);
}

- (instancetype)initWithBridge:(DKPCIBridge *)bridge
		       address:(DKPCIAddress *)address
{
	if ((self = [super init])) {
		kmem_asprintf(&m_name, "pciDev%u:%u", address->slot,
		    address->function);
		m_address = *address;
	}

	return self;
}

- (void)match
{
	struct match_entry *entry;
	DKPCIMatchData matchData;
	uint8_t highestMatch = 0;
	Class<DKPCIDeviceMatching> highestMatchClass = nil;

	matchData.device = pci_readw(UNPACK_ADDRESS(m_address), kDeviceID);
	matchData.vendor = pci_readw(UNPACK_ADDRESS(m_address), kVendorID);
	matchData.class = pci_readb(UNPACK_ADDRESS(m_address), kClass);
	matchData.subclass = pci_readb(UNPACK_ADDRESS(m_address), kSubclass);
	matchData.prog_if = pci_readb(UNPACK_ADDRESS(m_address), kProgIF);

	LIST_FOREACH (entry, &match_list, qlink) {
		uint8_t result = [entry->matchingClass
		    probeWithMatchData:&matchData];
		if (result > highestMatch) {
			highestMatch = result;
			highestMatchClass = entry->matchingClass;
		}
	}

	if (highestMatchClass != nil) {
		DKDevice<DKPCIDeviceMatching> *dev = [[(id)highestMatchClass alloc]
		    initWithPCIDevice:self];
		[self attachChild:dev onAxis:gDeviceAxis];
		[dev addToStartQueue];
	}
}

- (void)start
{
	uint8_t headerType = pci_readb(UNPACK_ADDRESS(m_address), kHeaderType);
	uint8_t pin = pci_readb(UNPACK_ADDRESS(m_address), kInterruptPin);

#if 0
	kprintf("PCI device at %d.%d.%d.%d; pin %d; has ACPI node %p\n",
	    m_addr.seg, m_addr.bus, m_addr.slot, m_addr.fun, pin, m_prom_node);
#endif

	(void)pin;

	switch (headerType & 0x7f) {
	case 0: {
#if 0
		uint8_t gsi;
		[DKACPIPlatformRoot routePCIPin:pin
				      forBridge:(DKPCIBridge *)[self provider]
					   slot:m_addr.slot
				       function:m_addr.fun
					   into:&gsi];
		kprintf("Regular device at %d.%d.%d.%d; "
			"pin (%d) routes to GSI %d\n",
		    m_addr.seg, m_addr.bus, m_addr.slot, m_addr.fun, pin, gsi);
#endif
		[self match];
		break;
	}

	case 1: {
		uint8_t secondary_bus = pci_readb(UNPACK_ADDRESS(m_address),
		    kSecondaryBus);

#if 0
		if ([[self provider] isKindOfClass:[DKPCI2PCIBridge class]])
			kfatal("loop\n");
		DKPCI2PCIBridge *bridge = [[DKPCI2PCIBridge alloc]
		    initWithUpstreamAddress:m_addr
					bus:secondary_bus
				   promNode:[m_prom_node
						subNodeForPCIAddress:m_addr]];
		[bridge attachToProvider:self onAxis:kDKDeviceAxis];
		[bridge addToStartQueue];
#else
		kprintf("PCI-to-PCI bridge at %d.%d.%d.%d; secondary bus %d\n",
		    UNPACK_ADDRESS(m_address), secondary_bus);
#endif
		break;
	}

	default:
		kprintf("%d.%d.%d.%d: Unknown PCI header type %d\n",
		    UNPACK_ADDRESS(m_address), headerType);
	}
}

- (uint8_t)configRead8:(uint16_t)offset
{
	return pci_readb(UNPACK_ADDRESS(m_address), offset);
}

- (uint16_t)configRead16:(uint16_t)offset
{
	return pci_readw(UNPACK_ADDRESS(m_address), offset);
}

- (uint32_t)configRead32:(uint16_t)offset
{
	return pci_readl(UNPACK_ADDRESS(m_address), offset);
}

- (void)configWrite8:(uint16_t)offset value:(uint8_t)value
{
	pci_writeb(UNPACK_ADDRESS(m_address), offset, value);
}

- (void)configWrite16:(uint16_t)offset value:(uint16_t)value
{
	pci_writew(UNPACK_ADDRESS(m_address), offset, value);
}

- (void)configWrite32:(uint16_t)offset value:(uint32_t)value
{
	pci_writel(UNPACK_ADDRESS(m_address), offset, value);
}

- (DKPCIBarInfo)barInfo:(uint8_t)bar
{
	DKPCIBarInfo info = { 0 };
	uint32_t barOffset = kBAR0 + (bar * 4);

	uint32_t originalBarValue = [self configRead32:barOffset];
	uint32_t barValue = originalBarValue;

	if (barValue & 0x1) {
		info.type = kPCIBarIO;
		info.base = barValue & ~0x3;

		[self configWrite32:barOffset value:0xFFFFFFFF];
		uint32_t sizeValue = [self configRead32:barOffset];
		[self configWrite32:barOffset value:originalBarValue];

		info.size = (~sizeValue + 1) & 0xFFFFFFFC;
	} else {
		bool is64Bit = ((barValue & 0x6) == 0x4);
		uint64_t base = barValue & ~0xF;

		info.type = kPCIBarMem;

		if (is64Bit) {
			uint32_t upperBarOffset = barOffset + 4;
			uint32_t originalBarUpperValue = [self
			    configRead32:upperBarOffset];
			uint32_t barValueUpper = originalBarUpperValue;

			base |= ((uint64_t)barValueUpper << 32);
			info.base = base;

			[self configWrite32:barOffset value:0xFFFFFFFF];
			[self configWrite32:upperBarOffset value:0xFFFFFFFF];

			uint32_t sizeLower = [self configRead32:barOffset];
			uint32_t sizeUpper = [self configRead32:upperBarOffset];
			[self configWrite32:upperBarOffset
				      value:originalBarUpperValue];
			[self configWrite32:barOffset value:originalBarValue];

			info.size = (((uint64_t)(~sizeUpper) << 32) |
			    (~sizeLower)) + 1;
		} else {
			info.base = base;

			[self configWrite32:barOffset value:0xFFFFFFFF];
			uint32_t sizeValue = [self configRead32:barOffset];
			[self configWrite32:barOffset value:originalBarValue];

			info.size = (~sizeValue + 1) & 0xFFFFFFF0;
		}
	}

	return info;
}

@end
