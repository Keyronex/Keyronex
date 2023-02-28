/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2020-2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

/**
 * @file vmem.h
 * @brief Public interface to the VMem resource allocator. See vmem.c for
 * detailed description of VMem.
 */

#ifndef KRX_KDK_VMEM_H
#define KRX_KDK_VMEM_H

#include <stddef.h>
#include <stdint.h>

#ifndef _KERNEL
typedef int ipl_t;
#else
#include ".//machdep.h"
#endif

typedef uintptr_t   vmem_addr_t;
typedef size_t	    vmem_size_t;
typedef struct vmem vmem_t;

typedef enum vmem_flag {
	/*! It is acceptable for the allocation to sleep. */
	kVMemSleep = 0x1,
	/*! The allocation must succeed. */
	kVMemMust = 0x2,
	/*! The requested address should be allocated. */
	kVMemExact = 0x2,
	/*! @private VMem to use statically allocated segments */
	kVMemBootstrap = 0x4,
	/*! @private PFNDB lock is already held. */
	kVMemPFNDBHeld = 0x8,
} vmem_flag_t;

typedef int (*vmem_alloc_t)(vmem_t *vmem, vmem_size_t size, vmem_flag_t flags,
    vmem_addr_t *out);
typedef void (*vmem_free_t)(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size, vmem_flag_t flags);

/** Create a new VMem arena. */
vmem_t *vmem_init(vmem_t *vmem, const char *name, vmem_addr_t base,
    vmem_size_t size, vmem_size_t quantum, vmem_alloc_t allocfn,
    vmem_free_t freefn, vmem_t *source, size_t qcache_max, vmem_flag_t flags,
    ipl_t ipl);
/** Destroy a VMem arena. (Does not free it; that must be done manually.) */
void vmem_destroy(vmem_t *vmem);

int vmem_xalloc(vmem_t *vmem, vmem_size_t size, vmem_size_t align,
    vmem_size_t phase, vmem_size_t nocross, vmem_addr_t min, vmem_addr_t max,
    vmem_flag_t flags, vmem_addr_t *out);

int vmem_xfree(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size, vmem_flag_t flags);

void vmem_dump(const vmem_t *vmem);

#endif /* KRX_KDK_VMEM_H */
