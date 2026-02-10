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

typedef struct kmem_cache kmem_cache_t;

enum kmem_alloc_flags {
	KM_SLEEP = 1,
	KM_NOFAIL = 2,
};

kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align,
    void (*ctor)(void *));
void *kmem_cache_alloc(kmem_cache_t *cache, enum kmem_alloc_flags flags);
void kmem_cache_free(kmem_cache_t *cache, void *ptr);

void *kmem_alloc(size_t size, enum kmem_alloc_flags flags);
void *kmem_zalloc(size_t size, enum kmem_alloc_flags flags);
void *kmem_realloc(void *ptr, size_t old_size, size_t size,
    enum kmem_alloc_flags flags);
void kmem_free(void *ptr, size_t size);

void *kmem_malloc(size_t size);
void kmem_mfree(void *ptr);
void kmem_mfree_sizeverify(void *ptr, size_t size);
void *kmem_mrealloc(void *ptr, size_t size);
void *kmem_calloc(size_t nmemb, size_t size);

int kmem_vasprintf(char **strp, const char *fmt, va_list ap);
int kmem_asprintf(char **str, const char *fmt, ...);


#endif /* ECX_KEYRONEX_KMEM_H */
