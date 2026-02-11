/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2022-2026 Cloudarox Solutions.
 */
/*
 * @file kwired.c
 * @brief Kernel wired memory allocator.
 */

#include <keyronex/dlog.h>
#include <keyronex/intr.h>
#include <keyronex/vm.h>
#include <keyronex/vmem.h>
#include <keyronex/vmem_impl.h>

#include "vm/map.h"

kspinlock_t kwired_lock = KSPINLOCK_INITIALISER;
vmem_t kwired_arena;

void
vm_kwired_init(void)
{
	vmem_init(&kwired_arena, "kernel-wired-heap", PIN_HEAP_BASE,
	    PIN_HEAP_SIZE, PGSIZE, NULL, NULL, NULL, 0, 0);
}

void *
vm_kwired_alloc(size_t npages, vm_alloc_flags_t flags)
{
	kfatal("implement me");
}

void
vm_kwired_free(void *ptr, size_t npages)
{
	kfatal("implement me");
}
