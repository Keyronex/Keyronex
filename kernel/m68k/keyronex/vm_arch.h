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

#define PMAP_MAX_LEVELS 3

#define PMAP_LEVELS 3

#define PMAP_L1_PAGES 0
#define PMAP_L2_PAGES 0
#define PMAP_L3_PAGES 0

#define LOWER_HALF 0x1000
#define LOWER_HALF_SIZE (0x80000000 - LOWER_HALF)

#define HIGHER_HALF 0x80000000
#define HHDM_BASE 0x80000000
#define HHDM_SIZE 0x40000000
#define HHDM_END (HHDM_BASE + HHDM_SIZE)

#define RPT_BASE 0xfc000000 /* -64 MiB */
#define RPT_SIZE 0x1000000 /* 16 miB */

#define PIN_HEAP_BASE 0xc0000000
#define PIN_HEAP_SIZE 0x10000000

#define PAGE_HEAP_BASE 0xf0000000
#define PAGE_HEAP_SIZE 0xc000000

#define FILE_MAP_BASE 0xd0000000
#define FILE_MAP_SIZE 0x10000000

#define MISC_MAP_BASE 0xe0000000
#define MISC_MAP_SIZE 0x10000000

/* Kernel text and data from -32MiB to the end of the address space. */
#define KERN_TEXT_BASE	0xfd000000

#define PGTABLE_SIZE	0x1000
#define PGSIZE		0x1000
#define PGSHIFT		12

enum vm_cache_mode {
	kCacheModeDefault = 0,
	kCacheModeWC = 0,
};

#define p2v(PA) ((vaddr_t)(PA) + HHDM_BASE)
#define v2p(VA) ((paddr_t)(VA) - HHDMBASE)

#endif /* ECX_KEYRONEX_VM_ARCH_H */
