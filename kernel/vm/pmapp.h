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

/*! @brief Retain a reference (preventing paging out) to a page. */
void vmp_page_retain(vm_page_t *page);

/*! @brief Release a reference to a page. */
void vmp_page_release(vm_page_t *page);

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

/*! @brief Get the page to which a transition PTE points. */
vm_page_t *pte_trans_get_page(pte_t *pte);

/*! @brief Test if a PTE is for outpaged memory. */
vaddr_t pte_is_outpaged(pte_t *pte);

/*! @brief Return whether this PTE points to a prototype PTE.*/
bool pte_denotes_proto(pte_t *pte);

/*! @brief Test if a PTE is an HW PTE. */
bool pte_is_hw(pte_t *pte);

/*! @brief Test if a PTE is a transition PTE. */
vaddr_t pte_is_transition(pte_t *pte);

/*! @brief Release a reference to a paging state object. */
void vmp_paging_state_release(struct vmp_paging_state *pstate);

/*! @brief Retain a reference to a paging state object. */
void vmp_paging_state_retain(struct vmp_paging_state *pstate);

/*! @brief Wait for a paging operation to complete. */
void vmp_paging_state_wait(struct vmp_paging_state *pstate);

#endif /* KRX_VM_PMAPP_H */
