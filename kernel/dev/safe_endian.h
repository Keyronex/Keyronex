#ifndef KRX_DEV_SAFE_ENDIAN_H
#define KRX_DEV_SAFE_ENDIAN_H

#include <stdint.h>

#include "kdk/endian.h"

typedef struct leu16 {
	uint16_t value;
} leu16_t;

typedef struct leu32 {
	uint32_t value;
} leu32_t;

typedef struct leu64 {
	uint64_t value;
} leu64_t;

#define from_leu16(LEU16) le16_to_native((LEU16).value)
#define from_leu32(LEU32) le32_to_native((LEU32).value)

#endif /* KRX_DEV_SAFE_ENDIAN_H */
