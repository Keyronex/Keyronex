#include "dev/pci/DKPCIBus.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "storportcompat.h"
#include "vm/vmp.h"

void unpackPci(uint32_t SystemIoBusNumber, uint32_t SlotNumber, uint16_t *seg,
    uint8_t *bus, uint8_t *slot, uint8_t *fun);

static void
dpc_callback(void *arg)
{
	struct dpc_alias *alias = arg;
	alias->dpcfunc(arg, alias->devext, alias->sysarg1, alias->sysarg2);
}

STORPORT_API
BOOLEAN
NTAPI
StorPortBusy(
  _In_ PVOID HwDeviceExtension,
  _In_ ULONG RequestsToComplete)
{
	kfatal("Implement me!\n");
}

STORPORT_API
ULONG
NTAPI
StorPortInitialize(
	_In_ PVOID Argument1,
	_In_ PVOID Argument2,
	_In_ PHW_INITIALIZATION_DATA HwInitializationData,
	_In_opt_ PVOID Unused
)
{
	struct storport_driver *driver = kmem_alloc(sizeof(struct storport_driver));
	driver->nt_driver_object = Argument1;
	driver->counter = 0;
	driver->hwinit = *HwInitializationData;
	TAILQ_INSERT_TAIL(&drivers, driver, queue_entry);
	return 0;
}

STORPORT_API
VOID __cdecl StorPortDebugPrint(
	_In_ ULONG DebugPrintLevel,
	_In_ PCCHAR DebugMessage,
	...
)
{
	__builtin_ms_va_list ap;
	__builtin_ms_va_start(ap, DebugMessage);
	npf_msvpprintf(kputc, NULL, DebugMessage, ap);
	__builtin_ms_va_end(ap);
}

STORPORT_API
BOOLEAN
NTAPI
StorPortDeviceBusy(
	_In_ PVOID HwDeviceExtension,
	_In_ UCHAR PathId,
	_In_ UCHAR TargetId,
	_In_ UCHAR Lun,
	_In_ ULONG RequestsToComplete
)
{
	kfatal("StorPortDeviceBusy\n");
	return TRUE;
}

STORPORT_API
ULONG
NTAPI
StorPortGetBusData(
	_In_ PVOID DeviceExtension,
	_In_ ULONG BusDataType,
	_In_ ULONG SystemIoBusNumber,
	_In_ ULONG SlotNumber,
	_Out_ _When_(Length != 0, _Out_writes_bytes_(Length)) PVOID Buffer,
	_In_ ULONG Length
)
{
	uint8_t *out = Buffer;
	uint16_t seg;
	uint8_t bus, slot, fun;
	unpackPci(SystemIoBusNumber, SlotNumber, &seg, &bus, &slot, &fun);
#if defined(__aarch64__) || defined(__amd64__) || defined(__riscv)
	for (int i = 0; i < Length; i++)
		out[i] = pci_readb(seg, bus, slot, fun, i);
#else
	(void)out;
	kfatal("Implement me\n");
#endif
	return Length;
}

STORPORT_API
PVOID
NTAPI
StorPortGetDeviceBase(
	_In_ PVOID HwDeviceExtension,
	_In_ INTERFACE_TYPE BusType,
	_In_ ULONG SystemIoBusNumber,
	_In_ STOR_PHYSICAL_ADDRESS IoAddress,
	_In_ ULONG NumberOfBytes,
	_In_ BOOLEAN InIoSpace
)
{
	return (PVOID)P2V(IoAddress.QuadPart);
}

STORPORT_API
PVOID
NTAPI
StorPortGetUncachedExtension(
	_In_ PVOID HwDeviceExtension,
	_In_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
	_In_ ULONG NumberOfBytes
)
{
	vm_page_t *page;
	int r;
	size_t npages = ROUNDUP(NumberOfBytes, PGSIZE) / PGSIZE;
	r = vm_page_alloc(
		&page,
		vm_npages_to_order(npages),
		kPageUseKWired,
		true
	);
	kassert(r == 0);
	return (PVOID)vm_page_direct_map_addr(page);
}

STORPORT_API
VOID NTAPI
StorPortLogError(
	_In_ PVOID HwDeviceExtension,
	_In_opt_ PSCSI_REQUEST_BLOCK Srb,
	_In_ UCHAR PathId,
	_In_ UCHAR TargetId,
	_In_ UCHAR Lun,
	_In_ ULONG ErrorCode,
	_In_ ULONG UniqueId
)
{
	kfatal("StorPortLogError\n");
}

