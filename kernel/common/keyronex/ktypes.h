/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file kwait.h
 * @brief Types used in the kernel.
 */

#ifndef ECX_KEYRONEX_KTYPES_H
#define ECX_KEYRONEX_KTYPES_H

#include <stdint.h>

#if !defined(__OBJC__)
#include <stdatomic.h>
#else
typedef uintptr_t atomic_uintptr_t;
#endif

#define KSTACK_SIZE   32768
#define MAX_CPUS 256
#define CPUMASK_WIDTH (MAX_CPUS / (sizeof(uintptr_t) * 8))

#define MIN2(a, b) (((a) < (b)) ? (a) : (b))
#define MAX2(a, b) (((a) > (b)) ? (a) : (b))

#define elementsof(x) (sizeof(x) / sizeof((x)[0]))
#define containerof(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);   \
        (type *)( (char *)__mptr - offsetof(type,member) );    \
})

#define roundup2(ADDR, ALIGN) (((ADDR) + ALIGN - 1) & ~(ALIGN - 1))
#define rounddown2(ADDR, ALIGN) ((((uintptr_t)ADDR)) & ~(ALIGN - 1))
#define roundup(VAL, ALIGN) (((VAL) + (ALIGN) - 1) / (ALIGN) * (ALIGN))
#define rounddown(VAL, ALIGN) (((VAL) / (ALIGN)) * (ALIGN))

typedef uint32_t kcpunum_t;

typedef struct kcpumask {
	uintptr_t mask[CPUMASK_WIDTH];
} kcpumask_t;

typedef struct katomic_cpumask {
	atomic_uintptr_t mask[CPUMASK_WIDTH];
} katomic_cpumask_t;

/*! Absolute time in nanoseconds. */
typedef uint64_t kabstime_t;
/*! Relative time in nanoseconds. */
typedef uint64_t knanosecs_t;

typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;

#define KCPUNUM_NULL ((kcpunum_t)-1)

#define ABSTIME_FOREVER ((kabstime_t)-1)
#define ABSTIME_NEVER  ((kabstime_t)0)

#define NS_PER_S  1000000000
#define NS_PER_MS 1000000

#endif /* ECX_KEYRONEX_KTYPES_H */
