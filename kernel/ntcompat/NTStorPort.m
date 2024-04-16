
#include "NTStorPort.h"
#include "dev/PCIBus.h"
#include "dev/amd64/IOAPIC.h"
#include "dev/pci_reg.h"
#include "kdk/dev.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/nanokern.h"
#include "kdk/object.h"
#include "kdk/queue.h"
#include "kdk/vm.h"
#include "ntcompat/ntcompat.h"
#include "ntcompat/storport.h"
#include "ntcompat/storportcompat.h"
#include "ntcompat/win_types.h"
#include "vm/vmp.h"

#if defined(__arch64__) || defined (__amd64__)
#include "lai/host.h"
#endif

#define INFO_ARGS(PINFO) (PINFO)->seg, (PINFO)->bus, (PINFO)->slot, (PINFO)->fun

struct storport_driver_queue drivers = TAILQ_HEAD_INITIALIZER(drivers);
void *wegot;

static inline void
packPci(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun,
    ULONG *SystemIoBusNumber, ULONG *SlotNumber)
{
	*SystemIoBusNumber = ((uint32_t)seg & 0xFFFF) << 16 |
	    ((uint32_t)bus & 0xFF) << 8 | ((uint32_t)slot & 0xFF);
	*SlotNumber = ((uint32_t)fun & 0xFF) << 24;
}

void
unpackPci(uint32_t SystemIoBusNumber, uint32_t SlotNumber, uint16_t *seg,
    uint8_t *bus, uint8_t *slot, uint8_t *fun)
{
	*seg = (SystemIoBusNumber >> 16) & 0xFFFF;
	*bus = (SystemIoBusNumber >> 8) & 0xFF;
	*slot = SystemIoBusNumber & 0xFF;
	*fun = (SlotNumber >> 24) & 0xFF;
}

static PACCESS_RANGE
getAccessRanges(struct pci_dev_info *info)
{
	PACCESS_RANGE accessranges = (PACCESS_RANGE)kmem_alloc(
	    sizeof(ACCESS_RANGE) * 7);

#if defined(__arch64__) || defined (__amd64__)
	for (size_t i = 0; i < 6; i++) {
		size_t off = kBaseAddress0 + sizeof(uint32_t) * i;
		uint64_t base;
		size_t len;
		uint32_t bar;

		bar = laihost_pci_readd(INFO_ARGS(info), off);

		if ((bar & 1) == 1) {
			accessranges[i].RangeStart.QuadPart = bar & 0xFFFFFFFC;
			accessranges[i].RangeLength = 0;
			accessranges[i].RangeInMemory = false;
		} else if (((bar >> 1) & 3) == 0) {
			uint32_t size_mask;

			laihost_pci_writed(INFO_ARGS(info), off, 0xffffffff);
			size_mask = laihost_pci_readd(INFO_ARGS(info), off);
			laihost_pci_writed(INFO_ARGS(info), off, bar);

			base = bar & 0xffffffF0;
			len = (size_t)1
			    << __builtin_ctzl(size_mask & 0xffffffF0);

			accessranges[i].RangeStart.QuadPart = base;
			accessranges[i].RangeLength = len;
			accessranges[i].RangeInMemory = true;
		} else {
			uint64_t size_mask, bar_high, size_mask_high;

			kassert(((bar >> 1) & 3) == 2);

			bar_high = laihost_pci_readd(INFO_ARGS(info), off + 4);
			base = (bar & 0xffffffF0) | (bar_high << 32);

			laihost_pci_writed(INFO_ARGS(info), off, 0xffffffff);
			laihost_pci_writed(INFO_ARGS(info), off + 4,
			    0xffffffff);
			size_mask = laihost_pci_readd(INFO_ARGS(info), off);
			size_mask_high = laihost_pci_readd(INFO_ARGS(info),
			    off + 4);
			laihost_pci_writed(INFO_ARGS(info), off, bar);
			laihost_pci_writed(INFO_ARGS(info), off + 4, bar_high);

			size_mask |= size_mask_high << 32;
			len = (size_t)1
			    << __builtin_ctzl(size_mask & 0xffffffffffffffF0);

			accessranges[i].RangeStart.QuadPart = base;
			accessranges[i].RangeLength = len;
			accessranges[i].RangeInMemory = true;
		}
	}
#endif

	return accessranges;
}

@interface
NTStorPort (Implementation)
- (bool)executeIOP:(iop_t *)iop;
- (void)iterate;
@end

@implementation NTStorPort

