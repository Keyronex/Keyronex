/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Fri Nov 01 2024.
 */

#include <ddk/DKAxis.h>
#include <ddk/DKInterrupt.h>
#include <ddk/DKPCIDevice.h>
#include <ddk/DKPROM.h>
#include <ddk/DKPlatformRoot.h>
#include <ddk/reg/pci.h>
#include <kdk/kmem.h>
#include <kdk/vm.h>

#include "dev/pci/DKPCIBridge.h"
#include "vm/vmp.h"

#define UNPACK_ADDRESS(addr) \
	(addr).segment, (addr).bus, (addr).slot, (addr).function

struct match_entry {
	LIST_ENTRY(match_entry) qlink;
	Class<DKPCIDeviceMatching> matchingClass;
};

static LIST_HEAD(, match_entry) match_list = LIST_HEAD_INITIALIZER(&match_list);
static kmutex_t match_list_lock = KMUTEX_INITIALIZER(match_list_lock);

@implementation DKPCIDevice

@synthesize address = m_address;
@synthesize bridge = m_bridge;

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
		kmem_asprintf(&m_name, "pci-dev-%u:%u", address->slot,
		    address->function);
		m_address = *address;
		m_bridge = bridge;
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

	(void)pin;

	switch (headerType & 0x7f) {
	case 0: {
#if 0
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
		DKPCI2PCIBridge *bridge = [[DKPCI2PCIBridge alloc]
		    initWithPCIDevice:self
				  bus:secondary_bus
			     promNode:[m_bridge.promNode
					  promSubNodeForBridgeAtPCIAddress:
					      m_address]];
		[self attachChild:bridge onAxis:gDeviceAxis];
		[bridge addToStartQueue];
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

- (void)setCommandFlag:(uint16_t)flag enabled:(bool)enabled
{
	uint16_t cmd = [self configRead16:kCommand];
	if (enabled)
		cmd |= flag;
	else
		cmd &= ~flag;
	[self configWrite16:kCommand value:cmd];
}

- (void)setMemorySpace:(bool)enabled
{
	[self setCommandFlag:0x2 enabled:enabled];
}

- (void)setBusMastering:(bool)enabled
{
	[self setCommandFlag:0x4 enabled:enabled];
}

- (void)setInterrupts:(bool)enabled
{
	[self setCommandFlag:0x400 enabled:!enabled];
}

- (dk_interrupt_source_t)interruptSource
{
	dk_interrupt_source_t source;

	[gPlatformRoot routePCIPin:[self configRead8:kInterruptPin]
			 forBridge:m_bridge
			      slot:m_address.slot
			  function:m_address.function
			      into:&source];

	return source;
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

- (uint16_t)findCapabilityByID:(uint8_t)id
{
	return [self findCapabilityByID:id startingOffset:0];
}

- (uint16_t)findCapabilityByID:(uint8_t)id startingOffset:(uint16_t)offset
{
	uint8_t capOffset = offset ? [self configRead8:offset + 1] :
				     [self configRead8:kCapabilitiesPointer];
	while (capOffset != 0) {
		uint8_t capID = [self configRead8:capOffset];
		if (capID == id)
			return capOffset;
		capOffset = [self configRead8:capOffset + 1];
	}
	return 0;
}

- (uint16_t)availableMSIxVectors
{
	uint16_t capOffset = [self findCapabilityByID:0x11];
	uint16_t msixControl;

	if (capOffset == 0)
		return 0;

	msixControl = [self configRead16:capOffset + 0x02];

	return (msixControl & 0x07FF) + 1;
}

- (int)setMSIx:(bool)enabled
{
	uint16_t capOffset = [self findCapabilityByID:0x11];
	uint16_t msixControl;
	uint32_t tableOffset, barNo, tableBase;
	DKPCIBarInfo barInfo;
	int r;

	kassert(enabled);

	if (capOffset == 0) {
		kprintf("MSI-X capability not found\n");
		return -1;
	}

	msixControl = [self configRead16:capOffset + 0x02];
	if (enabled) {
		msixControl |= 0x8000;
		m_msixCap = capOffset;
	} else {
		msixControl &= ~0x8000;
		m_msixCap = 0;
	}

	[self configWrite16:capOffset + 0x02 value:msixControl];

	tableOffset = [self configRead32:capOffset + 0x04];
	barNo = tableOffset & 0x7;
	tableBase = tableOffset & ~0x7;

	barInfo = [self barInfo:barNo];

	r = vm_ps_map_physical_view(&kernel_procstate, &m_msixTable, PGSIZE,
	    barInfo.base + tableBase, kVMRead | kVMWrite, kVMRead | kVMWrite,
	    false);

	return r;
}

- (int)allocateLeastLoadedMSIxInterruptForEntry:(struct intr_entry *)entry
{
	vaddr_t vectorAddressOffset;
	vaddr_t vectorDataOffset;
	vaddr_t vectorControlOffset;
	uint32_t vectorControl;
	uint16_t msixVector;
	uint32_t address;
	uint32_t data;
	int r;

	if (m_lastAllocatedMSIxVector >= [self availableMSIxVectors])
		return -1;
	else
		msixVector = m_lastAllocatedMSIxVector++;

	vectorAddressOffset = m_msixTable + (msixVector * 16);
	vectorDataOffset = m_msixTable + (msixVector * 16) + 8;
	vectorControlOffset = m_msixTable + (msixVector * 16) + 12;

	r = [gPlatformRoot allocateLeastLoadedMSIxInterruptForEntry:entry
							msixAddress:&address
							   msixData:&data];
	if (r != 0)
		return r;

	*(volatile uint32_t *)vectorAddressOffset = address;
	*(volatile uint32_t *)vectorDataOffset = data;
	vectorControl = *(volatile uint32_t *)vectorControlOffset;
	vectorControl &= ~0x1;
	__sync_synchronize();
	*(volatile uint32_t *)vectorControlOffset = vectorControl;

	return 0;
}

#pragma region MSI

- (uint8_t)availableMSIVectors
{
	uint16_t capOffset = [self findCapabilityByID:0x05];
	uint16_t msiControl;
	uint8_t mmc;

	if (capOffset == 0)
		return 0;

	msiControl = [self configRead16:capOffset + 0x02];
	mmc = (msiControl >> 1) & 0x7;

	return (1 << mmc);
}

- (int)allocateLeastLoadedMSIInterruptsForEntries:(struct intr_entry *)entries
					    count:(uint8_t)count
{
	uint16_t capOffset = [self findCapabilityByID:0x05];
	uint16_t msiControl;
	uint32_t msiAddress, msiData;
	uint8_t mme;
	bool is64;
	int r;

	if (capOffset == 0) {
		kprintf("MSI capability not found\n");
		return -1;
	}

	msiControl = [self configRead16:capOffset + 0x02];

	if (count > [self availableMSIVectors])
		kfatal("Requested MSI count %d exceeds available %d\n", count,
		    [self availableMSIVectors]);

	mme = 0;
	while ((1 << mme) < count)
		mme++;

	msiControl &= ~((0x7 << 4) | 0x1);
	msiControl |= (mme << 4) | 0x1;

	m_msiCap = capOffset;
	m_msiCount = count;

	[self configWrite16:capOffset + 0x02 value:msiControl];

	is64 = (msiControl & (1 << 7)) != 0;

	r = [gPlatformRoot allocateLeastLoadedMSIInterruptForEntries:entries
							       count:count
							  msiAddress:&msiAddress
							     msiData:&msiData];
	if (r != 0)
		return r;

	[self configWrite32:capOffset + 0x04 value:msiAddress];
	if (is64) {
		[self configWrite32:capOffset + 0x08 value:0];
		[self configWrite16:capOffset + 0x0c value:(uint16_t)msiData];
	} else {
		[self configWrite16:capOffset + 0x08 value:(uint16_t)msiData];
	}

	return 0;
}

@end
