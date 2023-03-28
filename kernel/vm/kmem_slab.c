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
 * \page kmem_slab KMem Slab Allocator
 *
 * See: Bonwick, J. (1994). The Slab Allocator: An Object-Caching Kernel Memory
 * Allocator.
 *
 * Overview
 * ========
 *
 * Implementation
 * ==============
 *
 * There are two formats of a slab: a small slab and a large slab.

 * Small slabs are for objects smaller than or equal to PGSIZE / 16. They are
 * one page in size, and consistent of objects densely packed, with the struct
 * kmem_slab header occupying the top bytes of the page.
 *
 * Objects and slab_bufctls are united in small slabs - since a slab is always
 * exactly PGSIZE in length, there is no need to look up a bufctl in the zone;
 * instead it can be calculated trivially. So an object slot in a small slab is
 * a bufctl linked into the freelist when it is free; otherwise it is the
 * object. This saves on memory expenditure (and means that bufctls for large
 * slabs can themselves be slab-allocated).
 *
 * Large slabs have out-of-line slab headers and bufctls, and their bufctls have
 * a back-pointer to their containing slab as well as their base address. To
 * free an object in a large zone requires to look up the bufctl; their bufctls
 * are therefore linked into a list of allocated bufctls in the kmem_zone.
 * [in the future this will be a hash table.]
 */

#include <stddef.h>
#include <stdint.h>

#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/vmem.h"

#ifdef _KERNEL
#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#else
#include <sys/mman.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kdk/kmem.h"

#define PGSIZE 4096
#define ROUNDUP(addr, align) (((addr) + align - 1) & ~(align - 1))
#define ROUNDDOWN(addr, align) ((((uintptr_t)addr)) & ~(align - 1))
#define PGROUNDUP(addr) ROUNDUP(addr, PGSIZE)
#define PGROUNDDOWN(addr) ROUNDDOWN(addr, PGSIZE)

#define mutex_lock(...)
#define mutex_unlock(...)

#define kdprintf(...) printf(__VA_ARGS__)
#define fatal(...)                   \
	({                           \
		printf(__VA_ARGS__); \
		exit(1);             \
	})

static inline void *
vm_kalloc(int npages, int unused)
{
	void *ret;
	assert(posix_memalign(&ret, PGSIZE * npages, PGSIZE) == 0);
	return ret;
}
#endif

/*!
 * Bufctl entry. These are stored inline for small slabs (and are replaced by
 * the object when an object is allocated) and out-of-line for large slabs (this
 * enables denser packing, since the data in the slab is now possible to pack
 * densely; it also facilitates alignment.)
 *
 * Note that only the first entry is present in all bufctls; only large bufctls
 * have the other two.
 */
struct kmem_bufctl {
	/*!
	 * Linkage either for free list (only case for small slab); or for large
	 * slabs, kmem_zone::bufctllist
	 */
	SLIST_ENTRY(kmem_bufctl) entrylist;

	/*! slab to which bufctl belongs */
	struct kmem_slab *slab;
	/*! absolute address of entry */
	char *base;
};

/*!
 * A single slab.
 */
struct kmem_slab {
	/*! linkage kmem_zone::slablist */
	STAILQ_ENTRY(kmem_slab) slablist;
	/*! zone to which it belongs */
	struct kmem_zone *zone;
	/*! number of free entries */
	uint16_t nfree;
	/*! number alloced*/
	uint16_t nalloced;
	/*! first free bufctl */
	struct kmem_bufctl *firstfree;
	/*!
	 * For a small slab, slab contents precede this structure. Large slabs
	 * however have a pointer to their data here.
	 */
	void *data[0];
};

/*!
 * Get the address of a small slab's header from the base address of the slab.
 */
#define SMALL_SLAB_HDR(BASE) ((BASE) + PGSIZE - sizeof(struct kmem_slab))

/*! Maximum size of object that will be stored in a small slab. */
const size_t kSmallSlabMax = 256;