+ (BOOL)probeWithPCIBus:(PCIBus *)provider info:(struct pci_dev_info *)info
{
	struct storport_driver *driver = NULL;
	PACCESS_RANGE ranges = NULL;
	PPORT_CONFIGURATION_INFORMATION pcfg = NULL;

	TAILQ_FOREACH (driver, &drivers, queue_entry) {
		struct sp_dev_ext *deviceExtension;
		PVOID HwDeviceExtension;
		bool matched = false;
		struct pci_match *match = driver->nt_driver_object->matches;

		while (match->vendor_id != 0) {
			if (info->vendorId == match->vendor_id &&
			    info->deviceId == match->device_id) {
				matched = true;
				break;
			}
			match++;
		}

		if (!matched)
			continue;

#if defined(__arch64__) || defined (__amd64__)
		laihost_pci_writew(INFO_ARGS(info), kCommand,
		    laihost_pci_readw(INFO_ARGS(info), kCommand) &
			~(0x1 | 0x2));
#endif

		if (pcfg == NULL)
			pcfg = kmem_alloc(
			    sizeof(PORT_CONFIGURATION_INFORMATION));
		memset(pcfg, 0x0, sizeof(PORT_CONFIGURATION_INFORMATION));

		if (ranges == NULL)
			ranges = getAccessRanges(info);

		pcfg->AccessRanges = (ACCESS_RANGE(*)[])ranges;
		pcfg->NumberOfAccessRanges = 6;
		pcfg->AdapterInterfaceType = kPCIBus;
		packPci(info->seg, info->bus, info->slot, info->fun,
		    &pcfg->SystemIoBusNumber, &pcfg->SlotNumber);

#if defined(__arch64__) || defined (__amd64__)
		laihost_pci_writew(INFO_ARGS(info), kCommand,
		    laihost_pci_readw(INFO_ARGS(info), kCommand) | (0x1 | 0x2));
#endif

		deviceExtension = kmem_alloc(
		    driver->hwinit.DeviceExtensionSize +
		    sizeof(struct sp_dev_ext));
		deviceExtension->driver = driver;
		deviceExtension->portConfig = pcfg;
		ke_spinlock_init(&deviceExtension->intxLock);

		HwDeviceExtension = &deviceExtension->hw_dev_ext[0];

		DKDevLog(self,
		    "Matching driver %s found; calling HwFindAdapter\n",
		    driver->nt_driver_object->name);
		BOOLEAN again;
		ULONG ret;

		ret = driver->hwinit.HwFindAdapter(HwDeviceExtension, NULL,
		    NULL, NULL, pcfg, &again);
		kassert(ret == SP_RETURN_FOUND);

		DKDevLog(self,
		    "HwFindAdapter succeeded; instantiating StorPort device\n");

		[[self alloc] initWithPCIBus:provider
					info:info
			      storportDriver:driver
			     deviceExtension:deviceExtension];

		return YES;
	}

	return NO;
}

static bool
intx_handler(md_intr_frame_t *frame, void *arg)
{
	struct sp_dev_ext *devExt = arg;
	// kprintf("Begin handling it.\n");
	devExt->driver->hwinit.HwInterrupt(devExt->hw_dev_ext);
	// kprintf("Done handling it.\n");
	return true;
}

- (instancetype)initWithPCIBus:(PCIBus *)provider
			  info:(struct pci_dev_info *)info
		storportDriver:(struct storport_driver *)driver
	       deviceExtension:(struct sp_dev_ext *)devExt;
{
	int r;

	self = [super initWithProvider:provider];

	m_info = *info;
	m_deviceExtension = devExt;
	m_HwDeviceExtension = &devExt->hw_dev_ext[0];

	// for (;;)
	//     ;

	kmem_asprintf(obj_name_ptr(self), "%s-%lu",
	    driver->nt_driver_object->name, driver->counter);

	m_deviceExtension->device = self;

	DKDevLog(self, "Calling HwInitialize\n");
	BOOLEAN suc = driver->hwinit.HwInitialize(m_HwDeviceExtension);

#ifdef __amd64
	r = [IOApic handleGSI:info->gsi
		  withHandler:intx_handler
		     argument:m_deviceExtension
		isLowPolarity:info->lopol
	      isEdgeTriggered:info->edge
		   atPriority:kIPLHigh
			entry:&m_intxEntry];
#else
	r = -1;
#endif
	kassert(r == 0);

	if (devExt->passive_init != NULL) {
		DKDevLog(self, "Calling HwPassiveInitializeRoutine\n");
		devExt->passive_init(m_HwDeviceExtension);
	}
	DKDevLog(self, "Calling HwUnitControl\n");

	DKDevLog(self, "Probing SCSI bus\n");
	[self iterate];

	if (suc)
		DKDevLog(self, "StorPort device successfully initialised\n");

	[self registerDevice];
	DKLogAttach(self);

	return self;
}

static PSCSI_REQUEST_BLOCK
srb_alloc(void)
{
	return kmem_alloc(sizeof(SCSI_REQUEST_BLOCK));
}

static void
srb_init_execute_scsi(PSCSI_REQUEST_BLOCK srb, int direction, uint8_t pathId,
    uint8_t targetId, uint8_t lun, size_t cdbLength, void *buffer,
    size_t bufferLength)
{
	memset(srb, 0x0, sizeof(SCSI_REQUEST_BLOCK));

	srb->PathId = pathId;
	srb->TargetId = targetId;
	srb->Lun = lun;
	srb->SrbFlags = direction;

	srb->Length = sizeof(SCSI_REQUEST_BLOCK);
	srb->Function = SRB_FUNCTION_EXECUTE_SCSI;
	srb->CdbLength = cdbLength;

	srb->DataBuffer = buffer;
	srb->DataTransferLength = bufferLength;
}

