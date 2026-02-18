/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file krx_endian.h
 * @brief Type-safe dealing in endians.
 */

#ifndef ECX_KEYRONEX_KRX_ENDIAN_H
#define ECX_KEYRONEX_KRX_ENDIAN_H

#include <stdint.h>

typedef struct leu16 {
	uint16_t value;
} leu16_t;

typedef struct leu32 {
	uint32_t value;
} leu32_t;

typedef struct leu64 {
	uint64_t value;
} leu64_t;

typedef struct les16 {
	int16_t value16;
} les16_t;

typedef struct les32 {
	int32_t value32;
} les32_t;

typedef struct les64 {
	int64_t value64;
} les64_t;

typedef struct beu16 {
	uint16_t value;
} beu16_t;

typedef struct beu32 {
	uint32_t value;
} beu32_t;

typedef struct beu64 {
	uint64_t value;
} beu64_t;

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
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

#define from_leu16(LEU16) le16_to_native((LEU16).value)
#define from_leu32(LEU32) le32_to_native((LEU32).value)
#define from_leu64(LEU64) le64_to_native((LEU64).value)

#define from_les16(LEU16) le16_to_native((LEU16).value16)
#define from_les32(LEU32) le32_to_native((LEU32).value32)
#define from_les64(LEU64) le64_to_native((LEU64).value64)

#define from_beu16(BEU16) be16_to_native((BEU16).value)
#define from_beu32(BEU32) be32_to_native((BEU32).value)
#define from_beu64(BEU64) be64_to_native((BEU64).value)

#define to_leu16(U16)	  ((leu16_t) { native_to_le16(U16) })
#define to_leu32(U32)	  ((leu32_t) { native_to_le32(U32) })
#define to_leu64(U64)	  ((leu64_t) { native_to_le64(U64) })

#define to_les16(U16)	  ((les16_t) { native_to_le16(U16) })
#define to_les32(U32)	  ((les32_t) { native_to_le32(U32) })
#define to_les64(U64)	  ((les64_t) { native_to_le64(U64) })

#define to_beu16(U16)	  ((beu16_t) { native_to_be16(U16) })
#define to_beu32(U32)	  ((beu32_t) { native_to_be32(U32) })
#define to_beu64(U64)	  ((beu64_t) { native_to_be64(U64) })

#endif /* ECX_KEYRONEX_KRX_ENDIAN_H */
