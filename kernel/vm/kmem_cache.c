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
#include <kdk/vmem.h>
#include <stddef.h>

#include "vm/vmp.h"

// #define KMEM_SANITY_CHECKING 1

#define SMALL_SLAB_MAX 512

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static void *kmem_slablayer_alloc(kmem_cache_t *cache, vmem_flag_t flags);
static void kmem_slablayer_free(kmem_cache_t *cache, void *ptr);
static void magazine_layer_init(kmem_cache_t *cp, size_t magsize);

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

struct kmem_magazine {
	STAILQ_ENTRY(kmem_magazine) mag_link;
	struct kmem_bufctl *mag_round[1];
};

struct kmem_cpu_cache {
	struct kmem_magazine *cc_loaded;
	struct kmem_magazine *cc_prev;
	size_t cc_rounds;
	size_t cc_prev_rounds;
	size_t cc_magsize;
};

struct kmem_depot {
	kspinlock_t kd_lock;
	STAILQ_HEAD(, kmem_magazine) kd_full;
	STAILQ_HEAD(, kmem_magazine) kd_empty;
	size_t kd_full_count;
	size_t kd_empty_count;
	size_t kd_magsize;
};

struct kmem_cache {
	char name[32];
	kspinlock_t spinlock;
	size_t size;
	size_t align;
	void (*ctor)(void *);
	STAILQ_HEAD(, kmem_slab) slabs;
	SLIST_HEAD(, kmem_bufctl) alloced;
	TAILQ_ENTRY(kmem_cache) qlink;

