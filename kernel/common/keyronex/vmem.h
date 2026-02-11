/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2024 Cloudarox Solutions.
 */
/*!
 * @file vmem.h
 * @brief VMem resource allocator definitions.
 */

#ifndef ECX_KDK_VMEM_H
#define ECX_KDK_VMEM_H

#include <keyronex/vm.h>

#include <stddef.h>
#include <stdint.h>

typedef uintptr_t vmem_addr_t;
typedef size_t vmem_size_t;
typedef struct vmem vmem_t;

typedef int (*vmem_alloc_t)(vmem_t *vmem, vmem_size_t size,
    vm_alloc_flags_t flags, vmem_addr_t *out);
typedef void (*vmem_free_t)(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size,
    vm_alloc_flags_t flags);

/* Create a new VMem arena. */
vmem_t *vmem_init(vmem_t *vmem, const char *name, vmem_addr_t base,
    vmem_size_t size, vmem_size_t quantum, vmem_alloc_t allocfn,
    vmem_free_t freefn, vmem_t *source, size_t qcache_max,
    vm_alloc_flags_t flags);
/* Destroy a VMem arena. (Does not free it; that must be done manually.) */
void vmem_destroy(vmem_t *vmem);

int vmem_xalloc(vmem_t *vmem, vmem_size_t size, vmem_size_t align,
    vmem_size_t phase, vmem_size_t nocross, vmem_addr_t min, vmem_addr_t max,
    vm_alloc_flags_t flags, vmem_addr_t *out);
int vmem_xfree(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size,
    vm_alloc_flags_t flags);
int vmem_xrealloc(vmem_t *vmem, vmem_addr_t addr, vmem_size_t oldsize,
    vmem_size_t newsize, vm_alloc_flags_t flags, vmem_addr_t *out);

void vmem_global_init(void);
void vmem_dump(const vmem_t *vmem);

#endif /* ECX_KDK_VMEM_H */
