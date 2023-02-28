#ifndef KRX_Keyronex_KERNDEFS_H
#define KRX_Keyronex_KERNDEFS_H

#include <stdint.h>

#if 0
#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif
#endif

#define krx_in
#define krx_out
#define krx_nullable

#define elementsof(x) (sizeof(x) / sizeof(x[0]))

#define MIN2(a, b) (((a) < (b)) ? (a) : (b))
#define MAX2(a, b) (((a) > (b)) ? (a) : (b))
#define ROUNDUP(addr, align) (((addr) + align - 1) & ~(align - 1))
#define ROUNDDOWN(addr, align) ((((uintptr_t)addr)) & ~(align - 1))
#define PGROUNDUP(addr) ROUNDUP(addr, PGSIZE)
#define PGROUNDDOWN(addr) ROUNDDOWN(addr, PGSIZE)

#define NS_PER_S 1000000000

#define KERN_HZ 100

typedef enum krx_status {
kNone,
} krx_status_t;

/*! Nanoseconds. */
typedef uint64_t nanosecs_t;
/*! A virtual address. */
typedef uintptr_t vaddr_t;
/*! A physical address. */
typedef uintptr_t paddr_t;
/*! A virtual offset. */
typedef intptr_t voff_t;

#endif /* KRX_Keyronex_KERNDEFS_H */
