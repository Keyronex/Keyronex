/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file kmem.h
 * @brief Kernel memory allocator (slab).
 */

#ifndef ECX_KEYRONEX_KMEM_H
#define ECX_KEYRONEX_KMEM_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include <keyronex/vm.h>

typedef struct kmem_cache kmem_cache_t;

void kmem_init(void);
void kmem_postsmp_init(void);

kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align,
    void (*ctor)(void *));
void *kmem_cache_alloc(kmem_cache_t *cache, vm_alloc_flags_t);
void kmem_cache_free(kmem_cache_t *cache, void *ptr);

void *kmem_xalloc(size_t size, vm_alloc_flags_t);
void *kmem_xzalloc(size_t size, vm_alloc_flags_t);
void *kmem_xrealloc(void *ptr, size_t old_size, size_t size, vm_alloc_flags_t);
void kmem_xfree(void *ptr, size_t size);

#define kmem_alloc(size) kmem_xalloc(size, 0)
#define kmem_zalloc(size) kmem_xzalloc(size, 0)
#define kmem_free(ptr, size) kmem_xfree(ptr, size)
#define kmem_realloc(ptr, old_size, size) kmem_xrealloc(ptr, old_size, size, 0)

void *kmem_malloc(size_t size);
void kmem_mfree(void *ptr);
void kmem_mfree_sizeverify(void *ptr, size_t size);
void *kmem_mrealloc(void *ptr, size_t size);
void *kmem_mcalloc(size_t nmemb, size_t size);

int kmem_vasprintf(char **strp, const char *fmt, va_list ap);
int kmem_asprintf(char **str, const char *fmt, ...);

#endif /* ECX_KEYRONEX_KMEM_H */
