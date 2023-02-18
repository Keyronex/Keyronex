/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 14 2023.
 */

#ifndef MLX_VM_VM_INTERNAL_H
#define MLX_VM_VM_INTERNAL_H

#include "vm/vm.h"

/*!
 *
 */
struct vmp_page_ref {
	/*!
	 * Where in the section does it belong to?
	 */
	size_t page_index;
	union {
		/*! Page reference (used by file sections) */
		vm_page_t *page;
		/*! Vpage reference (used by anonymous sections)*/
		vm_vpage_t *vpage;
	};
};

/*! @brief Enter a page mapping. */
void pmap_enter(vm_procstate_t *vmps, paddr_t phys, vaddr_t virt,
    vm_protection_t prot);
/*! @brief Remove a page mapping, returning the page previously mapped. */
vm_page_t *pmap_unenter(vm_procstate_t *vmps, vaddr_t vaddr);

/*!
 * @brief Adds a virtual address to a working set list.
 *
 * This function adds a virtual address entry to the working set list.
 * If the working set list is below its maximal size, and there is no low-memory
 * condition, it will be appended.
 * Otherwise, if the working set list is at its maximum, it will try to expand
 * its size and add the entry.
 * If the expansion fails, the function will dispose of the least recently added
 * entry in the working set list and add the new entry in its place.
 *
 * @param ws Pointer to the process vm state.
 * @param entry The virtual address entry to add to the working set list.
 */
void mi_wsl_insert(vm_procstate_t *vmps, vaddr_t entry);

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
void mi_wsl_trim_n_entries(vm_procstate_t *vmps, size_t n);

/*!
 * @brief Find the VAD in a process to which a virtual address belongs.
 *
 * @pre VAD mutex locked.
 */
vm_vad_t *vi_ps_vad_find(vm_procstate_t *ps, vaddr_t vaddr);

/*! @brief Comparator function for VAD rb-tree. */
int vi_vad_cmp(vm_vad_t *x, vm_vad_t *y);
/*! @brief Comparator function for page ref rb-tree. */
int vi_page_ref_cmp(struct vmp_page_ref *x, struct vmp_page_ref *y);

RB_PROTOTYPE(vm_vad_rbtree, vm_vad, rbtree_entry, vi_vad_cmp);
RB_PROTOTYPE(vmp_page_ref_rbtree, vmp_page_ref, rbtree_entry, vi_page_ref_cmp);

#endif /* MLX_VM_VM_INTERNAL_H */
