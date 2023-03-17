/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Feb 12 2023.
 */
/**
 * @file vm/ws.c
 * @brief Working Set Management Implementation
 *
 * This file implements functions for managing working set lists of processes. A
 * working set list is the collection of currently-resident pages in a process'
 * virtual address space that are subject to paging. The purpose of working set
 * management is to provide for a local approach to page replacement.
 *
 * The working set is represented as a circular queue of virtual addresses, with
 * the most recently inserted page at the tail of the queue. The functions in
 * this module implement operations for inserting and removing entries in the
 * working set, growing and shrinking the working set array, and disposing of
 * entries in the working set, which unmaps the page.
 */

#include <bsdqueue/queue.h>
#include <stdbool.h>

#include "kdk/kmem.h"
#include "vm/vm_internal.h"

#define WRAP(VALUE, TO) ((VALUE) % TO)

struct vmp_wsle {
	TAILQ_ENTRY(vmp_wsle) queue_entry;
	RB_ENTRY(vmp_wsle) rb_entry;
	vaddr_t vaddr;
};

static intptr_t
wsle_cmp(struct vmp_wsle *x, struct vmp_wsle *y)
{
	return x->vaddr - y->vaddr;
}

RB_GENERATE(vmp_wsle_rbtree, vmp_wsle, rb_entry, wsle_cmp);

/*!
 * Dispose of an entry in the working set list: unmap it and unreference the
 * page. Doesn't remove the WSLE, that's the job of elsewhere.
 */
static void
wsl_dispose(vm_map_t *map, struct vmp_wsle *wsle)
{
	pmap_unenter(map, wsle->vaddr);
}

void
vmp_wsl_init(vm_map_t *map)
{
	vm_wsl_t *wsl = &map->wsl;
	wsl->count = 0;
	TAILQ_INIT(&wsl->queue);
	RB_INIT(&wsl->rbtree);
}

void
vmp_wsl_insert(vm_map_t *map, vaddr_t entry, vm_page_t *page,
    vm_protection_t protection)
{
	struct vmp_wsle *wsle = kmem_alloc(sizeof(struct vmp_wsle));
	wsle->vaddr = entry;
	TAILQ_INSERT_TAIL(&map->wsl.queue, wsle, queue_entry);
	RB_INSERT(vmp_wsle_rbtree, &map->wsl.rbtree, wsle);
	pmap_enter(map, page->address, entry, protection);
	map->wsl.count++;
}

static void
wsl_remove(vm_map_t *map, struct vmp_wsle *wsle)
{
	RB_REMOVE(vmp_wsle_rbtree, &map->wsl.rbtree, wsle);
	TAILQ_REMOVE(&map->wsl.queue, wsle, queue_entry);
	map->wsl.count--;
	wsl_dispose(map, wsle);
}

void
vmp_wsl_remove(vm_map_t *map, vaddr_t entry)
{
	struct vmp_wsle *wsle, key;

	key.vaddr = entry;
	wsle = RB_FIND(vmp_wsle_rbtree, &map->wsl.rbtree, &key);
	if (wsle == NULL)
		kfatal("vmp_wsl_remove: no wsle for virtual address 0x%lx\n",
		    entry);

	wsl_remove(map, wsle);
}

void
vmp_wsl_remove_range(vm_map_t *map, vaddr_t start, vaddr_t end)
{
	struct vmp_wsle *wsle, *tmp;

	RB_FOREACH_SAFE (wsle, vmp_wsle_rbtree, &map->wsl.rbtree, tmp) {
		if (wsle->vaddr >= start && wsle->vaddr < end)
			wsl_remove(map, wsle);
	}
}

void
vmp_wsl_trim_n_entries(vm_map_t *map, size_t n)
{
	kfatal("Unimplemented\n");
}