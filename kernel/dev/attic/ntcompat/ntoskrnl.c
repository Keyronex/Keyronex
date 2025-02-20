#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/kern.h"
#include "ntcompat/ntcompat.h"
#include "ntcompat/win_types.h"

#define UNIMPLEMENTED() kfatal("%s: unimplemented\n", __PRETTY_FUNCTION__);

NTAPI
ULONG
NTAPI
vDbgPrintEx(ULONG ComponentId, ULONG Level, const char *Format,
    __builtin_ms_va_list arglist)
{
	return npf_msvpprintf(kputc, NULL, Format, arglist);
}

NTAPI
ULONG
NTAPI
DbgPrint(const char *Format, ...)
{
	ULONG ret;
	__builtin_ms_va_list ap;
	__builtin_ms_va_start(ap, Format);
	ret = npf_msvpprintf(kputc, NULL, Format, ap);
	__builtin_ms_va_end(ap);
	return ret;
}

VOID NTAPI
RtlAssert(_In_ PVOID FailedAssertion, _In_ PVOID FileName,
    _In_ ULONG LineNumber, _In_opt_ PSTR Message)
{
	kfatal("Failed assertion at <%s:%u>: %s\n", (const char *)FileName,
	   LineNumber, Message);
}

NTKERNELAPI
DECLSPEC_NORETURN
VOID NTAPI
KeBugCheck(_In_ ULONG BugCheckCode)
{
	kfatal("BugCheck(%u)\n", BugCheckCode);
}

NTKERNELAPI
DECLSPEC_NORETURN
VOID NTAPI
KeBugCheckEx(IN ULONG BugCheckCode, IN ULONG_PTR BugCheckParameter1,
    IN ULONG_PTR BugCheckParameter2, IN ULONG_PTR BugCheckParameter3,
    IN ULONG_PTR BugCheckParameter4)
{
	kfatal("KeBugCheckEx(%u, 0x%zx, 0x%zx, 0x%zx, 0x%zx)", BugCheckCode,
	    BugCheckParameter1, BugCheckParameter2, BugCheckParameter3,
	    BugCheckParameter4);
}

NTKERNELAPI
uint8_t NTAPI KeGetCurrentIrql(void)
{
	return splget();
}

