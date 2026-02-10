/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm.h
 * @brief Virtual memory.
 */

#ifndef ECX_KEYRONEX_VM_H
#define ECX_KEYRONEX_VM_H

#include <keyronex/vm_arch.h>

#include <stdint.h>

#define VM_MAX_AFFINITIES 64
#define VM_MAX_DOMAINS 8

typedef enum vm_page_use {
	VM_PAGE_DELETED,
	VM_PAGE_FREE,
	VM_PAGE_DEV_BUFFER,
	VM_PAGE_KWIRED,
	VM_PAGE_TABLE,
	VM_PAGE_PRIVATE,
	VM_PAGE_ANON_SHARED,
	VM_PAGE_ANON_FORKED,
	VM_PAGE_FILE,
	VM_PAGE_OBJ_TABLE,
	VM_PAGE_USE_N,
} vm_page_use_t;

typedef enum vm_alloc_flags {
	VM_SLEEP,
	VM_NOFAIL,
} vm_alloc_flags_t;

typedef enum vm_cache_mode vm_cache_mode_t;

size_t vm_npages_to_order(size_t npages);
size_t vm_bytes_to_order(size_t bytes);

#endif /* ECX_KEYRONEX_VM_H */
