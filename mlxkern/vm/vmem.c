/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2020-2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

/**
 * \page vmem VMem Resource Allocator
 *
 * See: Adams, A. and Bonwick, J. (2001). Magazines and Vmem: Extending the Slab
 * Allocator to Many CPUs and Arbitrary Resources.
 *
 * See: The NetBSD Project (2020). vmem - virtual memory allocator. Available at
 * https://man.netbsd.org/vmem.9
 *
 * # Overview
 *
 * VMem is a general-purpose resource allocator used by the kernel; it is not
 * just for virtual memory, despite its name. It deals in arenas, made up of
 * spans, which are non-overlapping interval scales. Thus they are useful for
 * e.g. PIDs as well as virtual address space. Several allocation strategies are
 * available, including an approximation of best-fit allocation provided for in
 * constant time. Best-fit is also available, as well as next-fit, which tries
 * to allocate sequentially (useful for PIDs).
 *
 * SCAL/UX VMem is an implementation of the allocator described in Adams and
 * Bonwick (2001) as used in Solaris, though the interface has similar
 * adjustments (e.g. to handle SPL constraints) as does NetBSD's implementation.
 * The code is original.
 *
 * ## Arena
 *
 * An arena is an interval scale, characterised by its start point, end point,
 * and quantum. For example, the arena kernel_heap starts at KHEAP_START, ends
 * at KHEAP_END, and has a quantum of PGSIZE; the quantum is the natural unit of
 * an arena, and allocations are made in multiples of it and aligned to it.
 *
 * Arenas may have a backing source; an arena with a backing source is a logical
 * subarena of another, which may import spans from it.
 *
 * ## Span
 *
 * A span is an interval scale within an arena; they made be imported from a
 * backing source arena, or explicitly given over to an arena's control. They
 * must not overlap.
 *
 * ## Segment
 *
 *
 * # Technical details
 *
 * VMem has its own allocation scheme for its segment tags; a freelist is kept
 * filled with a reserve of tags prior to each operation, so that any nested
 * operations which are invoked on the kernel VA arena can use those instead of
 * trying to allocate new segments, which would create a cycle. A miniature slab
 * allocator is used by default to allocate the segment structures.
 *
 * A segment is a subdivision of a span. Arenas are divided into a tail queue of
 * segments; a segment may either be a free area, an allocated area, or a span
 * marker; a span marker is located immediately prior to any other sort of
 * segment at the point in the queue representing the start of a span. For
 * example, where there is a span from 0x1000 to 0x8000, another from 0x8000 to
 * 0x10000; and allocated areas at 0x1000 to 0x2000 and from 0x9000 to 0x10000,
 * this is how the queue would look:
 *
 * [span 0x1000/0x7000] -> [alloced 0x1000/0x1000] -> [free 0x2000/0x6000] ->
 * [span 0x8000/0x7000] -> [free 0x8000/0x1000] -> [alloced 0x9000/0x7000]
 *
 * As well as the segment queue, there are several other containers to which
 * different sorts of segments are added.
 *
 * ## Freelists
 *
 * There are `2 ^ (word size in bits) / arena quantum` freelists. Each stores
 * segments sized from `arena quantum * 2 ^ n`, where n is the freelist array
 * index. (XXX not currently, they don't yet know of arena quantums; formula
 * right now is just `2 ^ n`).
 *
 */

/**
 * @file vmem.c
 * @brief Implementation of the VMem resource allocator.
 */


#ifdef _KERNEL
#include "vm/vm.h"
#include "ke/ke.h"
#include "libkern/libkern.h"
#else
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define vm_kalloc(SIZE, FLAGS) malloc(SIZE)
#define kmalloc malloc
#define kdprintf printf
#define fatal(...)                              \
	{                                       \
		kdprintf("fatal: " __VA_ARGS__); \
		exit(EXIT_FAILURE);             \
	}
#endif

#include "vmem_impl.h"

#ifndef ERESOURCEEXHAUSTED
#define ERESOURCEEXHAUSTED 1200
#endif

struct seg_chunk {
	bool	   used;
	vmem_seg_t seg;
};

typedef struct seg_block {
	LIST_ENTRY(seg_block);
	struct seg_chunk _chunks[63];
} seg_slab_t;

