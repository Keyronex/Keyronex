/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file bit32.h
 * @brief Generic bitmap interface.
 */

#ifndef ECX_SYS_IDALLOC_H
#define ECX_SYS_IDALLOC_H

#include <sys/k_intr.h>
#include <stdint.h>

typedef uint32_t bitmap_32_t;

static inline bool
bit32_test(bitmap_32_t *bitmap, uint32_t bit)
{
	return (bitmap[bit / 32] & (1 << (bit % 32))) != 0;
}

static inline void
bit32_set(bitmap_32_t *bitmap, uint32_t bit)
{
	bitmap[bit / 32] |= (1 << (bit % 32));
}

static inline void
bit32_clear(bitmap_32_t *bitmap, uint32_t bit)
{
	bitmap[bit / 32] &= ~(1 << (bit % 32));
}

static inline size_t
bit32_size(size_t n_bits)
{
	return (n_bits + 31) / 32 * sizeof(bitmap_32_t);
}

static inline uint32_t
bit32_nelem(size_t n_bits)
{
	return (n_bits + 31) / 32;
}

#endif /* ECX_SYS_IDALLOC_H */
