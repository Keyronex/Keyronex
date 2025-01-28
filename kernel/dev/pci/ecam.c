/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Nov 04 2024.
 */

#include <kdk/kmem.h>
#include <kdk/queue.h>
#include <kdk/vm.h>

#include "dev/pci/ecam.h"
#include "vm/vmp.h"

struct ecam_view {
	RB_ENTRY(ecam_view) entry;
	uint16_t seg;
	uint32_t bus;
	vaddr_t base;
};

static RB_HEAD(ecam_view_tree, ecam_view) ecam_views;
RB_PROTOTYPE_STATIC(ecam_view_tree, ecam_view, entry, ecam_view_cmp);

struct ecam_span *ecam_spans;
size_t ecam_spans_n;

static int
ecam_view_cmp(struct ecam_view *a, struct ecam_view *b)
{
	if (a->seg < b->seg)
		return -1;
	if (a->seg > b->seg)
		return 1;
	if (a->bus < b->bus)
		return -1;
	if (a->bus > b->bus)
		return 1;
	return 0;
}

RB_GENERATE_STATIC(ecam_view_tree, ecam_view, entry, ecam_view_cmp);

static paddr_t
ecam_paddr(uint16_t segment, uint8_t bus)
{
	for (size_t i = 0; i < ecam_spans_n; i++) {
		struct ecam_span *span = &ecam_spans[i];
		if (span->seg == segment && bus >= span->bus_start &&
		    bus <= span->bus_end)
			return span->base + (bus << 20);
	}

	return -1;
}

vaddr_t
ecam_get_view(uint16_t seg, uint8_t bus)
{
	struct ecam_view key, *view;

	key.seg = seg;
	key.bus = bus;
	view = RB_FIND(ecam_view_tree, &ecam_views, &key);

	if (view == NULL) {
		int r;
		paddr_t phys;

		phys = ecam_paddr(seg, bus);
		if (phys == -1)
			return -1;

		view = kmem_alloc(sizeof(*view));
		view->seg = seg;
		view->bus = bus;

		r = vm_ps_map_physical_view(&kernel_procstate, &view->base,
		    0x100000, phys, kVMAll, kVMAll, false);

		kassert(r == 0);

		RB_INSERT(ecam_view_tree, &ecam_views, view);
	}

	return view->base;
}
