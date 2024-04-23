#ifndef CWK_KERNEL_WIN_TYPES_H_
#define CWK_KERNEL_WIN_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#define _WIN64

typedef char CCHAR, *PCCHAR, *PSTR;
typedef uint8_t UCHAR, *PUCHAR;
typedef int8_t CHAR, *PCHAR;
typedef int16_t WORD;
typedef uint16_t USHORT, *PUSHORT;
typedef uint32_t ULONG,*PULONG;
typedef short CSHORT;
typedef int32_t LONG, *PLONG;
typedef int32_t DWORD;
typedef int64_t LONGLONG, *PLONGLONG;
typedef uint64_t ULONGLONG, *PULONGLONG;
typedef uint64_t ULONG64;
typedef void VOID, *PVOID;
typedef uint8_t BOOLEAN, *PBOOLEAN;
typedef size_t SIZE_T;

typedef uint16_t PWSTR;

typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;

typedef ULONG_PTR KAFFINITY;

typedef struct _GROUP_AFFINITY {
  KAFFINITY Mask;
  WORD      Group;
  WORD      Reserved[3];
} GROUP_AFFINITY, *PGROUP_AFFINITY;

typedef enum _INTERFACE_TYPE {
  InterfaceTypeUndefined = -1,
  Internal,
  Isa,
  Eisa,
  MicroChannel,
  TurboChannel,
  kPCIBus,
  VMEBus,
  NuBus,
  PCMCIABus,
  CBus,
  MPIBus,
  MPSABus,
  ProcessorInternal,
  InternalPowerBus,
  PNPISABus,
  PNPBus,
  Vmcs,
  ACPIBus,
  MaximumInterfaceType
} INTERFACE_TYPE, *PINTERFACE_TYPE;

typedef enum _DMA_WIDTH {
    Width8Bits,
    Width16Bits,
    Width32Bits,
    Width64Bits,
    WidthNoWrap,
    MaximumDmaWidth
}DMA_WIDTH, *PDMA_WIDTH;

typedef enum _DMA_SPEED {
    Compatible,
    TypeA,
    TypeB,
    TypeC,
    TypeF,
    MaximumDmaSpeed
}DMA_SPEED, *PDMA_SPEED;

typedef enum _KINTERRUPT_MODE {
  LevelSensitive,
  Latched
} KINTERRUPT_MODE;

typedef union _LARGE_INTEGER {
  struct {
    DWORD LowPart;
    LONG  HighPart;
  } DUMMYSTRUCTNAME;
  struct {
    DWORD LowPart;
    LONG  HighPart;
  } u;
  LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef int KIRQL;

typedef LARGE_INTEGER PHYSICAL_ADDRESS;

typedef struct _CONTEXT { } *PCONTEXT;

#define _WIN64

#define NTDDI_VERSION 9000
#define NTDDI_WIN8 4000
#define NTDDI_WIN7 3000
#define NTDDI_WS03SP1 2000
#define NTDDI_WINXP 1000

#ifdef __amd64
#define NTAPI __attribute__((ms_abi))
#define __cdecl __attribute__((ms_abi))
#else
#define NTAPI
#define __cdecl
#endif

#define ANYSIZE_ARRAY 1
#define NTKERNELAPI NTAPI
#define NTOSAPI NTAPI
#define DDKAPI NTAPI
#define DECLSPEC_NORETURN __attribute__((noreturn))
#define IN
#define OUT
#define OPTIONAL

//__attribute__((always_inline))
#define FORCEINLINE inline
#define DECLSPEC_IMPORT

#define _In_
#define _Out_
#define _Inout_
#define _When_(...)
#define _In_opt_
#define _In_reads_bytes_(...)
#define _Out_writes_bytes_(...)
#define _Field_size_bytes_(...)

#define _ANONYMOUS_UNION
#define _ANONYMOUS_STRUCT
#define DUMMYSTRUCTNAME dummy_struct
#define DUMMYUNIONNAME dummy_union

#define FALSE 0
#define TRUE 1

#define FIELD_OFFSET(type, field) __builtin_offsetof(type, field)

#endif /* CWK_KERNEL_WIN_TYPES_H_ */
