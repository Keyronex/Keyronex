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

#include <stdint.h>

typedef uintptr_t vaddr_t;
typedef uintptr_t paddr_t;

/* todo move me */
#define HHDM_BASE   0xffff800000000000

#define p2v(PA) ((vaddr_t)(PA) + HHDM_BASE)
#define v2p(VA) ((paddr_t)(VA) - HHDMBASE)

#endif /* ECX_KEYRONEX_VM_H */
