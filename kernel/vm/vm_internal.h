/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 14 2023.
 */

#ifndef KRX_VM_VM_INTERNAL_H
#define KRX_VM_VM_INTERNAL_H

#include "kdk/vm.h"

struct vmp_paging_state {
	kevent_t event;
};

/*! @brief Enter a page mapping. */
void pmap_enter(vm_procstate_t *vmps, paddr_t phys, vaddr_t virt,
    vm_protection_t prot);

/*! @brief Remove a page mapping, returning the page previously mapped. */
vm_page_t *pmap_unenter(vm_procstate_t *vmps, vaddr_t vaddr);

/*! @brief Translate virtual address to physical. */
paddr_t pmap_trans(vm_procstate_t *vmps, vaddr_t virt);

/*! @brief Locally invalidate a mapping. */
void pmap_invlpg(vaddr_t vaddr);

/*!
 * @brief Reduce protections on all mappings within some range of memory.
 */
void pmap_protect_range(vm_procstate_t *vmps, vaddr_t base, vaddr_t limit);

/*!
 * @brief Check if a page is present in a process.
 * @param paddr if this is non-NULL and the page is present, the page's physical
 * address will be written here.
 */
bool pmap_is_present(vm_procstate_t *vmps, vaddr_t vaddr, paddr_t *paddr);

/*!
 * @brief Check if a page is writeably mapped in a process.
 * @param paddr if this is non-NULL and the page is writeably mapped, the page's
 * physical address will be written here.
 */
bool pmap_is_writeable(vm_procstate_t *vmps, vaddr_t vaddr, paddr_t *paddr);

/*! @brief Initialise a process' working set list. */
void vmp_wsl_init(vm_procstate_t *vmps);

/*!
 * @brief Adds a mapping to the working set list.
 *
 * This function adds a mapping entry to to the working set list.
 * If the working set list is below its maximal size, and there is no low-memory
 * condition, it will be appended.
 * Otherwise, if the working set list is at its maximum, it will try to expand
 * its size and add the entry.
 * If the expansion fails, the function will dispose of the least recently added
 * entry in the working set list and add the new entry in its place.
 * The page MUST be already mapped in the page tables.
 *
 * @param ws Pointer to the process vm state.
 * @param entry The virtual address entry to add to the working set list.
 */
void vmp_wsl_insert(vm_procstate_t *vmps, vaddr_t entry);

/*!
 * @brief Removes a mapping from a working set list.
 *
 * This functions removes a mapping entry from a working set list.
 * The physical mapping will be abolished, a TLB shootdown issued if necessary,
 * and the page will have its reference count dropped.
 *
 */
void vmp_wsl_remove(vm_procstate_t *vmps, vaddr_t entry);

/*! @brief Remove any mappings within a range from a working set list. */
void vmp_wsl_remove_range(vm_procstate_t *vmps, vaddr_t start, vaddr_t end);

/**
 * @brief Trims a specified number of pages from a working set list.
 *
 * This function removes a specified number of pages, starting with the least
 * recently used, from a working set list. If the number of entries to be
 * trimmed is equal to the current size of the working set list, then all the
 * entries will be disposed.
 *
 * @param ws Pointer to the process vm state.
 * @param n Number of entries to be trimmed.
 */
void vmp_wsl_trim_n_entries(vm_procstate_t *vmps, size_t n);

/*!
 * @brief Find the VAD in a process to which a virtual address belongs.
 *
 * @pre VAD mutex locked.
 */
vm_vad_t *vmp_ps_vad_find(vm_procstate_t *ps, vaddr_t vaddr);

/*! @brief Comparator function for VAD rb-tree. */
int vmp_vad_cmp(vm_vad_t *x, vm_vad_t *y);
/*! @brief Comparator function for vpage rb-tree. */
int vmp_vpage_cmp(struct vmp_vpage *x, struct vmp_vpage *y);

RB_PROTOTYPE(vm_vad_rbtree, vm_vad, rb_entry, vmp_vad_cmp);
RB_PROTOTYPE(vmp_vpage_rb, vmp_vpage, rb_entry, vmp_vpage_cmp);

#endif /* KRX_VM_VM_INTERNAL_H */