static vmem_seg_t     static_segs[128];
static vmem_seglist_t free_segs = LIST_HEAD_INITIALIZER(free_segs);
static kspinlock_t     free_segs_lock = KSPINLOCK_INITIALISER;
static int	      nfreesegs = 0;
// static LIST_HEAD(, seg_block) seg_blocks = LIST_HEAD_INITIALIZER(seg_blocks);

static const char *vmem_seg_type_str[] = {
	[kVMemSegFree] = " free",
	[kVMemSegAllocated] = "alloc",
	[kVMemSegSpan] = " span",
	[kVMemSegSpanImported] = "spani",
};

static vmem_size_t
freelist(vmem_size_t size)
{
	return (sizeof(vmem_addr_t) * CHAR_BIT) - __builtin_clzl(size) - 1;
}

/**
 * Find the freelist index into which a free segment sized \p size should be
 * placed; accordingly, this index represents the first freelist from which one
 * of its entries *might* satisfy an allocation request of size \p size.
 */
static vmem_seglist_t *
freelist_for_size(vmem_t *vmem, vmem_size_t size)
{
	return &vmem->freelist[freelist(size)];
}

/**
 * Insert a free segment into the appropriate freelist.
 */
static void
freelist_insert(vmem_t *vmem, vmem_seg_t *freeseg)
{
	LIST_INSERT_HEAD(freelist_for_size(vmem, freeseg->size), freeseg,
	    seglist);
}

/** Allocate a segment. */
static vmem_seg_t *
seg_alloc(vmem_t *vmem, vmem_flag_t flags)
{
	vmem_seg_t *seg;
	ipl_t	    ipl;

	ipl = ke_spinlock_acquire(&free_segs_lock);
	kassert(!LIST_EMPTY(&free_segs));
	seg = LIST_FIRST(&free_segs);
	LIST_REMOVE(seg, seglist);
	nfreesegs--;
	ke_spinlock_release(&free_segs_lock, ipl);

	return seg;
}

/** Free a segment structure. */
static void
seg_free(vmem_t *vmem, vmem_seg_t *seg)
{
	ipl_t	    ipl;

	ipl = ke_spinlock_acquire(&free_segs_lock);
	LIST_INSERT_HEAD(&free_segs, seg, seglist);
	nfreesegs++;
	ke_spinlock_release(&free_segs_lock, ipl);
}

/** Refill the free segments list. */
static void
seg_refill(int flags)
{
	struct seg_block *block;

	if (nfreesegs >= 128)
		return;

	block = (void*)vm_kalloc(1, flags & kVMemSleep ? 0x3 : 0x2);
	kassert(block != NULL);
	for (int i = 0; i < elementsof(block->_chunks); i++) {
		memset(&block->_chunks[i].seg, 0x0,
		    sizeof(block->_chunks[i].seg));
		seg_free(NULL, &block->_chunks[i].seg);
	}
}

uint64_t
murmur64(uint64_t h)
{
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53L;
	h ^= h >> 33;
	return h;
}

static vmem_seglist_t *
hashbucket_for_addr(vmem_t *vmem, vmem_addr_t addr)
{
	uintptr_t hash = murmur64(addr) % kNHashBuckets;
	return &vmem->hashtab[hash];
}

static void
hashtab_insert(vmem_t *vmem, vmem_seg_t *seg)
{
	LIST_INSERT_HEAD(hashbucket_for_addr(vmem, seg->base), seg, seglist);
}