STORPORT_API
BOOLEAN
NTAPI
StorPortSetDeviceQueueDepth(
	_In_ PVOID HwDeviceExtension,
	_In_ UCHAR PathId,
	_In_ UCHAR TargetId,
	_In_ UCHAR Lun,
	_In_ ULONG Depth
)
{
	kprintf("StorPortSetDeviceQueueDepth\n");
	return TRUE;
}

STORPORT_API
VOID NTAPI
StorPortStallExecution(_In_ ULONG Delay)
{
	for (size_t i = 0; i < Delay * 10; i++) ;
		//asm("pause");
}

STORPORT_API
VOID
NTAPI
StorPortSynchronizeAccess(
	_In_ PVOID HwDeviceExtension,
	_In_ PSTOR_SYNCHRONIZED_ACCESS SynchronizedAccessRoutine,
	_In_opt_ PVOID Context)
{
	kfatal("Implement me!\n");
}

STORPORT_API
STOR_PHYSICAL_ADDRESS
NTAPI
StorPortGetPhysicalAddress(
	_In_ PVOID HwDeviceExtension,
	_In_opt_ PSCSI_REQUEST_BLOCK Srb,
	_In_ PVOID VirtualAddress,
	_Out_ ULONG *Length
)
{
	STOR_PHYSICAL_ADDRESS addr;
	addr.QuadPart = vmp_md_translate((vaddr_t)VirtualAddress);
	return addr;
}

STORPORT_API
PSTOR_SCATTER_GATHER_LIST
NTAPI
StorPortGetScatterGatherList(
	_In_ PVOID DeviceExtension,
	_In_ PSCSI_REQUEST_BLOCK Srb
)
{
	PSTOR_SCATTER_GATHER_LIST sglist = kmem_alloc(
		sizeof(STOR_SCATTER_GATHER_LIST) + sizeof(STOR_SCATTER_GATHER_ELEMENT)
	);
	sglist->NumberOfElements = 1;
	sglist->List[0].PhysicalAddress.QuadPart =
		vmp_md_translate((paddr_t)Srb->DataBuffer);
	sglist->List[0].Length = Srb->DataTransferLength;
	return sglist;
}

STORPORT_API
VOID NTAPI
StorPortQuerySystemTime(_Out_ PLARGE_INTEGER CurrentTime)
{
	CurrentTime->QuadPart = 0x1234;
}

STORPORT_API
VOID NTAPI
StorPortMoveMemory(
	_Out_writes_bytes_(Length) PVOID WriteBuffer,
	_In_reads_bytes_(Length) PVOID ReadBuffer,
	_In_ ULONG Length
)
{
	memcpy(WriteBuffer, ReadBuffer, Length);
	__sync_synchronize();
}

STORPORT_API
BOOLEAN
NTAPI
StorPortResume(
  _In_ PVOID HwDeviceExtension)
{
	kfatal("Implement me!\n");
}


STORPORT_API
ULONG
NTAPI
StorPortReadRegisterUlong(_In_ PVOID HwDeviceExtension, _In_ PULONG Register)
{
	ULONG ret = *(volatile PULONG)Register;
	__sync_synchronize();
	return ret;
}

STORPORT_API
VOID NTAPI
StorPortWriteRegisterUlong(
	_In_ PVOID HwDeviceExtension,
	_In_ PULONG Register,
	_In_ ULONG Value
)
{
	*(volatile PULONG)Register = Value;
	__sync_synchronize();
}

STORPORT_API
BOOLEAN
NTAPI
StorPortPause(
	_In_ PVOID HwDeviceExtension,
	_In_ ULONG TimeOut)
{
	kfatal("Implement me!\n");
}

STORPORT_API
PUCHAR
NTAPI
StorPortAllocateRegistryBuffer(
    _In_ PVOID HwDeviceExtension,
    _In_ PULONG Length)
{
	kprintf("StorPortAllocateRegistryBuffer\n");
	return NULL;
}


STORPORT_API
VOID
NTAPI
StorPortFreeRegistryBuffer(
    _In_ PVOID HwDeviceExtension,
    _In_ PUCHAR Buffer)
{
	kfatal("Implement me!\n");
}

STORPORT_API
BOOLEAN
NTAPI
StorPortRegistryRead(
    _In_ PVOID HwDeviceExtension,
    _In_ PUCHAR ValueName,
    _In_ ULONG Global,
    _In_ ULONG Type,
    _In_ PUCHAR Buffer,
    _In_ PULONG BufferLength)
{
	kfatal("Implement me!\n");
}

