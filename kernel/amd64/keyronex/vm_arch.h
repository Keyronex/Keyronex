/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm_arch.h
 * @brief AMD64 VM definitions.
 */

#ifndef ECX_KEYRONEX_VM_ARCH_H
#define ECX_KEYRONEX_VM_ARCH_H

#include <keyronex/vm_types.h>

#include <stddef.h>

#define PMAP_MAX_LEVELS 4

#if 1 /* 4-level paging */
#define PMAP_LEVELS 4

#define PMAP_L1_PAGES 1
#define PMAP_L2_PAGES 0 /* maybe */
#define PMAP_L3_PAGES 0

#define LOWER_HALF 0x10000
#define LOWER_HALF_SIZE ((1ULL << 47) - LOWER_HALF) /* 128 TiB ish */

#define HIGHER_HALF 0xffff800000000000 /* -128 TiB */
#define HHDM_BASE   0xffff800000000000 /* -128 TiB */
#define HHDM_SIZE   (1ULL << 46)       /* 64 TiB */
#define HHDM_END (HHDM_BASE + HHDM_SIZE)

/* RPT from -64 TiB to -60 TiB: 4 TiB = (256 TiB / 4096) * sizeof(vm_page_t) */
#define RPT_BASE 0xffffc00000000000 /* -64 TiB */
#define RPT_SIZE (1ULL << 42) /* 4 TiB */

/* Pinned heap from -56 TiB to -52 TiB */
#define PIN_HEAP_BASE 0xffffc80000000000
#define PIN_HEAP_SIZE (1ULL << 42)

/* Paged heap from -48 TiB to -44 TiB */
#define PAGE_HEAP_BASE 0xffffd00000000000
#define PAGE_HEAP_SIZE (1ULL << 42)

/* File mapping area from -40 TiB to -36 TiB */
#define FILE_MAP_BASE 0xffffd80000000000
#define FILE_MAP_SIZE (1ULL << 42)

/* Miscellaneous mapping area from -32 TiB to -28 TiB */
#define MISC_MAP_BASE 0xffffe00000000000
#define MISC_MAP_SIZE (1ULL << 42)

/* Kernel text and data from -2GiB to the end of the address space. */
#define KERN_TEXT_BASE	0xffffffff80000000
#define KERN_TEXT_SIZE	0x80000000

static inline void
pmap_indexes(vaddr_t vaddr, size_t indexes[PMAP_MAX_LEVELS])
{
	indexes[3] = (vaddr >> 39) & 0x1ff;
	indexes[2] = (vaddr >> 30) & 0x1ff;
	indexes[1] = (vaddr >> 21) & 0x1ff;
	indexes[0] = (vaddr >> 12) & 0x1ff;
}
#endif

#define PGTABLE_SIZE	0x1000
#define PGSIZE		0x1000
#define PGSHIFT		12
#define PGSIZE_L1	0x200000
#define PGSIZE_L2	0x40000000
#define PGSIZE_L3 	0x8000000000

/* PAT indexes */
enum vm_cache_mode {
	kCacheModeWB = 0,
	kCacheModeDefault = kCacheModeWB,
	kCacheModeWT = 1,
	kCacheModeUCMinus = 2,
	kCacheModeUC = 3,
	kCacheModeWP = 4,
	kCacheModeWC = 5,
};

#define p2v(PA) ((vaddr_t)(PA) + HHDM_BASE)
#define v2p(VA) ((paddr_t)(VA) - HHDMBASE)


#endif /* ECX_KEYRONEX_VM_ARCH_H */
