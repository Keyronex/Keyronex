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
 * @file vmem.h
 * @brief Private interface to the VMem resource allocator. See vmem.c for
 * detailed description of VMem.
 */

#ifndef KRX_KDK_VMEM_IMPL_H
#define KRX_KDK_VMEM_IMPL_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kdk/queue.h"
#include "./vmem.h"

enum { kNFreeLists = sizeof(vmem_addr_t) * CHAR_BIT, kNHashBuckets = 16 };

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
	} type : 4;
	bool is_static : 1;
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

	vmem_flag_t flags;

	vmem_alloc_t allocfn; /** allocate from ::source */
	vmem_free_t freefn;   /** release to :: source */
	vmem_t *source;	      /** backing arena to allocate from */

	vmem_segqueue_t segqueue;	       /** all segments */
	vmem_seglist_t freelist[kNFreeLists];  /** power of 2 freelist */
	vmem_seglist_t hashtab[kNHashBuckets]; /** allocated segs */
	vmem_seglist_t spanlist;	       /** span marker segs */
} vmem_t;

void vmem_earlyinit();

#endif /* KRX_KDK_VMEM_IMPL_H */