typedef void *PKSPIN_LOCK;
typedef struct _LIST_ENTRY {
	struct _LIST_ENTRY *Flink;
	struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

VOID
InsertHeadList(_Inout_ PLIST_ENTRY ListHead, _Inout_ PLIST_ENTRY Entry)
{
	PLIST_ENTRY First = ListHead->Flink;
	Entry->Flink = First;
	Entry->Blink = ListHead;
	First->Blink = Entry;
	ListHead->Flink = Entry;
}

VOID
InsertTailList(_Inout_ PLIST_ENTRY ListHead, _Inout_ PLIST_ENTRY Entry)
{
	PLIST_ENTRY Last = ListHead->Blink;
	Entry->Flink = ListHead;
	Entry->Blink = Last;
	Last->Flink = Entry;
	ListHead->Blink = Entry;
}

PLIST_ENTRY
RemoveHeadList(_Inout_ PLIST_ENTRY ListHead)
{
	PLIST_ENTRY First = ListHead->Flink;
	PLIST_ENTRY Next;
	if (First == ListHead)
		return NULL;
	Next = First->Flink;
	ListHead->Flink = Next;
	Next->Blink = ListHead;
	return First;
}

PLIST_ENTRY
NTAPI
ExInterlockedInsertHeadList(IN OUT PLIST_ENTRY ListHead,
    IN OUT PLIST_ENTRY ListEntry, IN OUT PKSPIN_LOCK Lock)
{
	PLIST_ENTRY first;
	ipl_t ipl;

	ipl = ke_spinlock_acquire_at((void *)Lock, kIPLHigh);
	first = ListHead->Flink;
	InsertHeadList(ListHead, ListEntry);
	ke_spinlock_release(Lock, ipl);

	return (first == ListHead) ? NULL : first;
}

PLIST_ENTRY NTAPI
ExInterlockedInsertTailList(IN OUT PLIST_ENTRY ListHead,
    IN OUT PLIST_ENTRY ListEntry, IN OUT PKSPIN_LOCK Lock)
{
	PLIST_ENTRY first;
	ipl_t ipl;

	ipl = ke_spinlock_acquire_at((void *)Lock, kIPLHigh);
	first = ListHead->Flink;
	InsertTailList(ListHead, ListEntry);
	ke_spinlock_release(Lock, ipl);

	return (first == ListHead) ? NULL : first;
}

PLIST_ENTRY
NTAPI
ExInterlockedRemoveHeadList(IN OUT PLIST_ENTRY ListHead,
    IN OUT PKSPIN_LOCK Lock)
{
	PLIST_ENTRY ListEntry;
	ipl_t ipl;

	ipl = ke_spinlock_acquire_at((void *)Lock, kIPLHigh);

	if (ListHead->Flink == ListHead)
		ListEntry = NULL;
	else
		ListEntry = RemoveHeadList(ListHead);

	ke_spinlock_release(Lock, ipl);

	return ListEntry;
}

NTKERNELAPI
ULONG
KeQueryActiveProcessorCount(OUT PVOID ActiveProcessors)
{
	kassert(ActiveProcessors == NULL);
	return ncpus;
}

NTKERNELAPI
ULONG
KeQueryMaximumProcessorCount(void)
{
	return ncpus;
}

NTKERNELAPI
ULONG
KeQueryActiveProcessorCountEx(IN USHORT GroupNumber)
{
	return ncpus;
}

NTKERNELAPI
ULONG
KeQueryMaximumProcessorCountEx(IN USHORT GroupNumber)
{
	return ncpus;
}

NTKERNELAPI
size_t NTAPI
nt_strlen(const char *str)
{
	return strlen(str);
}

typedef enum _POOL_TYPE {
	NonPagedPool,
	PagedPool,
	NonPagedPoolMustSucceed,
	DontUseThisType,
	NonPagedPoolCacheAligned,
	PagedPoolCacheAligned,
	NonPagedPoolCacheAlignedMustS,
	MaxPoolType,
	NonPagedPoolSession = 32,
	PagedPoolSession,
	NonPagedPoolMustSucceedSession,
	DontUseThisTypeSession,
	NonPagedPoolCacheAlignedSession,
	PagedPoolCacheAlignedSession,
	NonPagedPoolCacheAlignedMustSSession
} POOL_TYPE;

typedef enum _MEMORY_CACHING_TYPE_ORIG {
	MmFrameBufferCached = 2
} MEMORY_CACHING_TYPE_ORIG;

typedef enum _MEMORY_CACHING_TYPE {
	MmNonCached = FALSE,
	MmCached = TRUE,
	MmWriteCombined = MmFrameBufferCached,
	MmHardwareCoherentCached,
	MmNonCachedUnordered,
	MmUSWCCached,
	MmMaximumCacheType
} MEMORY_CACHING_TYPE;

NTKERNELAPI
PVOID NTAPI
ExAllocatePoolWithTag(IN POOL_TYPE PoolType, IN size_t NumberOfBytes,
    IN ULONG Tag)
{
	return kmem_alloc(NumberOfBytes);
}

NTKERNELAPI
VOID NTAPI
ExFreePoolWithTag(IN PVOID P, IN ULONG Tag)
{
	UNIMPLEMENTED();
}

NTOSAPI
PVOID
DDKAPI
MmAllocateContiguousMemorySpecifyCache(IN SIZE_T NumberOfBytes,
    IN PHYSICAL_ADDRESS LowestAcceptableAddress,
    IN PHYSICAL_ADDRESS HighestAcceptableAddress,
    IN PHYSICAL_ADDRESS BoundaryAddressMultiple /*OPTIONAL*/,
    IN MEMORY_CACHING_TYPE CacheType)
{
	UNIMPLEMENTED();
}

NTOSAPI
VOID DDKAPI
MmFreeContiguousMemorySpecifyCache(
    /*IN*/ PVOID BaseAddress,
    /*IN*/ SIZE_T NumberOfBytes,
    /*IN*/ MEMORY_CACHING_TYPE CacheType)
{
	UNIMPLEMENTED();
}

/* clang-format off */
image_export_t ntoskrnl_exports[] = {
	{ "strlen", nt_strlen },
	EXPORT_NT_FUNC(RtlAssert),
	EXPORT_NT_FUNC(DbgPrint),
	EXPORT_NT_FUNC(vDbgPrintEx),
	EXPORT_NT_FUNC(KeBugCheck),
	EXPORT_NT_FUNC(KeBugCheckEx),
	EXPORT_NT_FUNC(KeGetCurrentIrql),
	EXPORT_NT_FUNC(ExInterlockedInsertHeadList),
	EXPORT_NT_FUNC(ExInterlockedInsertTailList),
	EXPORT_NT_FUNC(ExInterlockedRemoveHeadList),
	EXPORT_NT_FUNC(KeQueryActiveProcessorCount),
	EXPORT_NT_FUNC(KeQueryMaximumProcessorCount),
	EXPORT_NT_FUNC(KeQueryActiveProcessorCountEx),
	EXPORT_NT_FUNC(KeQueryMaximumProcessorCountEx),
	EXPORT_NT_FUNC(ExAllocatePoolWithTag),
	EXPORT_NT_FUNC(ExFreePoolWithTag),
	EXPORT_NT_FUNC(MmAllocateContiguousMemorySpecifyCache),
	EXPORT_NT_FUNC(MmFreeContiguousMemorySpecifyCache),
	{ NULL, NULL }
};
