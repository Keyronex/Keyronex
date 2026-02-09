/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm.h
 * @brief Virtual memory.
 */

#ifndef ECX_KEYRONEX_VM_TYPES_H
#define ECX_KEYRONEX_VM_TYPES_H

#include <stdint.h>

typedef struct vm_page vm_page_t;

typedef uintptr_t vaddr_t;
typedef uintptr_t paddr_t;

typedef uint8_t vm_domid_t;

#define VM_DOMID_ANY ((vm_domid_t)-1)
#define VM_DOMID_LOCAL ((vm_domid_t)-2)

#endif /* ECX_KEYRONEX_VM_TYPES_H */
