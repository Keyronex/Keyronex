/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Fri Apr 26 2024.
 */
/*!
 * @file writeback.c
 * @brief Dirty page write-back daemon.
 */

#include "kdk/dev.h"
#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/misc.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "vm/m68k/vmp_m68k.h"
#include "vmp.h"

extern kevent_t vmp_writeback_event;

struct pagefile {
	vnode_t *vnode;
	uint8_t *bitmap;
	size_t total_slots;
	size_t free_slots;
	size_t next_free;
};

#define kMaxWritebacks 16
const int kMaxPagesPerWrite = 4;
struct pagefile pagefile;

int
init_vmp_pagefile(struct pagefile *pf, vnode_t *vnode, size_t length)
{
	size_t npages = length / PGSIZE;
	size_t bitmap_size = (npages + 7) / 8;

	pf->vnode = vnode;
	pf->total_slots = npages;
	pf->free_slots = npages - 1;
	pf->next_free = 0;

	pf->bitmap = (uint8_t *)kmem_alloc(bitmap_size);
	if (pf->bitmap == NULL)
		return -1;

	memset(pf->bitmap, 0, bitmap_size);
	pf->bitmap[0] = 1; /* don't allocate 0 */

	return 0;
}

uintptr_t
vmp_pagefile_alloc(struct pagefile *pf)
{
	size_t start = pf->next_free;
	for (size_t i = 0; i < pf->total_slots; ++i) {
		size_t idx = (start + i) % pf->total_slots;
		size_t byte_index = idx / 8;
		size_t bit_index = idx % 8;

		if (!(pf->bitmap[byte_index] & (1 << bit_index))) {
			pf->bitmap[byte_index] |= (1 << bit_index);
			pf->free_slots--;

			pf->next_free = (idx + 1) % pf->total_slots;
			return idx;
		}
	}

	return -1;
}

int
prepare_cluster_write(vm_page_t *main_page, vm_mdl_t *mdl, iop_frame_t *frame)
{
	pte_t *page_pte = (pte_t *)P2V(main_page->referent_pte);
	io_off_t offset;
	bool anon;

	switch (main_page->use) {
	case kPageUseAnonPrivate:
	case kPageUsePML1:
	case kPageUsePML2:
	case kPageUsePML3:
	case kPageUsePML4:
		kassert(vmp_pte_characterise(page_pte) == kPTEKindTrans);
		anon = true;
		frame->dev = pagefile.vnode->vfs->device;
		frame->vnode = pagefile.vnode;
		break;

	case kPageUseFileShared: {
		vm_object_t *obj = main_page->owner;
		vnode_t *vnode = obj->file.vnode;
		anon = false;
		frame->dev = vnode->vfs->device;
		frame->vnode = vnode;
		break;
	}
	}

	if (anon && main_page->drumslot == 0) {
		uintptr_t drumslot = vmp_pagefile_alloc(&pagefile);
		if (drumslot == -1)
			return -1;
		main_page->drumslot = drumslot;
		frame->rw.offset = main_page->drumslot * PGSIZE;
	} else {
		frame->rw.offset = (io_off_t)main_page->offset >>
		    VMP_PAGE_SHIFT;
	}

	mdl->offset = 0;
	mdl->nentries = 1;
	mdl->pages[0] = main_page;
	mdl->write = false;

	frame->function = kIOPTypeWrite;
	frame->rw.bytes = PGSIZE;
	frame->mdl = mdl;

	return 0;
}

void vmp_page_reclaim(vm_page_t *page, enum vm_page_use new_use);

void
reclaim_all_pages(void)
{
	ipl_t ipl = vmp_acquire_pfn_lock();
	vm_page_t *page;

	while (true) {
		page = TAILQ_FIRST(&vm_pagequeue_standby);
		if (page == NULL)
			break;

		TAILQ_REMOVE(&vm_pagequeue_standby, page, queue_link);
		vmp_page_reclaim(page, kPageUseDeleted);
	}

	vmp_release_pfn_lock(ipl);
}

void
vmp_writeback(void *)
{
	iop_t *iops[kMaxWritebacks] = { 0 };
	STATIC_MDL(kMaxPagesPerWrite) mdls[kMaxWritebacks];

	while (true) {
		kwaitresult_t w = ke_wait(&vmp_writeback_event,
		    "vmp_writeback_event", false, false, NS_PER_S / 5);
		ipl_t ipl;
		size_t nio_prepared = 0;

		if (pagefile.vnode == NULL)
			continue;

		if (iops[0] == NULL) {
			for (int i = 0; i < kMaxWritebacks; i++)
				iops[i] = iop_new_vnode_write(pagefile.vnode,
				    &mdls[i].mdl, 0, 0);
		}

		(void)w;
#if 0
		kprintf("The Writeback Daemon!\n");
#endif

		ipl = vmp_acquire_pfn_lock();
		for (int i = 0; i < kMaxWritebacks; i++) {
			vm_page_t *page = TAILQ_FIRST(&vm_pagequeue_modified);
			int r;

			if (page == NULL)
				break;

			kassert(page->refcnt == 0);
			kassert(page->dirty);

			vmp_page_retain_locked(page);
			page->dirty = false;

			iop_init(iops[i]);
			r = prepare_cluster_write(page, &mdls[i].mdl,
			    &iops[i]->stack[0]);
			kassert(r == 0);

			nio_prepared++;
		}

		if (nio_prepared != 0)
			kprintf("nio_prepared == %d\n", nio_prepared);

		vmp_release_pfn_lock(ipl);

		for (int i = 0; i < nio_prepared; i++) {
			int r = iop_send_sync(iops[i]);
			kassert(r == 0);
		}

		ipl = vmp_acquire_pfn_lock();
		for (int i = 0; i < nio_prepared; i++) {
			vmp_page_release_locked(mdls[i].mdl.pages[0]);

			if (mdls[i].mdl.pages[0]->use == kPageUseFileShared) {
				vm_object_t *obj = mdls[i].mdl.pages[0]->owner;

				if (--obj->file.n_dirty_pages == 0) {
					/*
					 * reached 0 after being 1 - this means
					 * there are no dirty pages left for
					 * this vnode (that aren't in working
					 * sets anyway) so the vnode can be
					 * released.
					 *
					 * if they were dirtied in the meantime,
					 * there is no problem - in the fault
					 * handler, the needful adjustment
					 * would have been made.
					 */
					/* todo: defer til PFN lock released */
					vn_release(obj->file.vnode);
				}
			}
		}
		vmp_release_pfn_lock(ipl);

		reclaim_all_pages();
	}
}

vnode_t *pagefile_vnode;

int
vm_pagefile_add(struct vnode *vnode)
{
	init_vmp_pagefile(&pagefile, vnode, 16ull * 1024 * 1024);
	pagefile_vnode = vnode;
	return 0;
}
