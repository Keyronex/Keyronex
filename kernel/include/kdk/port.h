#ifndef KRX_KDK_PORT_H
#define KRX_KDK_PORT_H

#if defined(__m68k__)
#include "m68k.h"
#elif defined(AARCH64)
#include "aarch64.h"
#elif defined(AMD64)
#include "amd64.h"
#elif defined(__riscv)
#include "riscv64.h"
#else
#error "Unknown port!"
#endif

#endif /* KRX_KDK_PORT_H */