struct virtio_scsi_req_cmd {
        // Device-readable part
        uint8_t lun[8];
        uint64_t id;
        uint8_t task_attr;
        uint8_t prio;
        uint8_t crn;
        uint8_t cdb[32];
};

NTAPI
STORPORT_API
VOID __cdecl StorPortNotification(
	_In_ SCSI_NOTIFICATION_TYPE NotificationType,
	_In_ PVOID HwDeviceExtension,
	...
)
{
	__builtin_ms_va_list ap;
	PBOOLEAN Result;
	__builtin_ms_va_start(ap, HwDeviceExtension);

	switch (NotificationType)
	{
	case RequestComplete: {
		PSCSI_REQUEST_BLOCK Srb =
			(PSCSI_REQUEST_BLOCK)va_arg(ap, PSCSI_REQUEST_BLOCK);
		NTStorPort *StorPort = devext_get_self(HwDeviceExtension);
		[StorPort completeSrb:Srb];
		break;
	}

	case AcquireSpinLock: {
		STOR_SPINLOCK SpinLock =
			(STOR_SPINLOCK) __builtin_va_arg(ap, STOR_SPINLOCK);
		PVOID LockContext = (PVOID) __builtin_va_arg(ap, PVOID);
		PSTOR_LOCK_HANDLE LockHandle =
			(PSTOR_LOCK_HANDLE) __builtin_va_arg(ap, PSTOR_LOCK_HANDLE);
		kassert(SpinLock == InterruptLock);
		(void)LockContext;
		LockHandle->Context.OldIrql = ke_spinlock_acquire_at(
			devext_get_spinlock(HwDeviceExtension), kIPLHigh
		);
		break;
	}

	case ReleaseSpinLock: {
		PSTOR_LOCK_HANDLE LockHandle =
			(PSTOR_LOCK_HANDLE) __builtin_va_arg(ap, PSTOR_LOCK_HANDLE);
		kassert(LockHandle->Lock = InterruptLock);
		ke_spinlock_release(
			devext_get_spinlock(HwDeviceExtension), LockHandle->Context.OldIrql
		);
		break;
	}

	case InitializeDpc: {
		PSTOR_DPC Dpc = (PSTOR_DPC) __builtin_va_arg(ap, PSTOR_DPC);
		struct dpc_alias *alias = (void *)Dpc;
		alias->dpcfunc = (NTAPI PHW_DPC_ROUTINE)va_arg(ap, PHW_DPC_ROUTINE);
		alias->devext = HwDeviceExtension;
		alias->dpc = kmem_alloc(sizeof(kdpc_t));
		alias->dpc->arg = alias;
		alias->dpc->callback = dpc_callback;
		alias->dpc->cpu = NULL;
		ke_spinlock_init((kspinlock_t *)&Dpc->Lock);
		break;
	}

	case IssueDpc: {
		PSTOR_DPC Dpc = __builtin_va_arg(ap, PSTOR_DPC);
		PVOID Arg1 = __builtin_va_arg(ap, PVOID);
		PVOID Arg2 = __builtin_va_arg(ap, PVOID);
		PBOOLEAN Succ = __builtin_va_arg(ap, PBOOLEAN);
		struct dpc_alias *alias = (void *)Dpc;

		alias->sysarg1 = Arg1;
		alias->sysarg2 = Arg2;
		ke_dpc_enqueue(alias->dpc);
		*Succ = TRUE;
		break;
	}

	case EnablePassiveInitialization: {
		struct sp_dev_ext *devExt = devext_get_sp_devext(HwDeviceExtension);
		devExt->passive_init =
			__builtin_va_arg(ap, NTAPI PHW_PASSIVE_INITIALIZE_ROUTINE);
		Result = (PBOOLEAN) __builtin_va_arg(ap, PBOOLEAN);
		*Result = TRUE;
		break;
	}

	case TraceNotification:
		break;

	default:
		kfatal("StorPortNotification\n");
	}
	__builtin_ms_va_end(ap);

	return;
}

