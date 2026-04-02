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

#include <sys/vm_arch.h>
#include <sys/vm_types.h>
#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#define VM_MAX_AFFINITIES 64
#define VM_MAX_DOMAINS 8

struct vnode;

typedef struct vm_map vm_map_t;
typedef struct vm_object vm_object_t;

typedef enum vm_page_use {
	VM_PAGE_DELETED,
	VM_PAGE_FREE,
	VM_PAGE_DEV_BUFFER,
	VM_PAGE_KWIRED,
	VM_PAGE_TABLE,
	VM_PAGE_PRIVATE,
	VM_PAGE_ANON_SHARED,
	VM_PAGE_ANON_FORKED,
	VM_PAGE_FILE,
	VM_PAGE_OBJ_TABLE,
	VM_PAGE_USE_N,
} vm_page_use_t;

typedef enum vm_alloc_flags {
	VM_SLEEP,
	VM_NOFAIL,
	VM_EXACT,
} vm_alloc_flags_t;

typedef enum vm_cache_mode vm_cache_mode_t;

struct vm_voaddr {
	uintptr_t object;
	size_t offset;
	bool private;
};

size_t vm_npages_to_order(size_t npages);
size_t vm_bytes_to_order(size_t bytes);

void *vm_kwired_alloc(size_t npages, vm_alloc_flags_t);
void vm_kwired_free(void *addr, size_t npages);
void vm_kwired_init(void);

int vm_k_map_phys(vaddr_t *p_vaddr, paddr_t pa, size_t size, vm_cache_mode_t);

void vm_kmap_init(void);

vm_map_t *vm_map_create(void);
void vm_map_release(vm_map_t *map);
int vm_fork(vm_map_t *parent, vm_map_t *child);

int vm_allocate(vm_map_t *map, vm_prot_t prot, vaddr_t *vaddrp, size_t size,
    bool exact);
int vm_map(vm_map_t *map, vm_object_t *object, vaddr_t *vaddrp, size_t size,
    uint64_t obj_offset, vm_prot_t initial_prot, vm_prot_t max_prot,
    bool inherit_shared, bool copy, bool exact);
int vm_map_phys(vm_map_t *map, paddr_t paddr, vaddr_t *vaddrp, size_t size,
    vm_prot_t prot, vm_cache_mode_t cache, bool exact);
int vm_unmap(struct vm_map *map, vaddr_t start, vaddr_t end);

int vm_voaddr_acquire(struct vm_map *, vaddr_t, struct vm_voaddr *out);
void vm_voaddr_release(struct vm_map *, struct vm_voaddr *);
intptr_t vm_voaddr_cmp(const struct vm_voaddr *a, const struct vm_voaddr *b);

vm_object_t *vm_obj_new_vnode(struct vnode *);
void vm_vnobj_set_valid_length(vm_object_t *, size_t);

vm_page_t *vm_page_alloc(vm_page_use_t, size_t order, vm_domid_t,
    vm_alloc_flags_t);
void vm_page_delete(vm_page_t *page, bool unref);

vaddr_t vm_page_hhdm_addr(vm_page_t *page);
paddr_t vm_page_paddr(vm_page_t *page);

paddr_t vm_translate(vaddr_t);

void *sys_mmap(void *addr, size_t len, int prot, int flags, int fildes,
    off_t offset);

#endif /* ECX_KEYRONEX_VM_H */
