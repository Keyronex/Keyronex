/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2020-2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

/*!
 * @file kmem.h
 * @brief Implementation of a slab allocator and a generic kmalloc in terms of
 * it.
 */

#ifndef KRX_KDK_KMEM_H
#define KRX_KDK_KMEM_H

#include "./vmem.h"
#ifndef _KERNEL
#include <sys/queue.h>
#define mutex_t int
#else
#include "./kern.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * A KMem zone - provides slab allocation for a particular size of object. In
 * the future it will incorporate per-CPU caches.
 */
typedef struct kmem_zone {
	/*! linkage for kmem_zones */
	STAILQ_ENTRY(kmem_zone) zonelist;
	/*! identifier name */
	const char *name;
	/*! size of contained objects */
	size_t size;
	/*! queue of slabs */
	STAILQ_HEAD(, kmem_slab) slablist;
	/*! locking */
	kspinlock_t lock;

	/*! the below are applicable only to large slabs */
	/*! list of allocated bufctls, used TODO(med): Use a hash table? */
	SLIST_HEAD(, kmem_bufctl) bufctllist;
} kmem_zone_t;

STAILQ_HEAD(kmem_zones, kmem_zone);

/*! Initialise the KMem system. */
void kmem_init(void);

/*! Dump information about all zones. */
void kmem_dump(void);

/*!
 * Initialise a new zone.
 *
 * @param name zone identifier. This is assumed to be a constant string, so it
 * is not copied.
 * @param size size of the objects which it will hold.
 */
void kmem_zone_init(struct kmem_zone *zone, const char *name, size_t size);

/*!
 * Allocate from a zone.
 */
void *kmem_xzonealloc(kmem_zone_t *zone, enum vmem_flag flags);

/*!
 * Release memory previously allocated with kmem_zonealloc().
 */
void kmem_xzonefree(kmem_zone_t *zone, void *ptr, enum vmem_flag flags);

/*!
 * Allocate kernel wired memory. Memory will be aligned to zone's size (thus
 * power-of-2 allocations will be naturally aligned).
 */
void *kmem_xalloc(size_t size, enum vmem_flag flags)
    __attribute__((alloc_size(1)));

/*!
 * Release memory allocated by kmem_alloc(). \p size must match the size that
 * was allocated by kmem_alloc or kmem_realloc.
 */
void kmem_xfree(void *ptr, size_t size, enum vmem_flag flags);

void *kmem_xrealloc(void *ptr, size_t oldSize, size_t size,
    enum vmem_flag flags);

/*!
 * Allocate zeroed memory from a zone.
 */
void *kmem_zalloc(size_t size);

/*!
 * Malloc compatibility function.
 */
void *kmem_malloc(size_t size) __attribute__((alloc_size(1)));
void kmem_mfree(void *ptr);
void *kmem_mrealloc(void *ptr, size_t size);
void *kmem_calloc(size_t nmemb, size_t size);

/*!
 * As asprintf, but use kmem backing.
 */
int kmem_asprintf(char **str, const char *fmt, ...);

/*!
 * Free a null-terminated string. The string **must** be null-terminated at
 * the end of its allocation, and nowhere else.
 * @returns NULL
 */
void *kmem_strfree(char *str);


#define kmem_zonealloc(zone) kmem_xzonealloc(zone, (enum vmem_flag)0)
#define kmem_zonefree(zone, ptr) kmem_xzonefree(zone, ptr, (enum vmem_flag)0)
#define kmem_alloc(size) kmem_xalloc(size, (enum vmem_flag)0)
#define kmem_free(ptr, size) kmem_xfree(ptr, size, (enum vmem_flag)0)
#define kmem_realloc(ptr, oldsize, size) \
	kmem_xrealloc(ptr, oldsize, size, (enum vmem_flag)0)

extern struct kmem_zones kmem_zones;

#ifdef __cplusplus
} /* extern "C" */
#endif


#endif /* KRX_KDK_KMEM_H */