static int
vmem_add_internal(vmem_t *vmem, int spantype, vmem_addr_t base,
    vmem_size_t size, vmem_flag_t flags, vmem_seg_t **freeseg_out)
{
	vmem_seg_t *afterspan = NULL, *iter = NULL;
	vmem_seg_t *newspan, *newfree;

	/* find the last span before base */
	LIST_FOREACH (iter, &vmem->spanlist, seglist) {
		if (iter->base >= base)
			break;
		afterspan = iter;
	}

	newspan = seg_alloc(vmem, flags);
	newspan->base = base;
	newspan->size = size;
	newspan->type = spantype;

	newfree = seg_alloc(vmem, flags);
	newfree->base = base;
	newfree->size = size;
	newfree->type = kVMemSegFree;

	if (afterspan) {
		vmem_seg_t *nextspan = LIST_NEXT(afterspan, seglist);

		LIST_INSERT_AFTER(afterspan, newspan, seglist);

		if (nextspan)
			TAILQ_INSERT_BEFORE(nextspan, newspan, segqueue);
		else
			TAILQ_INSERT_TAIL(&vmem->segqueue, newspan, segqueue);

	} else {
		LIST_INSERT_HEAD(&vmem->spanlist, newspan, seglist);
		TAILQ_INSERT_HEAD(&vmem->segqueue, newspan, segqueue);
	}

	TAILQ_INSERT_AFTER(&vmem->segqueue, newspan, newfree, segqueue);
	freelist_insert(vmem, newfree);

	if (freeseg_out)
		*freeseg_out = newfree;

	return 0;
}

vmem_t *
vmem_init(vmem_t *vmem, const char *name, vmem_addr_t base, vmem_size_t size,
    vmem_size_t quantum, vmem_alloc_t allocfn, vmem_free_t freefn,
    vmem_t *source, size_t qcache_max, vmem_flag_t flags, ipl_t pl)
{
	strcpy(vmem->name, name);
	vmem->base = base;
	vmem->size = size;
	vmem->quantum = quantum;
	vmem->flags = flags;
	vmem->allocfn = allocfn;
	vmem->freefn = freefn;
	vmem->source = source;

	TAILQ_INIT(&vmem->segqueue);
	LIST_INIT(&vmem->spanlist);
	for (int i = 0; i < kNFreeLists; i++)
		LIST_INIT(&vmem->freelist[i]);
	for (int i = 0; i < kNHashBuckets; i++)
		LIST_INIT(&vmem->hashtab[i]);

	if (size != 0 && !source)
		vmem_add_internal(vmem, kVMemSegSpan, base, size, flags, NULL);

	return vmem;
}

int
vmem_add(vmem_t *vmem, vmem_addr_t base, vmem_size_t size, vmem_flag_t flags)
{
	return vmem_add_internal(vmem, kVMemSegSpan, base, size, flags, NULL);
}

/**
 * Split an allocation out of free segment \p seg. Expects pointers to pointers
 * to pre-allocated segments \p left and \p right, which are set up with
 * appropriate values if necessary; otherwise they are freed and set to NULL.
 */
static void
split_seg(vmem_t *vmem, vmem_seg_t *seg, vmem_seg_t **left, vmem_seg_t **right,
    vmem_addr_t addr, vmem_size_t size)
{
	kassert(seg->type == kVMemSegFree);

	LIST_REMOVE(seg, seglist);

	if (addr > seg->base) {
		(*left)->type = kVMemSegFree;
		(*left)->base = seg->base;
		(*left)->size = addr - seg->base;
		TAILQ_INSERT_BEFORE(seg, *left, segqueue);
		freelist_insert(vmem, *left);
	} else {
		seg_free(vmem, *left);
	}

	if (addr + size < seg->base + seg->size) {
		(*right)->type = kVMemSegFree;
		(*right)->base = addr + size;
		(*right)->size = (seg->base + seg->size) - (addr + size);
		TAILQ_INSERT_AFTER(&vmem->segqueue, seg, *right, segqueue);
		freelist_insert(vmem, *right);
	} else {
		seg_free(vmem, *right);
	}

	seg->type = kVMemSegAllocated;
	seg->base = addr;
	seg->size = size;
	hashtab_insert(vmem, seg);
}

/**
 * Get the previous segment in the queue; returns NULL if there is none.
 */
static vmem_seg_t *
prev_seg(vmem_seg_t *seg)
{
	return TAILQ_PREV(seg, vmem_segqueue, segqueue);
}

/**
 * Get the next segment in the queue; returns NULL if there is none.
 */
static vmem_seg_t *
next_seg(vmem_seg_t *seg)
{
	return TAILQ_NEXT(seg, segqueue);
}

