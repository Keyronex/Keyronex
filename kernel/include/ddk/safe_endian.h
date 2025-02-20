/*
 * Copyright (c) 2023-2025 NetaScale Object Solutions.
 * Created on Sun Sep 24 2023.
 */

#ifndef KRX_DEV_SAFE_ENDIAN_H
#define KRX_DEV_SAFE_ENDIAN_H

#include <kdk/endian.h>
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

typedef struct beu16 {
	uint16_t value;
} beu16_t;

typedef struct beu32 {
	uint32_t value;
} beu32_t;

typedef struct beu64 {
	uint64_t value;
} beu64_t;

#define from_leu16(LEU16) le16_to_native((LEU16).value)
#define from_leu32(LEU32) le32_to_native((LEU32).value)
#define from_leu64(LEU64) le64_to_native((LEU64).value)

#define from_beu16(BEU16) be16_to_native((BEU16).value)
#define from_beu32(BEU32) be32_to_native((BEU32).value)

#define to_leu16(U16) ((leu16_t) { native_to_le16(U16) })
#define to_leu32(U32) ((leu32_t) { native_to_le32(U32) })
#define to_leu64(U64) ((leu64_t) { native_to_le64(U64) })

#define to_beu16(U16) ((beu16_t) { native_to_be16(U16) })
#define to_beu32(U32) ((beu32_t) { native_to_be32(U32) })

#endif /* KRX_DEV_SAFE_ENDIAN_H */
