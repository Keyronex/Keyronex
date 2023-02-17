/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 14 2023.
 */

#ifndef MLX_VM_VM_INTERNAL_H
#define MLX_VM_VM_INTERNAL_H

#include "vm/vm.h"

/*! @brief Enter a page mapping. */
void pmap_enter(vm_procstate_t *vmps, paddr_t phys, vaddr_t virt,
    vm_protection_t prot);
/*! @brief Remove a page mapping, returning the page previously mapped. */
vm_page_t *pmap_unenter(vm_procstate_t *vmps, vaddr_t vaddr);

/*!
 * @brief Find the VAD in a process to which a virtual address belongs.
 *
 * @pre VAD mutex locked.
 */
vm_vad_t *vi_ps_vad_find(vm_procstate_t *ps, vaddr_t vaddr);

#endif /* MLX_VM_VM_INTERNAL_H */
