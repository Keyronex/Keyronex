/*
 * Copyright (c) 2020-2025 NetaScale Object Solutions.
 */
/*
 * @file kmem.c
 * @brief kmem_alloc allocator & kmem_cache_alloc(9) slab allocator
 * implementation.
 */

#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include <kdk/misc.h>
#include <kdk/queue.h>
#include <kdk/vm.h>
#include <stddef.h>

#include "vm/vmp.h"

// #define KMEM_SANITY_CHECKING 1

#define SMALL_SLAB_MAX 512

struct kmem_bufctl {
	SLIST_ENTRY(kmem_bufctl) sllink;
	struct kmem_slab *slab;
	void *base;
};

struct kmem_slab {
	STAILQ_ENTRY(kmem_slab) sqlink;
	kmem_cache_t *cache;
	uint16_t free_n;
	uint16_t alloced_n;
	SLIST_HEAD(, kmem_bufctl) free;
};

struct vm_slab_page {
	uint16_t vm_page_flags;
	uintptr_t padding[3];
	struct kmem_slab slab;
};

_Static_assert(sizeof(struct vm_slab_page) == sizeof(vm_page_t),
    "struct vm_slab_page size not equal to struct vm_page");

struct kmem_cache {
	char name[32];
	kspinlock_t spinlock;
	size_t size;
	size_t align;
	void (*ctor)(void *);
	STAILQ_HEAD(, kmem_slab) slabs;
	SLIST_HEAD(, kmem_bufctl) alloced;
	TAILQ_ENTRY(kmem_cache) qlink;
};

static TAILQ_HEAD(, kmem_cache) all_caches = TAILQ_HEAD_INITIALIZER(all_caches);
static kspinlock_t all_caches_lock = KSPINLOCK_INITIALISER;
static kmem_cache_t kmem_alloc_caches[10], kmem_bufctl_cache, kmem_slab_cache,
    kmem_cache_cache;

void
kmem_cache_init(kmem_cache_t *cache, const char *name, size_t size,
    size_t align, void (*ctor)(void *))
{
	ipl_t ipl;
	strncpy(cache->name, name, sizeof(cache->name));
	cache->size = size;
	cache->align = align;
	cache->ctor = ctor;

	STAILQ_INIT(&cache->slabs);
	SLIST_INIT(&cache->alloced);

	ke_spinlock_init(&cache->spinlock);

	ipl = ke_spinlock_acquire(&all_caches_lock);
	TAILQ_INSERT_TAIL(&all_caches, cache, qlink);
	ke_spinlock_release(&all_caches_lock, ipl);
}

kmem_cache_t *
kmem_cache_create(const char *name, size_t size, size_t align,
    void (*ctor)(void *))
{
	kmem_cache_t *cache;

	cache = kmem_xalloc(sizeof(kmem_cache_t), 0);
	if (cache == NULL)
		return NULL;

	kmem_cache_init(cache, name, size, align, ctor);

	return cache;
}

void
kmem_init(void)
{
	for (size_t i = 0; i < 10; i++) {
		char name[32];
		npf_snprintf(name, sizeof(name), "kmem_alloc_%zu", i);
		kmem_cache_init(&kmem_alloc_caches[i], name, 8 << i, 8 << i,
		    NULL);
	}
	kmem_cache_init(&kmem_bufctl_cache, "kmem_bufctl",
	    sizeof(struct kmem_bufctl), __alignof(struct kmem_bufctl), NULL);
	kmem_cache_init(&kmem_slab_cache, "kmem_slab", sizeof(struct kmem_slab),
	    __alignof(struct kmem_slab), NULL);
	kmem_cache_init(&kmem_cache_cache, "kmem_cache", sizeof(kmem_cache_t),
	    __alignof(kmem_cache_t), NULL);
}

#define roundup2(A, B) ROUNDUP(A, B)

