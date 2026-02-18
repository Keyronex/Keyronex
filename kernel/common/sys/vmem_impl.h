/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2024 Cloudarox Solutions.
 */
/*!
 * @file vmem_impl.h
 * @brief VMem resource allocator implementation definitions.
 */

#ifndef ECX_KEYRONEX_VMEM_IMPL_H
#define ECX_KEYRONEX_VMEM_IMPL_H

#include <sys/vmem.h>

#include <libkern/queue.h>

#include <limits.h>

#define VMEM_FREELIST_N sizeof(vmem_addr_t) * CHAR_BIT
#define VMEM_HASHBUCKET_N 16

/**
 * A segment is either a free area, an allocated area, or a span marker (either
 * a span explicitly given to an arena, which must be manually freed, or one
 * retrieved from the backing arena.)
 */
typedef struct vmem_seg {
	enum {
		kVMemSegFree,
		kVMemSegAllocated,
		kVMemSegSpan,
		kVMemSegSpanImported,
	} type	       : 4;
	vmem_addr_t base;
	vmem_size_t size;

	TAILQ_ENTRY(vmem_seg) segqueue; /** links vmem_t::segqueue */
	LIST_ENTRY(vmem_seg) seglist;	/** links a vmem_t::freelist[n] if free;
					 * a vmem_t::hashtab bucket if allocated
					 * otherwise vmem_t::spanlist
					 */
} vmem_seg_t;

typedef TAILQ_HEAD(vmem_segqueue, vmem_seg) vmem_segqueue_t;
typedef LIST_HEAD(vmem_seglist, vmem_seg) vmem_seglist_t;

typedef struct vmem {
	char name[64];	     /** identifier for debugging */
	vmem_addr_t base;    /** base address */
	vmem_size_t size;    /** size in bytes */
	vmem_size_t quantum; /** minimum allocation size */
	vmem_size_t used;

	vmem_alloc_t allocfn; /** allocate from ::source */
	vmem_free_t freefn;   /** release to :: source */
	vmem_t *source;	      /** backing arena to allocate from */

	vmem_segqueue_t segqueue;	       /** all segments */
	vmem_seglist_t freelist[VMEM_FREELIST_N];  /** power of 2 freelist */
	vmem_seglist_t hashtab[VMEM_HASHBUCKET_N]; /** allocated segs */
	vmem_seglist_t spanlist;	       /** span marker segs */
} vmem_t;

#endif /* ECX_KEYRONEX_VMEM_IMPL_H */