	struct kmem_cpu_cache *cache_cpu;
	struct kmem_depot depot;
	size_t cache_magsize;
	bool use_magazines;
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
	cache->use_magazines = false;

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

void
kmem_postsmp_init(void)
{
	for (size_t i = 0; i < 10; i++)
		magazine_layer_init(&kmem_alloc_caches[i], 32);
	for (size_t i = 0; i < 10; i++)
		kmem_alloc_caches[i].use_magazines = true;
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

	if (flags & kVMemPFNDBHeld)
		vmp_pages_alloc_locked(&page, 0, kPageUseKWired, 0);
	else
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

static void *
kmem_slablayer_alloc(kmem_cache_t *cache, vmem_flag_t flags)
{
	struct kmem_slab *slab;
	struct kmem_bufctl *bufctl;
	void *ret;

	ke_spinlock_acquire_nospl(&cache->spinlock);

	slab = STAILQ_FIRST(&cache->slabs);
	if (slab == NULL || slab->free_n == 0) {
		if (unlikely(cache->size > SMALL_SLAB_MAX))
			slab = large_slab_new(cache, flags);
		else
			slab = small_slab_new(cache, flags);

		if (unlikely(slab == NULL)) {
			ke_spinlock_release_nospl(&cache->spinlock);
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

	if (cache->size <= SMALL_SLAB_MAX)
		ret = (void *)bufctl;
	else {
		SLIST_INSERT_HEAD(&cache->alloced, bufctl, sllink);
		ret = bufctl->base;
	}

	ke_spinlock_release_nospl(&cache->spinlock);

	return ret;
}

static void
kmem_slablayer_free(kmem_cache_t *cache, void *ptr)
{
	struct kmem_slab *slab;
	struct kmem_bufctl *bufctl = NULL;

	if (likely(cache->size <= SMALL_SLAB_MAX)) {
		vm_page_t *page;
		bufctl = ptr;
#if KMEM_SANITY_CHECKING
		kassert(ptr >= (void *)HHDM_BASE && ptr < (void *)(HHDM_END));
#endif
		page = vm_hhdm_addr_to_page((vaddr_t)ptr);
#if KMEM_SANITY_CHECKING
		kassert(((struct vm_slab_page *)page)->slab.cache == cache);
#endif
		slab = &((struct vm_slab_page *)page)->slab;
		ke_spinlock_acquire_nospl(&cache->spinlock);
	} else {
		struct kmem_bufctl *iter;

		ke_spinlock_acquire_nospl(&cache->spinlock);

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

	ke_spinlock_release_nospl(&cache->spinlock);
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
kmem_mcalloc(size_t nmemb, size_t size)
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

static struct kmem_magazine *
magazine_create(size_t magsize)
{
	size_t size = offsetof(struct kmem_magazine, mag_round[magsize]);
	struct kmem_magazine *mag = kmem_xalloc(size, 0);
	return mag;
}

static inline void
swap_magazines(struct kmem_cpu_cache *ccp)
{
	struct kmem_magazine *tmp_mag = ccp->cc_loaded;
	size_t tmp_rounds = ccp->cc_rounds;
	ccp->cc_loaded = ccp->cc_prev;
	ccp->cc_rounds = ccp->cc_prev_rounds;
	ccp->cc_prev = tmp_mag;
	ccp->cc_prev_rounds = tmp_rounds;
}

static void *
magazine_depot_alloc(kmem_cache_t *cp, struct kmem_cpu_cache *ccp,
    vmem_flag_t flags)
{
	struct kmem_magazine *full_mag;
	void *obj = NULL;

	ke_spinlock_acquire_nospl(&cp->depot.kd_lock);

	full_mag = STAILQ_FIRST(&cp->depot.kd_full);
	if (full_mag != NULL) {
		STAILQ_REMOVE_HEAD(&cp->depot.kd_full, mag_link);
		cp->depot.kd_full_count--;

		STAILQ_INSERT_HEAD(&cp->depot.kd_empty, ccp->cc_prev, mag_link);
		cp->depot.kd_empty_count++;

		ccp->cc_prev = ccp->cc_loaded;
		ccp->cc_prev_rounds = ccp->cc_rounds;
		ccp->cc_loaded = full_mag;
		ccp->cc_rounds = cp->depot.kd_magsize;
		obj = ccp->cc_loaded->mag_round[--ccp->cc_rounds];
	}

	ke_spinlock_release_nospl(&cp->depot.kd_lock);

	if (obj == NULL)
		obj = kmem_slablayer_alloc(cp, flags);

	return obj;
}

static void
magazine_depot_free(kmem_cache_t *cp, struct kmem_cpu_cache *ccp, void *buf)
{
	struct kmem_magazine *empty_mag;
	int freed = 0;

	ke_spinlock_acquire_nospl(&cp->depot.kd_lock);

	empty_mag = STAILQ_FIRST(&cp->depot.kd_empty);
	if (empty_mag != NULL) {
		STAILQ_REMOVE_HEAD(&cp->depot.kd_empty, mag_link);
		cp->depot.kd_empty_count--;

		STAILQ_INSERT_HEAD(&cp->depot.kd_full, ccp->cc_prev, mag_link);
		cp->depot.kd_full_count++;

		ccp->cc_prev = ccp->cc_loaded;
		ccp->cc_prev_rounds = ccp->cc_rounds;
		ccp->cc_loaded = empty_mag;
		ccp->cc_rounds = 0;
		ccp->cc_loaded->mag_round[ccp->cc_rounds++] = buf;
		freed = 1;
	} else if (cp->depot.kd_empty_count < cp->depot.kd_full_count) {
		empty_mag = magazine_create(cp->cache_magsize);
		if (empty_mag != NULL) {
			STAILQ_INSERT_HEAD(&cp->depot.kd_full, ccp->cc_prev,
			    mag_link);
			cp->depot.kd_full_count++;

			ccp->cc_prev = ccp->cc_loaded;
			ccp->cc_prev_rounds = ccp->cc_rounds;
			ccp->cc_loaded = empty_mag;
			ccp->cc_rounds = 0;
			ccp->cc_loaded->mag_round[ccp->cc_rounds++] = buf;
			freed = 1;
		}
	}

	ke_spinlock_release_nospl(&cp->depot.kd_lock);

	if (!freed)
		kmem_slablayer_free(cp, buf);
}

void *
kmem_magazine_alloc(kmem_cache_t *cp, vmem_flag_t flags)
{
	struct kmem_cpu_cache *ccp;
	void *obj;

	ccp = &cp->cache_cpu[KCPU_LOCAL_LOAD(cpu_num)];

	if (likely(ccp->cc_rounds > 0)) {
		obj = ccp->cc_loaded->mag_round[--ccp->cc_rounds];
		return obj;
	}

	if (likely(ccp->cc_prev_rounds > 0)) {
		swap_magazines(ccp);
		obj = ccp->cc_loaded->mag_round[--ccp->cc_rounds];
		return obj;
	}

	obj = magazine_depot_alloc(cp, ccp, flags);

	return obj;
}

void
kmem_magazine_free(kmem_cache_t *cp, void *buf)
{
	struct kmem_cpu_cache *ccp;

	ccp = &cp->cache_cpu[KCPU_LOCAL_LOAD(cpu_num)];

	if (likely(ccp->cc_rounds < ccp->cc_magsize)) {
		ccp->cc_loaded->mag_round[ccp->cc_rounds++] = buf;
		return;
	}

	if (likely(ccp->cc_prev_rounds < ccp->cc_magsize)) {
		swap_magazines(ccp);
		ccp->cc_loaded->mag_round[ccp->cc_rounds++] = buf;
		return;
	}

	magazine_depot_free(cp, ccp, buf);
};

static void
magazine_layer_init(kmem_cache_t *cp, size_t magsize)
{
	ke_spinlock_init(&cp->depot.kd_lock);
	STAILQ_INIT(&cp->depot.kd_full);
	STAILQ_INIT(&cp->depot.kd_empty);
	cp->depot.kd_full_count = 0;
	cp->depot.kd_empty_count = 0;
	cp->depot.kd_magsize = magsize;

	cp->cache_cpu = kmem_xalloc(sizeof(struct kmem_cpu_cache) * ncpus, 0);
	cp->cache_magsize = magsize;

	for (int i = 0; i < ncpus; i++) {
		struct kmem_cpu_cache *ccp = &cp->cache_cpu[i];
		ccp->cc_loaded = magazine_create(magsize);
		ccp->cc_prev = magazine_create(magsize);
		ccp->cc_magsize = magsize;
		ccp->cc_rounds = 0;
		ccp->cc_prev_rounds = 0;
	}
}

void *
kmem_cache_alloc(kmem_cache_t *cache, vmem_flag_t flags)
{
	void *ret;
	ipl_t ipl;

	ipl = spldpc();

	if (likely(cache->use_magazines))
		ret = kmem_magazine_alloc(cache, flags);
	else
		ret = kmem_slablayer_alloc(cache, flags);

	splx(ipl);

	return ret;
}

void
kmem_cache_free(kmem_cache_t *cache, void *ptr)
{
	ipl_t ipl = spldpc();

	if (likely(cache->use_magazines))
		kmem_magazine_free(cache, ptr);
	else
		kmem_slablayer_free(cache, ptr);

	splx(ipl);
}