/* return the size in bytes held in a slab of a given zone*/
static size_t
slab_size(kmem_cache_t *zone)
{
	if (zone->size <= SMALL_SLAB_MAX)
		return PGSIZE;
	else
		/* aim for at least 16 entries */
		return roundup2(zone->size * 16, PGSIZE);
}

/* return the capacity in number of objects of a slab of this zone */
static size_t
slab_elems_per(kmem_cache_t *zone)
{
	if (zone->size <= SMALL_SLAB_MAX)
		return PGSIZE / zone->size;
	else
		return slab_size(zone) / zone->size;
}

static inline size_t
zonenum(size_t size)
{
	kassert(size > 0);
	if (size <= 8)
		return 0;
	if (size <= 16)
		return 1;
	if (size <= 32)
		return 2;
	if (size <= 64)
		return 3;
	if (size <= 128)
		return 4;
	if (size <= 256)
		return 5;
	if (size <= 512)
		return 6;
	if (size <= 1024)
		return 7;
	if (size <= 2048)
		return 8;
#if 0
	 if (size <= 4096)
		 return 9;
#endif
	return -1;
}

void *
kmem_xalloc(size_t size, vmem_flag_t flags)
{
	size_t zoneidx = zonenum(size);

	if (zoneidx != -1) {
		return kmem_cache_alloc(&kmem_alloc_caches[zoneidx], flags);
	} else {
		return (void*)vm_kalloc(roundup2(size, PGSIZE) / PGSIZE, flags);
	}
}

void *
kmem_xzalloc(size_t size, vmem_flag_t flags)
{
	void *ptr = kmem_xalloc(size, flags);
	if (ptr != NULL)
		memset(ptr, 0, size);
	return ptr;
}

void *
kmem_xrealloc(void *ptr, size_t old_size, size_t size,
    vmem_flag_t flags)
{
	size_t old_zone, new_zone;
	void *new;

	if (ptr == NULL) {
		return kmem_xalloc(size, flags);
	} else if (size == 0) {
		kmem_free(ptr, old_size);
		return NULL;
	} else if (size == old_size) {
		return ptr;
	}

	old_zone = zonenum(old_size);
	new_zone = zonenum(size);

	if (old_zone != (size_t)-1 && new_zone != (size_t)-1 &&
	    old_zone == new_zone) {
		return ptr;
	}

	new = kmem_xalloc(size, flags);
	if (new == NULL)
		return NULL;

	memcpy(new, ptr, MIN2(old_size, size));

	/* Free the old allocation */
	kmem_free(ptr, old_size);

	return new;
}

void
kmem_xfree(void *ptr, size_t size)
{
	size_t zoneidx;

	if (ptr == NULL)
		return;

	zoneidx = zonenum(size);

	if (zoneidx != -1) {
		kmem_cache_free(&kmem_alloc_caches[zoneidx], ptr);
	} else {
		vm_kfree((vaddr_t)ptr, roundup2(size, PGSIZE) / PGSIZE, 0);
	}
}

static struct kmem_slab *
small_slab_new(kmem_cache_t *cache, vmem_flag_t flags)
{
	vm_page_t *page;
	struct vm_slab_page *slab_page;
	struct kmem_slab *slab;
	vaddr_t base;

	vm_page_alloc(&page, 0, kPageUseKWired, 0);
	if (page == NULL)
		return NULL;

	slab_page = (struct vm_slab_page *)page;
	slab = &slab_page->slab;
	base = vm_page_direct_map_addr(page);

	slab->cache = cache;
	slab->free_n = PGSIZE / cache->size;
	slab->alloced_n = 0;
	SLIST_INIT(&slab->free);

	for (size_t i = 0; i < slab->free_n; i++) {
		struct kmem_bufctl *bufctl = (struct kmem_bufctl *)(base +
		    i * cache->size);
		SLIST_INSERT_HEAD(&slab->free, bufctl, sllink);
	}

	return slab;
}

