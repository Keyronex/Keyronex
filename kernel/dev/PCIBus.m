#include "PCIBus.h"
#include "dev/E1000.h"
#include "dev/virtio/DKVirtIOPCITransport.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "ntcompat/NTStorPort.h"

#define INFO_ARGS(INFO) (INFO)->seg, (INFO)->bus, (INFO)->slot, (INFO)->fun
#define ENABLE_CMD_FLAG(INFO, FLAG)           \
	pci_writew(INFO_ARGS(INFO), kCommand, \
	    pci_readw(INFO_ARGS(INFO), kCommand) | (FLAG))
#define DISABLE_CMD_FLAG(INFO, FLAG)          \
	pci_writew(INFO_ARGS(INFO), kCommand, \
	    pci_readw(INFO_ARGS(INFO), kCommand) & ~(FLAG))

#if !defined(__m68k__)

@implementation PCIBus

static paddr_t ecam_base = -1;

+ (void)setECAMBase:(paddr_t)base
{
	ecam_base = base;
}

+ (paddr_t)getECAMBaseForSegment:(uint16_t)seg bus:(uint8_t)bus
{
	kassert(ecam_base != -1);
	return ecam_base + (seg << 20) + (bus << 15);
}

+ (void)enableBusMasteringForInfo:(struct pci_dev_info *)info
{
	ENABLE_CMD_FLAG(info, 0x4);
}

+ (void)setMemorySpaceForInfo:(struct pci_dev_info *)info enabled:(BOOL)enabled
{
	if (!enabled)
		DISABLE_CMD_FLAG(info, (1 << 1));
	else
		ENABLE_CMD_FLAG(info, (1 << 1));
}

+ (void)enumerateCapabilitiesForInfo:(struct pci_dev_info *)info
			    delegate:(DKDevice<DKPCIDeviceDelegate> *)delegate
{
	if (PCIINFO_CFG_READ(w, info, kStatus) & (1 << 4)) {
		voff_t capOffset = PCIINFO_CFG_READ(b, info,
		    kCapabilitiesPointer);

		while (capOffset != 0) {
			[delegate capabilityEnumeratedAtOffset:capOffset];
			capOffset = PCIINFO_CFG_READ(b, info, capOffset + 1);
		}
	}
}

+ (paddr_t)getBar:(int)num forInfo:(struct pci_dev_info *)info
{
	io_off_t off = kBaseAddress0 + sizeof(uint32_t) * num;
	uint32_t bar;

	uint64_t base;
	size_t len;

	bar = pci_readl(INFO_ARGS(info), off);

	if ((bar & 1) == 1) {
		kprintf("I/O space bar\n");
		return 0;
	} else if (((bar >> 1) & 3) == 0) {
		uint32_t size_mask;

		pci_writel(INFO_ARGS(info), off, 0xffffffff);
		size_mask = pci_readl(INFO_ARGS(info), off);
		pci_writel(INFO_ARGS(info), off, bar);

		base = bar & 0xffffffF0;
		len = (size_t)1 << __builtin_ctzl(size_mask & 0xffffffF0);

	} else {
		uint64_t size_mask, bar_high, size_mask_high;

		kassert(((bar >> 1) & 3) == 2);

		bar_high = pci_readl(INFO_ARGS(info), off + 4);
		base = (bar & 0xffffffF0) | (bar_high << 32);

		pci_writel(INFO_ARGS(info), off, 0xffffffff);
		pci_writel(INFO_ARGS(info), off + 4, 0xffffffff);
		size_mask = pci_readl(INFO_ARGS(info), off);
		size_mask_high = pci_readl(INFO_ARGS(info), off + 4);
		pci_writel(INFO_ARGS(info), off, bar);
		pci_writel(INFO_ARGS(info), off + 4, bar_high);

		size_mask |= size_mask_high << 32;
		len = (size_t)1
		    << __builtin_ctzl(size_mask & 0xffffffffffffffF0);

		(void)len;
	}

	return (paddr_t)base;
}

+ (void)setInterruptsEnabled:(BOOL)enabled forInfo:(struct pci_dev_info *)info
{
	if (enabled)
		DISABLE_CMD_FLAG(info, (1 << 10));
	else
		ENABLE_CMD_FLAG(info, (1 << 10));
}

static BOOL
match_e1000(uint16_t vendorId, uint16_t devId)
{
	if (vendorId != 0x8086)
		return NO;

	switch (devId) {
	case 0x100c: /* 82544GC */
	case 0x100e: /* 82540EM */
	case 0x100f: /* 82545EM */
	case 0x10d3: /* 82574L */
	case 0x10c9: /* 82576 - qemu igb */
	case 0x15bd: /* I219-LM */
	case 0x15d7: /* I219-LM */
	case 0x15fa: /* I219-V */
		return YES;

	default:
		return NO;
	}
}

- (void)doSegment:(uint16_t)seg
	      bus:(uint8_t)bus
	     slot:(uint8_t)slot
	 function:(uint8_t)fun
{
	struct pci_dev_info info;
	uint8_t pin;

#if defined(__aarch64__) || defined(__amd64__) || defined(__riscv)
#define CFG_READ(WIDTH, OFFSET) \
	pci_read##WIDTH(seg, bus, slot, fun, OFFSET)

	info.vendorId = CFG_READ(w, kVendorID);
	if (info.vendorId == 0xffff)
		return;
	info.deviceId = CFG_READ(w, kDeviceID);
	info.klass = CFG_READ(b, kClass);
	info.subClass = CFG_READ(b, kSubclass);

	info.seg = seg;
	info.bus = bus;
	info.slot = slot;
	info.fun = fun;
	info.pin = CFG_READ(b, kInterruptPin);

	if (info.pin != 0)
		info.intx_source = [self routePCIPinForInfo:&info];

#endif

#if 0
	kprintf("%x:%x (klass %x subclass %x)\n", info.vendorId, info.deviceId,
	    info.klass, info.subClass);
#endif

	BOOL handled;
	handled = [NTStorPort probeWithPCIBus:self info:&info];
	if (!handled && info.vendorId == 0x1af4)
		handled = [DKVirtIOPCITransport probeWithPCIBus:self
							   info:&info];
	else if (!handled && match_e1000(info.vendorId, info.deviceId))
		handled = [E1000 probeWithPCIBus:self info:&info];
	if (!handled)
		kprintf("No driver for PCI device %d.%d.%d (%x:%x)\n", info.bus,
		    info.slot, info.fun, info.vendorId, info.deviceId);
}

- (void)enumerateDevices
{
	for (unsigned slot = 0; slot < 32; slot++) {
		size_t nFun = 1;

		if (pci_readw(m_seg, m_bus, slot, 0, kVendorID) == 0xffff)
			continue;

		if (pci_readb(m_seg, m_bus, slot, 0, kHeaderType) & (1 << 7))
			nFun = 8;

		for (unsigned fun = 0; fun < nFun; fun++)
			[self doSegment:m_seg bus:m_bus slot:slot function:fun];
	}
}

- (dk_interrupt_source_t)routePCIPinForInfo:(struct pci_dev_info *)info
{
	kfatal("Subclass responsibility\n");
}

@end
#endif
