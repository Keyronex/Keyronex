/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file k_cpumask.h
 * @brief CPU set operations.
 */

#ifndef ECX_KEYRONEX_K_CPUMASK_H
#define ECX_KEYRONEX_K_CPUMASK_H

#include <sys/k_types.h>

#include <stdbool.h>
#include <stddef.h>

#define UINTPTR_BITS (sizeof(uintptr_t) * 8)

extern size_t ke_ncpu;

static inline void
ke_cpumask_zero(kcpumask_t *set)
{
	for (size_t i = 0; i < CPUMASK_WIDTH; i++)
		set->mask[i] = 0;
}

static inline void
ke_cpumask_set(kcpumask_t *set, kcpunum_t cpu)
{
	set->mask[cpu / UINTPTR_BITS] |= (uintptr_t)1 << (cpu % UINTPTR_BITS);
}

static inline void
ke_cpumask_clear(kcpumask_t *set, kcpunum_t cpu)
{
	set->mask[cpu / UINTPTR_BITS] &= ~((uintptr_t)1 << (cpu % UINTPTR_BITS));
}

static inline bool
ke_cpumask_isset(const kcpumask_t *set, kcpunum_t cpu)
{
	return (set->mask[cpu / UINTPTR_BITS] &
		   ((uintptr_t)1 << (cpu % UINTPTR_BITS))) != 0;
}

static inline bool
ke_cpumask_empty(const kcpumask_t *set)
{
	for (size_t i = 0; i < CPUMASK_WIDTH; i++)
		if (set->mask[i] != 0)
			return false;
	return true;
}

static inline kcpunum_t
ke_cpumask_next(const kcpumask_t *set, kcpunum_t from)
{
	for (size_t i = from / UINTPTR_BITS; i < CPUMASK_WIDTH; i++) {
		uintptr_t word = set->mask[i];

		if (i == from / UINTPTR_BITS)
			word &= ~(uintptr_t)0 << (from % UINTPTR_BITS);
		if (word != 0)
			return i * UINTPTR_BITS + __builtin_ctzl(word);
	}
	return KCPUNUM_NULL;
}

#define KE_CPUMASK_FOREACH(CPU, SET)                         \
	for ((CPU) = ke_cpumask_next((SET), 0); (CPU) != KCPUNUM_NULL; \
	    (CPU) = ke_cpumask_next((SET), (CPU) + 1))

static inline void
ke_atomic_cpumask_zero(katomic_cpumask_t *set)
{
	for (size_t i = 0; i < CPUMASK_WIDTH; i++)
		atomic_store_explicit(&set->mask[i], 0, memory_order_relaxed);
}

static inline void
ke_atomic_cpumask_set(katomic_cpumask_t *set, kcpunum_t cpu,
    memory_order order)
{
	atomic_fetch_or_explicit(&set->mask[cpu / UINTPTR_BITS],
	    (uintptr_t)1 << (cpu % UINTPTR_BITS), order);
}

static inline void
ke_atomic_cpumask_clear(katomic_cpumask_t *set, kcpunum_t cpu,
    memory_order order)
{
	atomic_fetch_and_explicit(&set->mask[cpu / UINTPTR_BITS],
	    ~((uintptr_t)1 << (cpu % UINTPTR_BITS)), order);
}

static inline bool
ke_atomic_cpumask_isset(katomic_cpumask_t *set, kcpunum_t cpu,
    memory_order order)
{
	uintptr_t word = atomic_load_explicit(&set->mask[cpu / UINTPTR_BITS],
	    order);
	return (word & ((uintptr_t)1 << (cpu % UINTPTR_BITS))) != 0;
}

static inline kcpunum_t
ke_atomic_cpumask_first(katomic_cpumask_t *set, memory_order order)
{
	size_t width = (ke_ncpu + UINTPTR_BITS - 1) / UINTPTR_BITS;

	for (size_t i = 0; i < width; i++) {
		uintptr_t word = atomic_load_explicit(&set->mask[i], order);

		if (word != 0) {
			kcpunum_t cpu = i * UINTPTR_BITS + __builtin_ctzl(word);
			return cpu < ke_ncpu ? cpu : KCPUNUM_NULL;
		}
	}
	return KCPUNUM_NULL;
}

#endif /* ECX_KEYRONEX_K_CPUMASK_H */