static struct kmem_slab *
large_slab_new(kmem_cache_t *cache, vmem_flag_t flags)
{
	struct kmem_slab *slab;
	void *base;

	base = (void *)vm_kalloc(slab_size(cache) / PGSIZE, flags);
	if (base == NULL)
		return NULL;

	slab = kmem_cache_alloc(&kmem_slab_cache, flags);
	if (slab == NULL)
		kfatal("Handle this case\n");

	slab->cache = cache;
	slab->free_n = slab_elems_per(cache);
	slab->alloced_n = 0;
	SLIST_INIT(&slab->free);

	for (size_t i = 0; i < slab->free_n; i++) {
		struct kmem_bufctl *bufctl;

		bufctl = kmem_cache_alloc(&kmem_bufctl_cache,
		    flags);
		bufctl->slab = slab;
		bufctl->base = (void *)((uintptr_t)base + i * cache->size);
		SLIST_INSERT_HEAD(&slab->free, bufctl, sllink);
	}

	return slab;
}

void *
kmem_cache_alloc(kmem_cache_t *cache, vmem_flag_t flags)
{
	ipl_t ipl;
	struct kmem_slab *slab;
	struct kmem_bufctl *bufctl;
	void *ret;

	ipl = ke_spinlock_acquire(&cache->spinlock);

	slab = STAILQ_FIRST(&cache->slabs);
	if (slab == NULL || slab->free_n == 0) {
		if (cache->size > SMALL_SLAB_MAX)
			slab = large_slab_new(cache, flags);
		else
			slab = small_slab_new(cache, flags);

		if (slab == NULL) {
			ke_spinlock_release(&cache->spinlock, ipl);
			return NULL;
		}

		STAILQ_INSERT_HEAD(&cache->slabs, slab, sqlink);
	}

	bufctl = SLIST_FIRST(&slab->free);

#if KMEM_SANITY_CHECKING
	kassert(slab->free_n + slab->alloced_n == slab_elems_per(cache));
	if (cache->size <= SMALL_SLAB_MAX) {
		kassert((void *)bufctl >= (void *)HHDM_BASE &&
		    (void *)bufctl < (void *)(HHDM_BASE + HHDM_SIZE));
	} else {
		kassert(bufctl->base >= (void *)KVM_WIRED_BASE &&
		    bufctl->base < (void *)(KVM_WIRED_BASE + KVM_WIRED_SIZE));
		kassert(bufctl->slab == slab);
	}
#endif

	slab->alloced_n++;
	slab->free_n--;
	SLIST_REMOVE_HEAD(&slab->free, sllink);

	if (slab->free_n == 0) {
#if KMEM_SANITY_CHECKING
		kassert(SLIST_EMPTY(&slab->free));
#endif
		/* push this empty slab to the back of the queue */
		STAILQ_REMOVE(&cache->slabs, slab, kmem_slab, sqlink);
		STAILQ_INSERT_TAIL(&cache->slabs, slab, sqlink);
	}

	ke_spinlock_release(&cache->spinlock, ipl);

	if (cache->size <= SMALL_SLAB_MAX)
		ret = (void *)bufctl;
	else {
		SLIST_INSERT_HEAD(&cache->alloced, bufctl, sllink);
		ret = bufctl->base;
	}

	return ret;
}

