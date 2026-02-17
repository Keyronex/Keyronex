#ifndef KRX_DDK_LINTYPES_H
#define KRX_DDK_LINTYPES_H

#include <keyronex/krx_endian.h>

typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int16_t __s16;

typedef leu16_t __le16;
typedef leu32_t __le32;
typedef leu64_t __le64;
typedef les16_t __les16;
typedef les32_t __les32;
typedef les64_t __les64;

typedef __u8 u8;
typedef __u16 u16;
typedef __u32 u32;
typedef __u64 u64;
typedef __s16 s16;

typedef __le16 le16;
typedef __le32 le32;
typedef __le64 le64;

typedef leu16_t __virtio16;
typedef leu32_t __virtio32;
typedef leu64_t __virtio64;

#endif /* KRX_DDK_LINTYPES_H */