/*!
 * 8-byte granularity <= 64 byte;
 * 16-byte granularity <= 128 byte;
 * 32-byte granularity <= 256 byte;
 * 64-byte granularity <= 512 byte;
 * 128-byte granularity <= 1024 byte;
 * 256-byte granularity <= 2048 byte;
 * 512-byte granularity < 4096 byte;
 * >=4096 byte allocations are directly carried out by vm_kalloc() so
 * granularity is 4096 bytes.
 */
#define ZONE_SIZES(X)      \
	X(8, kmem_8)       \
	X(16, kmem_16)     \
	X(24, kmem_24)     \
	X(32, kmem_32)     \
	X(40, kmem_40)     \
	X(48, kmem_48)     \
	X(56, kmem_56)     \
	X(64, kmem_64)     \
	X(80, kmem_80)     \
	X(96, kmem_96)     \
	X(112, kmem_112)   \
	X(128, kmem_128)   \
	X(160, kmem_160)   \
	X(192, kmem_192)   \
	X(224, kmem_224)   \
	X(256, kmem_256)   \
	X(320, kmem_320)   \
	X(384, kmem_384)   \
	X(448, kmem_448)   \
	X(512, kmem_512)   \
	X(640, kmem_640)   \
	X(768, kmem_768)   \
	X(896, kmem_896)   \
	X(1024, kmem_1024) \
	X(1280, kmem_1280) \
	X(1536, kmem_1536) \
	X(1792, kmem_1792) \
	X(2048, kmem_2048) \
	X(2560, kmem_2560) \
	X(3072, kmem_3072) \
	X(3584, kmem_3584) \
	X(4096, kmem_4096)

/*! struct kmem_slab's for large-slab zones */
static struct kmem_zone kmem_slab;
/*! struct kmem_bufctl's for large-slab zones */
static struct kmem_zone kmem_bufctl;
/* general-purpose zones for kmem_alloc */
#define DEFINE_ZONE(SIZE, NAME) static struct kmem_zone NAME;
ZONE_SIZES(DEFINE_ZONE);
#undef DEFINE_ZONE
/*! array of the kmem_alloc zones for convenience*/
#define REFERENCE_ZONE(SIZE, NAME) &NAME,
static kmem_zone_t *kmem_alloc_zones[] = { ZONE_SIZES(REFERENCE_ZONE) };
#undef REFERENCE_ZONE
/*! list of all zones; TODO(med): protect with a lock */
struct kmem_zones kmem_zones = STAILQ_HEAD_INITIALIZER(kmem_zones);

#define KMEM_SANITY_CHECKS 1

void
kmem_zone_init(struct kmem_zone *zone, const char *name, size_t size)
{
	zone->name = name;
	zone->size = size;
	ke_spinlock_init(&zone->lock);
	STAILQ_INIT(&zone->slablist);
	SLIST_INIT(&zone->bufctllist);
	STAILQ_INSERT_TAIL(&kmem_zones, zone, zonelist);
}

void
kmem_init(void)
{
	kmem_zone_init(&kmem_slab, "kmem_slab",
	    sizeof(struct kmem_slab) + sizeof(void *));
	kmem_zone_init(&kmem_bufctl, "kmem_bufctl", sizeof(struct kmem_bufctl));
#define ZONE_INIT(SIZE, NAME) kmem_zone_init(&NAME, #NAME, SIZE);
	ZONE_SIZES(ZONE_INIT);
#undef ZONE_INIT
}

/* return the size in bytes held in a slab of a given zone*/
static size_t
slabsize(kmem_zone_t *zone)
{
	if (zone->size <= kSmallSlabMax) {
		return PGSIZE;
	} else {
		/* aim for at least 16 entries */
		return PGROUNDUP(zone->size * 16);
	}
}

/* return the capacity in number of objects of a slab of this zone */
static uint32_t
slabcapacity(kmem_zone_t *zone)
{
	if (zone->size <= kSmallSlabMax) {
		return (slabsize(zone) - sizeof(struct kmem_slab)) / zone->size;
	} else {
		return slabsize(zone) / zone->size;
	}
}