void
kmem_cache_free(kmem_cache_t *cache, void *ptr)
{
	ipl_t ipl;
	struct kmem_slab *slab;
	struct kmem_bufctl *bufctl = NULL;

	if (cache->size <= SMALL_SLAB_MAX) {
		vm_page_t *page;
		bufctl = ptr;
		kassert(ptr >= (void *)HHDM_BASE && ptr < (void *)(HHDM_END));
		page = vm_hhdm_addr_to_page((vaddr_t)ptr);
#if KMEM_SANITY_CHECKING
		kassert(((struct vm_slab_page *)page)->slab.cache == cache);
#endif
		slab = &((struct vm_slab_page *)page)->slab;
		ipl = ke_spinlock_acquire(&cache->spinlock);
	} else {
		struct kmem_bufctl *iter;

		ipl = ke_spinlock_acquire(&cache->spinlock);

		SLIST_FOREACH (iter, &cache->alloced, sllink) {
			if (iter->base == ptr) {
				bufctl = iter;
				break;
			}
		}

#if KMEM_SANITY_CHECKING
		if (bufctl == NULL)
			kfatal("kmem_cache_free: %p not in cache", ptr);
#endif

		SLIST_REMOVE(&cache->alloced, bufctl, kmem_bufctl, sllink);
		slab = iter->slab;
	}

	slab->alloced_n--;
	slab->free_n++;
	SLIST_INSERT_HEAD(&slab->free, bufctl, sllink);

	if (slab->free_n == 1) {
		/* no longer full; push slab to front of the queue */
		STAILQ_INSERT_HEAD(&cache->slabs, slab, sqlink);
	} else if (slab->alloced_n == 0) {
#if 0
		 kprintf("todo: free empty slab?\n");
#endif
	}

	ke_spinlock_release(&cache->spinlock, ipl);
}

/* Malloc-style (bad) wrappers */

void *
kmem_malloc(size_t size)
{
	size_t total_size = size + sizeof(size_t);
	void *ptr = kmem_xalloc(total_size, 0);

	if (ptr != NULL) {
		*((size_t *)ptr) = size;
		return (char *)ptr + sizeof(size_t);
	}

	return NULL;
}

void
kmem_mfree(void *ptr)
{
	if (ptr != NULL) {
		size_t *size_ptr = (size_t *)((char *)ptr - sizeof(size_t));
		size_t size = *size_ptr;
		kmem_free(size_ptr, size + sizeof(size_t));
	}
}

void
kmem_mfree_sizeverify(void *ptr, size_t size)
{
	if (ptr != NULL) {
		size_t *size_ptr = (size_t *)((char *)ptr - sizeof(size_t));
		size_t real_size = *size_ptr;
		kassert(real_size == size);
		kmem_free(size_ptr, size + sizeof(size_t));
	}
}

void *
kmem_mrealloc(void *ptr, size_t size)
{
	size_t *size_ptr, orig_size, total_size;
	void *new;

	if (ptr == NULL)
		return kmem_malloc(size);

	if (size == 0) {
		kmem_mfree(ptr);
		return NULL;
	}

	size_ptr = (size_t *)((char *)ptr - sizeof(size_t));
	orig_size = *size_ptr;
	total_size = size + sizeof(size_t);

	new = kmem_xrealloc(size_ptr, orig_size + sizeof(size_t), total_size, 0);
	if (new == NULL)
		return NULL;

	*((size_t *)new) = size;
	return (char *)new + sizeof(size_t);
}

void *
kmem_calloc(size_t nmemb, size_t size)
{
	void *ptr = kmem_malloc(size * nmemb);

	if (ptr != NULL) {
		memset(ptr, 0x0, size * nmemb);
		return ptr;
	}

	return NULL;
}

int
kmem_vasprintf(char **strp, const char *fmt, va_list ap)
{
	size_t size = 0;
	va_list apcopy;

	va_copy(apcopy, ap);
	size = npf_vsnprintf(NULL, 0, fmt, apcopy);
	va_end(apcopy);

	if (size < 0)
		return -1;

	*strp = (char *)kmem_xalloc(size + 1, 0);

	if (NULL == *strp)
		return -1;

	size = npf_vsnprintf(*strp, size + 1, fmt, ap);
	return size;
}

int
kmem_asprintf(char **str, const char *fmt, ...)
{
	size_t size = 0;
	va_list ap;

	va_start(ap, fmt);
	size = kmem_vasprintf(str, fmt, ap);
	va_end(ap);

	return size;
}

void *
kmem_strfree(char *str)
{
	kmem_free(str, strlen(str) + 1);
	return NULL;
}
