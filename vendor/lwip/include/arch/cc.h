#ifndef KRX_ARCH_CC_H
#define KRX_ARCH_CC_H

#include <limits.h>

#include "abi-bits/time.h"
#include "bits/ssize_t.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BYTE_ORDER
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BYTE_ORDER (LITTLE_ENDIAN)
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BYTE_ORDER (BIG_ENDIAN)
#else
#error "Byte order is neither little nor big!"
#endif
#endif

#define LWIP_NO_INTTYPES_H 0
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_STDLIB_H 1

#define LWIP_TIMEVAL_PRIVATE 0

#define LWIP_PLATFORM_DIAG(x) kprintf x
#ifdef NDEBUG
#define LWIP_PLATFORM_ASSERT(x)
#else
#define LWIP_PLATFORM_ASSERT(x)                                            \
	do {                                                               \
		kprintf("Assertion \"%s\" failed at %s:%d\n", x, __FILE__, \
		    __LINE__);                                             \
		kfatal("fatal in lwip\n");                                   \
	} while (0)
#endif
#ifdef __cplusplus
}
#endif

#endif /* KRX_ARCH_CC_H */