static struct kmem_slab *
small_slab_new(kmem_zone_t *zone, vmem_flag_t flags)
{
	struct kmem_slab *slab;
	struct kmem_bufctl *entry = NULL;
	void *base;

	/* create a new slab */
	base = (void *)vm_kalloc(1, flags);
	slab = SMALL_SLAB_HDR(base);

	STAILQ_INSERT_HEAD(&zone->slablist, slab, slablist);

	slab->zone = zone;
	slab->nfree = slabcapacity(zone);
	slab->nalloced = 0;

	/* set up the freelist */
	for (size_t i = 0; i < slabcapacity(zone); i++) {
		entry = (struct kmem_bufctl *)(base + i * zone->size);
		entry->entrylist.sle_next = (struct kmem_bufctl *)(base +
		    (i + 1) * zone->size);
	}
	entry->entrylist.sle_next = NULL;
	slab->firstfree = (struct kmem_bufctl *)(base);

	return slab;
}

static struct kmem_slab *
large_slab_new(kmem_zone_t *zone, vmem_flag_t flags)
{
	struct kmem_slab *slab;
	struct kmem_bufctl *entry = NULL, *prev = NULL;

	slab = kmem_zonealloc(&kmem_slab);

	STAILQ_INSERT_HEAD(&zone->slablist, slab, slablist);
	slab->zone = zone;
	slab->nfree = slabcapacity(zone);
	slab->nalloced = 0;
	slab->data[0] = (void *)vm_kalloc(slabsize(zone) / PGSIZE, flags);

	/* set up the freelist */
	for (size_t i = 0; i < slabcapacity(zone); i++) {
		entry = kmem_zonealloc(&kmem_bufctl);
		entry->slab = slab;
		entry->base = slab->data[0] + zone->size * i;
		if (prev)
			prev->entrylist.sle_next = entry;
		else {
			/* this is the first entry */
			slab->firstfree = entry;
		}
		prev = entry;
	}
	entry->entrylist.sle_next = NULL;

	return slab;
}

static void
slab_free(kmem_zone_t *zone, struct kmem_slab *slab, vmem_flag_t flags)
{
#if KMEM_DBG == 1
	kdprintf("Freeing slab %p in zone %s\n", slab, zone->name);
#endif
	if (zone->size > kSmallSlabMax) {
		vm_kfree((vaddr_t)slab->data[0], slabsize(zone) / PGSIZE,
		    flags);
		kmem_xzonefree(&kmem_slab, slab, flags);
	} else {
		vm_kfree(PGROUNDDOWN((uintptr_t)slab), 1, flags);
	}
}

void *
kmem_xzonealloc(kmem_zone_t *zone, vmem_flag_t flags)
{
	struct kmem_bufctl *entry, *next;
	struct kmem_slab *slab;
	void *ret;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&zone->lock);

	slab = STAILQ_FIRST(&zone->slablist);
	if (!slab || slab->nfree == 0) {
		/* no slabs or all full (full slabs always at tail of queue) */
		if (zone->size > kSmallSlabMax) {
			slab = large_slab_new(zone, flags);
		} else {
			slab = small_slab_new(zone, flags);
		}
#if KMEM_DBG == 1
		kdprintf("kmem: zone %s added slab %p\n", zone->name, slab);
#endif
	}

	__atomic_sub_fetch(&slab->nfree, 1, __ATOMIC_RELAXED);
	slab->nalloced++;
	entry = slab->firstfree;

#if KMEM_SANITY_CHECKS == 1
	kassert(slab->nfree + slab->nalloced == slabcapacity(zone));
	kassert(slab->zone == zone);
	kassert(slab->nfree <= slabcapacity(zone));
