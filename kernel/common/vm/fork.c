/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file fork.c
 * @brief Virtual memory fork operation.
 */

#include <sys/k_log.h>

#include <vm/map.h>
#include <vm/page.h>

kspinlock_t anon_creation_lock;
kspinlock_t anon_stealing_lock;

int
vm_fork(vm_map_t *src_map, vm_map_t *dst_map)
{
	kfatal("Implement me!");
}
