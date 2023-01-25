#include <dev/VirtIOFamily/VirtIOBlockDevice.h>
#include <dev/VirtIOFamily/VirtIONetwork.h>
#include <kern/kmem.h>

#include "acpispec/resources.h"
#include "dev/ACPIPC.h"
#include "dev/IOApic.h"
#include "dev/PCIBus.h"
#include "dev/laiex.h"
#include "lai/core.h"
#include "lai/error.h"
#include "lai/helpers/pci.h"
#include "lai/host.h"

enum {
	kVendorID = 0x0, /* u16 */
	kDeviceID = 0x2, /* u16 */
	kCommand = 0x4,	 /* u16 */
	kStatus = 0x6,	 /* bit 4 = capabilities list exists */
	kSubclass = 0xa,
	kClass = 0xb,
	kHeaderType = 0xe, /* bit 7 = is multifunction */
	kBaseAddress0 = 0x10,
	kCapabilitiesPointer = 0x34,
	kInterruptPin = 0x3d, /* u8 */
};

enum {
	kCapMSIx = 0x11,
};

static int
laiex_eval_one_int(lai_nsnode_t *node, const char *path, uint64_t *out,
    lai_state_t *state)
{
	LAI_CLEANUP_VAR lai_variable_t var = LAI_VAR_INITIALIZER;
	lai_nsnode_t		      *handle;
	int			       r;

	handle = lai_resolve_path(node, path);
	if (handle == NULL)
		return LAI_ERROR_NO_SUCH_NODE;

	r = lai_eval(&var, handle, state);
	if (r != LAI_ERROR_NONE)
		return r;

	return lai_obj_get_integer(&var, out);
}

#if 0
static void
iterate(lai_nsnode_t *obj, size_t depth)
{
	struct lai_ns_child_iterator iterator =
	    LAI_NS_CHILD_ITERATOR_INITIALIZER(obj);
	lai_nsnode_t *node;

	while ((node = lai_ns_child_iterate(&iterator))) {
		for (int i = 0; i < depth; i++)
			kprintf(" ");
		kprintf("NAME: %s\n", node->name);
		iterate(node, depth + 2);
	}
}
#endif

@implementation PCIBus

#define INFO_ARGS(INFO) INFO->seg, INFO->bus, INFO->slot, INFO->fun
#define ENABLE_CMD_FLAG(INFO, FLAG)                   \
	laihost_pci_writew(INFO_ARGS(INFO), kCommand, \
	    laihost_pci_readw(INFO_ARGS(INFO), kCommand) | (FLAG))
#define DISABLE_CMD_FLAG(INFO, FLAG)                  \
	laihost_pci_writew(INFO_ARGS(INFO), kCommand, \
	    laihost_pci_readw(INFO_ARGS(INFO), kCommand) & ~(FLAG))

+ (int)handleInterruptOf:(dk_device_pci_info_t *)pciInfo
	     withHandler:(intr_handler_fn_t)handler
		argument:(void *)arg
	      atPriority:(ipl_t)priority
{
	acpi_resource_t res;
	int		r;

	r = lai_pci_route_pin(&res, INFO_ARGS(pciInfo), pciInfo->pin);
	if (r != LAI_ERROR_NONE)
		return -r;

	// TODO: res.irq_flags & ACPI_SMALL_IRQ_EDGE_TRIGGERED

	r = [IOApic handleGSI:res.base
		  withHandler:handler
		     argument:arg
		  lowPolarity:res.irq_flags & ACPI_SMALL_IRQ_ACTIVE_LOW
		   atPriority:priority];
	if (r < 0)
		return r;

	return 0;
}

+ (void)enableMemorySpace:(dk_device_pci_info_t *)pciInfo
{
	ENABLE_CMD_FLAG(pciInfo, 0x1 | 0x2);
}

+ (void)enableBusMastering:(dk_device_pci_info_t *)pciInfo
{
	ENABLE_CMD_FLAG(pciInfo, 0x4);
}

+ (void)setInterruptsOf:(dk_device_pci_info_t *)pciInfo enabled:(BOOL)enabled
{
	if (enabled)
		DISABLE_CMD_FLAG(pciInfo, (1 << 10));
	else
		ENABLE_CMD_FLAG(pciInfo, (1 << 10));
}

+ (paddr_t)getBar:(uint8_t)num info:(dk_device_pci_info_t *)pciInfo
{
	return (paddr_t)((uintptr_t)laihost_pci_readd(INFO_ARGS(pciInfo),
			     kBaseAddress0 + sizeof(uint32_t) * num) &
	    0xfffffff0);
}

