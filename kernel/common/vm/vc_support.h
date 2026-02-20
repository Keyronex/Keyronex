/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vc_support.h
 * @brief Viewcache support interface.
 */

#ifndef ECX_VM_VC_SUPPORT_H
#define ECX_VM_VC_SUPPORT_H

#include <sys/vm.h>

void vm_vc_unmap(vaddr_t addr, size_t size);
void vm_vc_clean(vm_object_t *vmobj, size_t offset, vaddr_t addr, size_t size);

#endif /* ECX_VM_VC_SUPPORT_H */