static void
trimstr(char *str, size_t length)
{
	while (length > 0 && (str[length - 1] == ' ')) {
		length--;
	}

	str[length] = '\0';
}

- (void)iterate
{
	void *buffer = kmem_alloc(512);
	struct _LUN_LIST *list = buffer;
	struct _REPORT_LUNS *cdb;
	PSCSI_REQUEST_BLOCK srb;
	iop_t *iop;
	iop_return_t ret;

	srb = srb_alloc();
	iop = iop_new(self);

	for (size_t path = 0;
	     path <  1/* m_deviceExtension->portConfig->NumberOfBuses */; path++) {
		for (size_t target = 0; target < 1
		     /*m_deviceExtension->portConfig->MaximumNumberOfTargets */;
		     target++) {
			srb_init_execute_scsi(srb, SRB_FLAGS_DATA_IN, path,
			    target, 0, sizeof(struct _REPORT_LUNS), buffer,
			    512);
			cdb = (void *)srb->Cdb;
			cdb->OperationCode = SCSIOP_REPORT_LUNS;
			*(uint32_t *)cdb->AllocationLength = __builtin_bswap32(
			    512);

			*(uint32_t *)list->LunListLength = 0x0;

			iop_init_scsi(iop, self, srb);
			ret = iop_send_sync(iop);
			kassert(ret == kIOPRetCompleted);

			if (srb->SrbStatus != SRB_STATUS_SUCCESS &&
			    !(srb->SrbStatus == SRB_STATUS_DATA_OVERRUN &&
				srb->DataTransferLength >=
				    sizeof(struct _LUN_LIST))) {
				 kfatal(
				     "Target %zu: SRB status is %x\n", target,
				     srb->SrbStatus
				);
				continue;
			}

			uint32_t length = __builtin_bswap32(*(
					      uint32_t *)list->LunListLength) /
			    8;
			for (size_t lunIdx = 0; lunIdx < length; lunIdx++) {
				uint64_t lun = __builtin_bswap64(
				    *(uint64_t *)list->Lun[lunIdx]);
				struct _CDB6INQUIRY *cdb;
				INQUIRYDATA *data = kmem_alloc(sizeof(*data));

				srb_init_execute_scsi(srb, SRB_FLAGS_DATA_IN,
				    path, target, lun,
				    sizeof(struct _CDB6INQUIRY), data,
				    sizeof(*data));

				cdb = (void *)srb->Cdb;
				cdb->OperationCode = SCSIOP_INQUIRY;
				cdb->LogicalUnitNumber = lunIdx;
				cdb->Control = 0;
				cdb->AllocationLength = sizeof(*data);

				iop_init_scsi(iop, self, srb);
				ret = iop_send_sync(iop);
				kassert(ret == kIOPRetCompleted);

				if (srb->SrbStatus != SRB_STATUS_SUCCESS) {
					kfatal("Srb not successful!\n");
					continue;
				}

				trimstr(data->VendorId, sizeof(data->VendorId));
				trimstr(data->ProductId,
				    sizeof(data->ProductId));
				trimstr((char *)data->ProductRevisionLevel,
				    sizeof(data->ProductRevisionLevel));

				DKDevLog(self,
				    "<%.8s %.16s %.4s> at bus %zu target %zu lun %llu\n",
				    data->VendorId, data->ProductId,
				    data->ProductRevisionLevel, path, target,
				    lun);
			}
		}
	}
}

- (iop_return_t)dispatchIOP:(iop_t *)iop
{
	[self executeIOP:iop];
	return kIOPRetPending;
}

- (bool)executeIOP:(iop_t *)iop
{
	iop_frame_t *frame = iop_stack_current(iop);
	PSCSI_REQUEST_BLOCK srb = frame->scsi.srb;
	struct storport_driver *drv = m_deviceExtension->driver;
	BOOLEAN r;

	kassert(frame->function = kIOPTypeSCSI);

	srb->SrbStatus = SRB_STATUS_PENDING;
	srb->SrbExtension = kmem_alloc(drv->hwinit.SrbExtensionSize);
	srb->OriginalRequest = iop;

	r = drv->hwinit.HwBuildIo(m_HwDeviceExtension, srb);
	kassert(r == TRUE);
	r = drv->hwinit.HwStartIo(m_HwDeviceExtension, srb);
	kassert(r == TRUE);
	return true;
}

- (void)completeSrb:(PSCSI_REQUEST_BLOCK)Srb
{
	struct storport_driver *drv = m_deviceExtension->driver;
	kmem_free(Srb->SrbExtension, drv->hwinit.SrbExtensionSize);
	iop_continue(Srb->OriginalRequest, kIOPRetCompleted);
}

@end
