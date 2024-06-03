#ifndef KRX_KDK_ENDIAN_H
#define KRX_KDK_ENDIAN_H

#include "port.h"

typedef uint16_t le16_t;
typedef uint32_t le32_t;
typedef uint64_t le64_t;

#if ENDIAN == KRX_BIG_ENDIAN
#define le16_to_native(X) __builtin_bswap16(X)
#define native_to_le16(X) __builtin_bswap16(X)

#define le32_to_native(X) __builtin_bswap32(X)
#define native_to_le32(X) __builtin_bswap32(X)

#define le64_to_native(X) __builtin_bswap64(X)
#define native_to_le64(X) __builtin_bswap64(X)

#define be16_to_native(X) (X)
#define native_to_be16(X) (X)

#define be32_to_native(X) (X)
#define native_to_be32(X) (X)

#define be64_to_native(X) (X)
#define native_to_be64(X) (X)
#else
#define le16_to_native(X) (X)
#define native_to_le16(X) (X)

#define le32_to_native(X) (X)
#define native_to_le32(X) (X)

#define le64_to_native(X) (X)
#define native_to_le64(X) (X)

#define be16_to_native(X) __builtin_bswap16(X)
#define native_to_be16(X) __builtin_bswap16(X)

#define be32_to_native(X) __builtin_bswap32(X)
#define native_to_be32(X) __builtin_bswap32(X)

#define be64_to_native(X) __builtin_bswap64(X)
#define native_to_be64(X) __builtin_bswap64(X)
#endif


#endif /* KRX_KDK_ENDIAN_H */