static int
try_import(vmem_t *vmem, vmem_size_t size, vmem_flag_t flags, vmem_seg_t **out)
{
	vmem_addr_t addr;
	int	    r;

	if (!vmem->allocfn)
		return -ERESOURCEEXHAUSTED;

	r = vmem->allocfn(vmem->source, size, flags, &addr);
	if (r < 0)
		return r;

	r = vmem_add_internal(vmem, kVMemSegSpanImported, addr, size, flags,
	    out);
	if (r < 0)
		vmem->freefn(vmem->source, addr, size);

	return r;
}

int
vmem_xalloc(vmem_t *vmem, vmem_size_t size, vmem_size_t align,
    vmem_size_t phase, vmem_size_t nocross, vmem_addr_t min, vmem_addr_t max,
    vmem_flag_t flags, vmem_addr_t *out)
{
	size_t		freelist_idx = freelist(size) - 1;
	vmem_seglist_t *list;
	vmem_seg_t	   *freeseg, *newlseg, *newrseg;
	vmem_addr_t	addr;
	bool		tried_import = false;

	kassert(align == 0 && "not supported yet\n");
	kassert(phase == 0 && "not supported yet\n");
	kassert(min == 0 || ((flags & kVMemExact) && " not supported yet\n"));
	kassert(max == 0 && " not supported yet\n");

	if (!(flags & kVMemBootstrap))
		seg_refill(flags);

	kassert(size != 0);

	/* preallocate new segments, they will be freed if necessary */
	newlseg = seg_alloc(vmem, flags);
	newrseg = seg_alloc(vmem, flags);

search:
	/* TODO: strategies other than this one... */
	if (++freelist_idx >= elementsof(vmem->freelist)) {
		if (tried_import)
			return -ERESOURCEEXHAUSTED;
		else {
			int r;
			tried_import = true;
			r = try_import(vmem, size, flags, &freeseg);
			if (r < 0)
				return r;
			addr = freeseg->base;
			goto split_seg;
		}
	}

	list = &vmem->freelist[freelist_idx];
	LIST_FOREACH (freeseg, list, seglist) {
		if (freeseg->base <= min) {
			addr = min;
		} else if (flags & kVMemExact && freeseg->base > min) {
			continue;
		} else {
			addr = freeseg->base;
		}

		if (addr + size > freeseg->base + freeseg->size) {
			continue;
		}

		goto split_seg;
	}

	goto search;

split_seg:
	split_seg(vmem, freeseg, &newlseg, &newrseg, addr, size);

	*out = addr;
	return 0;
}

static void
freeseg_expand(vmem_t *vmem, vmem_seg_t *seg, vmem_addr_t newbase,
    vmem_size_t newsize)
{
	size_t oldfreelist = freelist(seg->size), new;
	seg->base = newbase;
	seg->size = newsize;
	new = freelist(seg->size);
	if (new != oldfreelist) {
		/* remove from old freelist */
		LIST_REMOVE(seg, seglist);
		/* place on freelist appropriate to new size */
		freelist_insert(vmem, seg);
	}
}

