/*
 * Copyright (c) 2023 The Melantix Project.
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

#include "vm/vm.h"

#define IS_FREELIST_LINK(entry) (((entry)&1) == 1)
#define DETAG(PENTRY) ((uintptr_t **)(((uintptr_t)PENTRY) & ~1))

#define malloc(...) 0x0
#define free(...)
#define assert(...)

/*!
 * Dispose of an entry in the working set list: unmap it and unreference the
 * page.
 */
static void
wsl_dispose(vm_procstate_t *vmps, vaddr_t vaddr)
{
	(void)vmps;
	(void)vaddr;
}

static void
wsl_realloc(vm_procstate_t *vmps, size_t new_size)
{
	vm_wsl_t *ws = &vmps->wsl;
	uintptr_t *new_entries = (uintptr_t *)malloc(
	    new_size * sizeof(uintptr_t));
	int i = ws->head;
	int new_tail = 0;

	ws->cur_size = 0;

	while (i != ws->tail) {
		uintptr_t entry = ws->entries[i++];
		if (!IS_FREELIST_LINK(entry)) {
			new_entries[new_tail++] = entry;
		}
		if (i >= ws->max_size) {
			i = 0;
		}
	}
	new_entries[new_tail] = ws->entries[ws->tail];
	free(ws->entries);
	ws->entries = new_entries;
	ws->head = 0;
	ws->tail = new_tail;
	ws->array_size = new_size;
}

static bool
wsl_grow(vm_procstate_t *vmps)
{
	vm_wsl_t *ws = &vmps->wsl;
	size_t increment = 16;

	/* todo: check if we're ALLOWED to grow first.... */

	if (ws->max_size + increment <= ws->array_size) {
		/* can fit new max size within the array size; simply grow */
		ws->max_size += increment;
	} else {
		/* need to allocate a bigger array. increase its size a bit
		 * more. */
		wsl_realloc(vmps, ws->array_size + increment * 4);
		ws->max_size += increment;
	}

	return true;
}

void
vmp_wsl_insert(vm_procstate_t *vmps, vaddr_t entry, vm_page_t *page,
    vm_protection_t protection)
{
	vm_wsl_t *ws = &vmps->wsl;

	if (ws->freelist_head != 0) {
		/* we are always allowed to use freelist entries, they're
		 * cruelly included in our working set size*/
		uintptr_t *freelist = *DETAG(ws->freelist_head);
		if (*freelist != 0) {
			ws->freelist_head = DETAG(*freelist);
		}
		*freelist = entry;
	} else {
		size_t new_tail = (ws->tail + 1) % ws->array_size;
		if (new_tail == ws->head) {
			/* out of slots, first try to expand the WSL */
			if (wsl_grow(vmps)) {
				/* expanded, can now append as normal */
				goto append;
			} else {
				/* could not expand, dispose of head entry and
				 * replace it */
				wsl_dispose(vmps, ws->entries[ws->head]);
				ws->entries[ws->head] = entry;
				ws->head = (ws->head + 1) % ws->array_size;
				ws->tail = (ws->tail + 1) % ws->array_size;
			}
		} else {
		append:
			/* the simple case: no disposal, just appending */
			new_tail = (ws->tail + 1) % ws->array_size;
			ws->entries[new_tail] = entry;
			ws->tail = new_tail;
			ws->cur_size++;
		}
	}
}

void
vmp_wsl_remove(vm_procstate_t *vmps, vaddr_t entry)
{
	vm_wsl_t *ws = &vmps->wsl;

	for (int i = ws->head; i != ws->tail;) {
		uintptr_t *wse = &ws->entries[i];
		if (!IS_FREELIST_LINK(entry)) {
			if (*wse == entry) {
				wsl_dispose(vmps, entry);
				*wse = (uintptr_t)ws->freelist_head;
				ws->freelist_head =
				    (uintptr_t **)((uintptr_t)wse | 0x1);
			}
		}
		if (++i >= ws->max_size) {
			i = 0;
		}
	}
}

void
vmp_wsl_trim_n_entries(vm_procstate_t *vmps, size_t n)
{
	vm_wsl_t *ws = &vmps->wsl;
	uintptr_t i, entry;

	assert(!(n > ws->cur_size));

	if (n == ws->cur_size) {
		/*! in this case, clear everything */
		for (i = ws->head; i != ws->tail; i = (i + 1) % ws->max_size) {
			entry = ws->entries[i];
			wsl_dispose(vmps, entry);
		}
		ws->head = ws->tail = 0;
		ws->cur_size = 0;
		return;
	}
	for (i = ws->head; i != (ws->head + n) % ws->max_size;
	     i = (i + 1) % ws->max_size) {
		entry = ws->entries[i];
		wsl_dispose(vmps, entry);
	}
	ws->head = (ws->head + n) % ws->max_size;
	ws->cur_size -= n;
}