STORPORT_API
ULONG
StorPortExtendedFunction(
	_In_ STORPORT_FUNCTION_CODE FunctionCode,
	_In_ PVOID HwDeviceExtension,
	...
)
{
	ULONG ret;
	__builtin_ms_va_list ap;
	__builtin_ms_va_start(ap, HwDeviceExtension);

	switch (FunctionCode)
	{
	case ExtFunctionAllocatePool: {
		ULONG NumberOfBytes = __builtin_va_arg(ap, ULONG);
		ULONG Tag = __builtin_va_arg(ap, ULONG);
		PVOID *BufferPointer = __builtin_va_arg(ap, PVOID *);

		(void)Tag;
		*BufferPointer = kmem_alloc(NumberOfBytes);

		ret = 0;
		break;
	}

	case ExtFunctionFreePool: {
		PVOID *BufferPointer = __builtin_va_arg(ap, PVOID *);
		(void)BufferPointer;
		return 0;
	}

	case ExtFunctionGetMessageInterruptInformation: {
		ULONG MessageId = __builtin_va_arg(ap, ULONG);
		PMESSAGE_INTERRUPT_INFORMATION InterruptInfo =
			__builtin_va_arg(ap, PMESSAGE_INTERRUPT_INFORMATION);
		(void)MessageId;
		(void)InterruptInfo;
		ret = 0xc1000002l;
		break;
	}

	case ExtFunctionInitializePerformanceOptimizations: {
		BOOLEAN Query = __builtin_va_arg(ap, int);
		PPERF_CONFIGURATION_DATA PerfConfigData =
			__builtin_va_arg(ap, PPERF_CONFIGURATION_DATA);
		(void)Query;
		(void)PerfConfigData;
		ret = 0xc1000002l;
		break;
	}

	case ExtFunctionSetPowerSettingNotificationGuids:
		ret = 0xc1000002l;
		break;

	case ExtFunctionInvokeAcpiMethod:
		ret = 0xc1000002l;
		break;

	case ExtFunctionInitializeTimer: {
		PVOID *TimerHandle = __builtin_va_arg(ap, PVOID *);
		ktimer_t *timer = kmem_alloc(sizeof(*timer));

		*TimerHandle = timer;

		ret = 0;
		break;
	}

	case ExtFunctionRequestTimer: {
		PVOID TimerHandle = __builtin_va_arg(ap, PVOID);
		PHW_TIMER_EX TimerCallback = __builtin_va_arg(ap, PHW_TIMER_EX);
		PVOID CallbackContext = __builtin_va_arg(ap, PVOID);
		ULONGLONG TimerValue = __builtin_va_arg(ap, ULONGLONG);
		ULONGLONG TolerableDelay = __builtin_va_arg(ap, ULONGLONG);

		(void)TimerHandle;
		(void)TimerCallback;
		(void)CallbackContext;
		(void)TimerValue;
		(void)TolerableDelay;

		ret = 0;
		break;
	}

	case ExtFunctionInitializePoFxPower:
	case ExtFunctionGetD3ColdSupport: {
		ret = 0xc1000002l;
		break;
	}

	default:
		kfatal(
			"StorPortExtendedFunction: unhandled function code %u\n",
			FunctionCode
		);
	}

	return ret;
}

/* clang-format off */
image_export_t storport_exports[] = {
	EXPORT_NT_FUNC(StorPortInitialize),
	EXPORT_NT_FUNC(StorPortDebugPrint),
	EXPORT_NT_FUNC(StorPortBusy),
	EXPORT_NT_FUNC(StorPortGetBusData),
	EXPORT_NT_FUNC(StorPortGetDeviceBase),
	EXPORT_NT_FUNC(StorPortGetUncachedExtension),
	EXPORT_NT_FUNC(StorPortLogError),
	EXPORT_NT_FUNC(StorPortStallExecution),
	EXPORT_NT_FUNC(StorPortSynchronizeAccess),
	EXPORT_NT_FUNC(StorPortGetPhysicalAddress),
	EXPORT_NT_FUNC(StorPortGetScatterGatherList),
	EXPORT_NT_FUNC(StorPortQuerySystemTime),
	EXPORT_NT_FUNC(StorPortMoveMemory),
	EXPORT_NT_FUNC(StorPortResume),
	EXPORT_NT_FUNC(StorPortReadRegisterUlong),
	EXPORT_NT_FUNC(StorPortWriteRegisterUlong),
	EXPORT_NT_FUNC(StorPortNotification),
	EXPORT_NT_FUNC(StorPortExtendedFunction),
	EXPORT_NT_FUNC(StorPortPause),
	EXPORT_NT_FUNC(StorPortDeviceBusy),
	EXPORT_NT_FUNC(StorPortSetDeviceQueueDepth),
	EXPORT_NT_FUNC(StorPortAllocateRegistryBuffer),
	EXPORT_NT_FUNC(StorPortFreeRegistryBuffer),
	EXPORT_NT_FUNC(StorPortRegistryRead),
	{NULL, NULL}
};
