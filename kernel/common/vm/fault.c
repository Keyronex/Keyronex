/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file fault.c
 * @brief Virtual memory fault handling.
 */

#include <sys/k_log.h>

#include <vm/map.h>
#include <vm/page.h>

void
vm_fault(vaddr_t va, vm_prot_t prot)
{
	kfatal("VM fault on address 0x%zx, prot 0x%x", va, prot);
}