int
vmem_xfree(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size)
{
	vmem_seglist_t *bucket = hashbucket_for_addr(vmem, addr);
	vmem_seg_t	   *seg, *left, *right;
	bool		coalesced_left = false, coalesced = false;

	LIST_FOREACH (seg, bucket, seglist) {
		if (seg->base == addr)
			goto free;
	}

	kfatal("vmem_xfree: segment at address 0x%lx doesn't exist\n", addr);
	return -1;

free:
#if 0
	if (size != 0 && seg->size != size)
		fatal(
		    "vmem_xfree: mismatched size (given 0x%lx, actual 0x%lx)\n",
		    size, seg->size);
#endif

	size = seg->size;

	/* remove from hashtable */
	LIST_REMOVE(seg, seglist);

	/* coalesce to the left */
	left = prev_seg(seg);
	if (left->type == kVMemSegFree) {
		freeseg_expand(vmem, left, left->base, left->size + seg->size);
		TAILQ_REMOVE(&vmem->segqueue, seg, segqueue);
		seg_free(vmem, seg);
		seg = left;
		left = prev_seg(seg);
		coalesced_left = coalesced = true;
	}

	/* coalesce to the right */
	right = next_seg(seg);
	if (right && right->type == kVMemSegFree) {
		freeseg_expand(vmem, right, seg->base, right->size + seg->size);
		TAILQ_REMOVE(&vmem->segqueue, seg, segqueue);
		if (coalesced_left)
			/* remaining on a freelist */
			LIST_REMOVE(seg, seglist);
		seg_free(vmem, seg);
		seg = right;
		right = next_seg(seg);
		coalesced = true;
	}

	if (left->type == kVMemSegSpanImported && seg->size == left->size) {
		kassert(!right || right->type == kVMemSegSpanImported);
		kdprintf("Entire ispan 0x%lx-0x%lx is free\n", left->base,
		    left->base + left->size);
		vmem->freefn(vmem->source, left->base, left->size);

		/* XXX seg not set free yet if it wasn't coalesced */
#if 0
		if(seg->type != kVMemSegFree) {
			fatal("unexpected seg type %d\n", seg->type);
		}
#endif
		if (coalesced)
			LIST_REMOVE(seg, seglist);
		TAILQ_REMOVE(&vmem->segqueue, seg, segqueue);
		seg_free(vmem, seg);

		LIST_REMOVE(left, seglist);
		TAILQ_REMOVE(&vmem->segqueue, left, segqueue);
		seg_free(vmem, left);

		coalesced = true;
	}

	if (!coalesced) {
		TAILQ_REMOVE(&vmem->segqueue, seg, segqueue);
		seg_free(vmem, seg);
	}

	return size;
}

void
vmem_earlyinit()
{
	for (int i = 0; i < elementsof(static_segs); i++)
		seg_free(NULL, &static_segs[i]);
}

void
vmem_destroy(vmem_t *vmem)
{
	vmem_seg_t *seg;

	for (int i = 0; i < kNHashBuckets; i++)
		kassert(LIST_EMPTY(&vmem->hashtab[i]));

	TAILQ_FOREACH (seg, &vmem->segqueue, segqueue)
		seg_free(vmem, seg);
}

void
vmem_dump(const vmem_t *vmem)
{
	vmem_seg_t *span;

	kdprintf("VMem arena <%s> segment queue:\n", vmem->name);
	TAILQ_FOREACH (span, &vmem->segqueue, segqueue) {
		kdprintf("[%s:0x%lx-0x%lx]\n", vmem_seg_type_str[span->type],
		    span->base, span->base + span->size);
	}
}

#ifndef _KERNEL
int
main()
{
	vmem_t     *vmem = malloc(sizeof *vmem);
	vmem_seg_t *span;
	vmem_addr_t addr = -1ul;

	vmem_earlyinit();

	vmem_init(vmem, "hello", 0x0, 0x1000000, 0x1000, NULL, NULL, NULL, 0, 0,
	    0);
	vmem_xalloc(vmem, 0x2000, 0, 0, 0, 0x2000, 0, kVMemExact, &addr);

#if 0
	vmem_add(vmem, 0x1000, 0x1000, 0);
	vmem_add(vmem, 0x3000, 0x5000, 0);
	vmem_add(vmem, 0x0, 0x1000, 0);
	vmem_add(vmem, 0x2000, 0x1000, 0);
	vmem_add(vmem, 0x10000, 0x5000, 0);
#endif

	kdprintf("spans:\n");
	LIST_FOREACH (span, &vmem->spanlist, seglist) {
		kdprintf("[%p-%p]\n", span->base, span->base + span->size);
	}
	kdprintf("\nsegs\n");
	TAILQ_FOREACH (span, &vmem->segqueue, segqueue) {
		kdprintf("[%s:%p-%p]\n", vmem_seg_type_str[span->type],
		    span->base, span->base + span->size);
	}

	vmem_xalloc(vmem, 0x2000, 0, 0, 0, 0, 0, 0, &addr);

#if 0
	kdprintf("\nsegs\n");
	TAILQ_FOREACH (span, &vmem->segqueue, segqueue) {
		kdprintf("[%s:%p-%p]\n", vmem_seg_type_str[span->type],
		    span->base, span->base + span->size);
	}

	kdprintf("FREEING\n");
	vmem_xfree(vmem, addr, 0x2000);
#endif

	kdprintf("\nsegs\n");
}
#endif
