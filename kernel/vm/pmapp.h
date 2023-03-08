/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 08 2023.
 */
/*!
 * @file vm/pmapp.h
 * @brief Private physical map.
 */

#ifndef KRX_VM_PMAPP_H
#define KRX_VM_PMAPP_H

#include "kdk/vm.h"

/*!
 * @brief Return a wired pointer to the PTE for an virtual address, or NULL.
 *
 * If the given virtual address is mapped by a PTE in this address space, then
 * this returns a pointer to that PTE, and pins the PTE in memory. Otherwise,
 * it returns NULL.
 *
 */
pte_t *pte_get_and_pin(vm_procstate_t *vmps, vaddr_t vaddr);

/*! @brief Get the address to which an HW PTE points. */
paddr_t pte_hw_get_addr(pte_t *pte);

/*! @brief Get the vm_page of the address to which an HW PTE points. */
vm_page_t *pte_hw_get_page(pte_t *pte);

/*! @brief Get the address to which a software PTE points. */
vaddr_t pte_sw_get_addr(pte_t *pte);

/*! @brief Return whether this PTE is a fork PTE. (May be HW or SW PTE.) */
bool pte_is_fork(pte_t *pte);

/*! @brief Return whether this PTE is an HW PTE. */
bool pte_is_hw(pte_t *pte);

#endif /* KRX_VM_PMAPP_H */
