/*
 * Copyright 2020-2025 NetaScale Object Solutions.
 * All rights reserved.
 */
/*!
 * @file kmem.h
 * @brief Implementation of a slab allocator and a generic kmalloc in terms of
 * it.
 */

#ifndef KRX_KDK_KMEM_H
#define KRX_KDK_KMEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <kdk/vmem.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct kmem_cache kmem_cache_t;

void kmem_init(void);

kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align,
    void (*ctor)(void *));
void *kmem_cache_alloc(kmem_cache_t *cache, vmem_flag_t flags);
void kmem_cache_free(kmem_cache_t *cache, void *ptr);

void *kmem_xalloc(size_t size, vmem_flag_t flags);
void *kmem_xzalloc(size_t size, vmem_flag_t flags);
void *kmem_xrealloc(void *ptr, size_t old_size, size_t size, vmem_flag_t flags);
void kmem_xfree(void *ptr, size_t size);

#define kmem_alloc(size) kmem_xalloc(size, 0)
#define kmem_zalloc(size) kmem_xzalloc(size, 0)
#define kmem_free(ptr, size) kmem_xfree(ptr, size)
#define kmem_realloc(ptr, old_size, size) kmem_xrealloc(ptr, old_size, size, 0)

void *kmem_malloc(size_t size);
void kmem_mfree(void *ptr);
void kmem_mfree_sizeverify(void *ptr, size_t size);
void *kmem_mrealloc(void *ptr, size_t size);
void *kmem_calloc(size_t nmemb, size_t size);

int kmem_vasprintf(char **strp, const char *fmt, va_list ap);
int kmem_asprintf(char **str, const char *fmt, ...);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_KDK_KMEM_H */