+ (BOOL)probeWithAcpiNode:(lai_nsnode_t *)node provider:(DKDevice *)provider;
{
	uint64_t		      seg = -1, bbn = -1;
	LAI_CLEANUP_STATE lai_state_t state;
	int			      r;

	lai_init_state(&state);

	r = laiex_eval_one_int(node, "_SEG", &seg, &state);
	if (r != LAI_ERROR_NONE) {
		if (r == LAI_ERROR_NO_SUCH_NODE) {
			seg = 0;
		} else {
			DKLog("PCIBus", "failed to evaluate _SEG: %d\n", r);
			return NO;
		}
	}

	r = laiex_eval_one_int(node, "_BBN", &bbn, &state);
	if (r != LAI_ERROR_NONE) {
		if (r == LAI_ERROR_NO_SUCH_NODE) {
			bbn = 0;
		} else {
			DKLog("PCIBus", "failed to evaluate _BBN: %d\n", r);
		}
	}

	[[self alloc] initWithSeg:seg bus:bbn provider:provider];

	return YES;
}

+ (void)enumerateCapabilitiesOf:(dk_device_pci_info_t *)pciInfo
		   withCallback:(void (*)(dk_device_pci_info_t *, voff_t cap,
				    void *))callback
		       userData:(void *)userData
{
	if (PCIINFO_CFG_READ(w, pciInfo, kStatus) & (1 << 4)) {
		voff_t pCap = PCIINFO_CFG_READ(b, pciInfo,
		    kCapabilitiesPointer);

		while (pCap != 0) {
			callback(pciInfo, pCap, userData);
			pCap = PCIINFO_CFG_READ(b, pciInfo, pCap + 1);
		}
	}
}

static void
doCapability(dk_device_pci_info_t *pciInfo, voff_t pCap)
{
	uint8_t cap = laihost_pci_readw(INFO_ARGS(pciInfo), pCap);

	switch (cap) {
	case kCapMSIx:
		DKDevLog(pciInfo->busObj, "Supports MSI-x\n");
		break;
	}
}

static void
doFunction(PCIBus *bus, uint16_t seg, uint8_t busNum, uint8_t slot, uint8_t fun)
{
	dk_device_pci_info_t pciInfo;
	uint8_t class, subClass;
	uint16_t vendorId, deviceId;

#define CFG_READ(WIDTH, OFFSET) \
	laihost_pci_read##WIDTH(seg, busNum, slot, fun, OFFSET)

	vendorId = CFG_READ(w, kVendorID);
	if (vendorId == 0xffff)
		return;
	deviceId = CFG_READ(w, kDeviceID);
	class = CFG_READ(b, kClass);
	subClass = CFG_READ(b, kSubclass);

	pciInfo.busObj = bus;
	pciInfo.seg = seg;
	pciInfo.bus = busNum;
	pciInfo.slot = slot;
	pciInfo.fun = fun;
	pciInfo.pin = CFG_READ(b, kInterruptPin);

	DKDevLog(bus,
	    "Function at %d:%d:%d:%d: "
	    "Vendor %x, device %x, class %d/%d, pin %d\n",
	    seg, busNum, slot, fun, vendorId, deviceId, class, subClass,
	    pciInfo.pin);

	if (CFG_READ(w, kStatus) & (1 << 4)) {
		voff_t pCap = CFG_READ(b, kCapabilitiesPointer);

		while (pCap != 0) {
			doCapability(&pciInfo, pCap);
			pCap = CFG_READ(b, pCap + 1);
		}
	}

	if (class == 6 && subClass == 4) {
		DKDevLog(bus, "FIXME: PCI-PCI Bridge\n");
		return;
	} else if (class == 1 && subClass == 8) {
		//[NVMeController probeWithPCIInfo:&pciInfo];
		return;
	} else if (vendorId == 0x1af4 && deviceId == 0x1001) {
		//[VirtIOBlockDevice probeWithPCIInfo:&pciInfo];
	} else if (vendorId == 0x1af4 && deviceId == 0x1000) {
		[VirtIONetwork probeWithPCIInfo:&pciInfo];
	}
}

- initWithSeg:(uint8_t)seg bus:(uint8_t)bus provider:(DKDevice *)provider
{
	self = [super initWithProvider:provider];

	kmem_asprintf(&m_name, "PCIBus%d@%d", seg, bus);
	[self registerDevice];
	DKLogAttach(self);

	for (int slot = 0; slot < 32; slot++) {
		size_t cntFun = 1;

		if (laihost_pci_readw(seg, bus, slot, 0, kVendorID) == 0xffff)
			continue;

		if (laihost_pci_readb(seg, bus, slot, 0, kHeaderType) &
		    (1 << 7))
			cntFun = 8;

		for (int fun = 0; fun < cntFun; fun++)
			doFunction(self, seg, bus, slot, fun);
	}

	return self;
}

@end