#endif
	kassert(entry != NULL);

	next = entry->entrylist.sle_next;
	if (next == NULL) {
		/* slab is now empty; put it to the back of the slab queue */
		STAILQ_REMOVE(&zone->slablist, slab, kmem_slab, slablist);
		STAILQ_INSERT_TAIL(&zone->slablist, slab, slablist);
		slab->firstfree = NULL;
	} else {
#if KMEM_SANITY_CHECKS == 1
		void *slab_base, *slab_end, *next_data;

		if (zone->size <= kSmallSlabMax) {
			slab_base = (void *)PGROUNDDOWN(slab);
			next_data = next;
		} else {
			slab_base = slab->data[0];
			next_data = next->base;
		}
		slab_end = slab_base + slabsize(zone);

		ASSERT_IN_KHEAP(next);

		kassert((void *)next_data >= slab_base &&
		    (void *)next_data < slab_end);
		kassert(
		    (uintptr_t)((void *)next_data - slab_base) % zone->size ==
		    0);
#endif

		slab->firstfree = next;
	}

	if (zone->size <= kSmallSlabMax) {
		ret = (void *)entry;
	} else {
		SLIST_INSERT_HEAD(&zone->bufctllist, entry, entrylist);
		ret = entry->base;
	}

	ke_spinlock_release(&zone->lock, ipl);

	return ret;
}

void
kmem_xzonefree(kmem_zone_t *zone, void *ptr, vmem_flag_t flags)
{
	struct kmem_slab *slab;
	struct kmem_bufctl *newfree = NULL;
	ipl_t ipl;

	ASSERT_IN_KHEAP(ptr);

	ipl = ke_spinlock_acquire(&zone->lock);

	if (zone->size <= kSmallSlabMax) {
		slab = (struct kmem_slab *)SMALL_SLAB_HDR(PGROUNDDOWN(ptr));
#if KMEM_SANITY_CHECKS == 1
		{
			struct kmem_slab *iter;
			STAILQ_FOREACH (iter, &zone->slablist, slablist) {
				if (iter == slab)
					goto next;
			}
			kfatal("No such slab!\n");
		}
	next:
#endif
		newfree = (struct kmem_bufctl *)ptr;
	} else {
		struct kmem_bufctl *iter;

		SLIST_FOREACH (iter, &zone->bufctllist, entrylist) {
			if (iter->base == ptr) {
				newfree = iter;
				break;
			}
		}

		if (!newfree) {
			kfatal("kmem_slabfree: invalid pointer %p", ptr);
			return;
		}

		SLIST_REMOVE(&zone->bufctllist, newfree, kmem_bufctl,
		    entrylist);
		slab = iter->slab;
	}

	slab->nfree++;
	slab->nalloced--;

#if KMEM_SANITY_CHECKS == 1
	kassert(slab->nfree + slab->nalloced == slabcapacity(zone));
#endif

	if (slab->nfree == slabcapacity(zone)) {
		STAILQ_REMOVE(&zone->slablist, slab, kmem_slab, slablist);
		slab_free(zone, slab, flags);
	} else {
		/* TODO: push slab to front; if nfree == slab capacity, free the
		 * slab */
		newfree->entrylist.sle_next = slab->firstfree;
		slab->firstfree = newfree;
	}

	ke_spinlock_release(&zone->lock, ipl);
}

void
kmem_dump()
{
	kmem_zone_t *zone;

	kdprintf("\033[7m%-24s%-6s%-6s%-6s%-6s\033[m\n", "name", "size",
	    "slabs", "objs", "free");

	STAILQ_FOREACH (zone, &kmem_zones, zonelist) {
		size_t cap;
		size_t nSlabs = 0;
		size_t totalFree = 0;
		struct kmem_slab *slab;
		ipl_t ipl;

		ipl = ke_spinlock_acquire(&zone->lock);

		cap = slabcapacity(zone);

		STAILQ_FOREACH (slab, &zone->slablist, slablist) {
			nSlabs++;
			totalFree += slab->nfree;
		}

		kdprintf("%-24s%-6zu%-6lu%-6lu%-6lu\n", zone->name, zone->size,
		    nSlabs, cap * nSlabs - totalFree, totalFree);

		ke_spinlock_release(&zone->lock, ipl);
	}
}

static inline int
zonenum(size_t size)
{
	if (size <= 64)
		return ROUNDUP(size, 8) / 8 - 1;
	else if (size <= 128)
		return ROUNDUP(size - 64, 16) / 16 + 7;
	else if (size <= 256)
		return ROUNDUP(size - 128, 32) / 32 + 11;
	else if (size <= 512)
		return ROUNDUP(size - 256, 64) / 64 + 15;
	else if (size <= 1024)
		return ROUNDUP(size - 512, 128) / 128 + 19;
	else if (size <= 2048)
		return ROUNDUP(size - 1024, 256) / 256 + 23;
	else if (size <= 4096)
		return ROUNDUP(size - 2048, 512) / 512 + 27;
	else
		/* use vm_kalloc() directly */
		return -1;
}

static void *
_kmem_alloc(size_t size, vmem_flag_t flags)
{
	int zoneidx;

	kassert(size > 0);

	zoneidx = zonenum(size);

	if (zoneidx == -1) {
		size_t realsize = PGROUNDUP(size);
		return (void *)vm_kalloc(realsize / PGSIZE, flags);
	} else {
		return kmem_xzonealloc(kmem_alloc_zones[zoneidx], flags);
	}
}

void *
kmem_xalloc(size_t size, vmem_flag_t flags)
{
	void *ret = _kmem_alloc(size, flags);
#if 0
	memset(ret - 64, 0xDEAFBEEF, 64);
	memset(ret + size, 0xDEADBEEF, 64);
	memset(ret, 0x0, size);
#endif
	return ret;
}

void
kmem_xfree(void *ptr, size_t size, vmem_flag_t flags)
{
	int zoneidx = zonenum(size);

	kassert(size > 0);

	if (zoneidx == -1) {
		size_t realsize = PGROUNDUP(size);
		return vm_kfree((uintptr_t)ptr, realsize / PGSIZE, flags);
	} else
		return kmem_xzonefree(kmem_alloc_zones[zoneidx], ptr, flags);
}

void *
kmem_xrealloc(void *ptr, size_t oldSize, size_t size, vmem_flag_t flags)
{
	void *ret;

	ret = kmem_xalloc(size, flags);
	if (ptr != NULL) {
		kassert(oldSize > 0);
		kassert(size > oldSize);
		memcpy(ret, ptr, oldSize);
		kmem_xfree(ptr, oldSize, flags);
	}
	return ret;
}

void *
kmem_zalloc(size_t size)
{
	void *ret = kmem_alloc(size);
	memset(ret, 0x0, size);
	return ret;
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

#ifndef _KERNEL
int
main(int argc, char *argv[])
{
	void *two;

	kmem_init();

	printf("alloc 8/1: %p\n", kmem_zonealloc(&kmem_slab_8));
	two = kmem_zonealloc(&kmem_slab_8);
	printf("alloc 8/2: %p\n", two);
	printf("alloc 8/3: %p\n", kmem_zonealloc(&kmem_slab_8));

	kdprintf("free 8/2\n");
	kmem_slabfree(&kmem_slab_8, two);

	printf("alloc 8/4 (should match 8/2): %p\n",
	    kmem_zonealloc(&kmem_slab_8));

	printf("alloc 1024/1: %p\n", kmem_zonealloc(&kmem_slab_1024));
	two = kmem_zonealloc(&kmem_slab_1024);
	printf("alloc 1024/2: %p\n", two);
	printf("alloc 1024/3: %p\n", kmem_zonealloc(&kmem_slab_1024));

	kdprintf("free 1024/2\n");
	kmem_slabfree(&kmem_slab_1024, two);

	printf("alloc 1024/4 (should match 1024/2): %p\n",
	    kmem_zonealloc(&kmem_slab_1024));

	return 2;
}
#endif
