/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 29 2023.
 */

#include "kdk/amd64/vmamd64.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "vm/vm_internal.h"

static void cleaner_loop(void *);

static ethread_t cleaner_thread;
/* clang-format off */
static struct vmp_objpage_dirty_queue
    dirty_queue = TAILQ_HEAD_INITIALIZER(dirty_queue),
    clean_queue = TAILQ_HEAD_INITIALIZER(clean_queue);
/* clang-format on */

static struct {
	unsigned dirty;
	unsigned clean;
} pgcleaner_stats;

int
vm_cleaner_init(void)
{
	ps_create_system_thread(&cleaner_thread, "vm_pagecleaner", cleaner_loop,
	    NULL);
	ki_thread_start(&cleaner_thread.kthread);
	return 0;
}

static kmutex_t *
page_owner_lock(vm_page_t *page)
{
	kmutex_t *mtx;
	if (page->use == kPageUseAnonymous)
		mtx = &page->anon->mutex;
	else {
		kassert(page->use == kPageUseObject);
		mtx = &page->obj->mutex;
	}
	kassert(mtx != NULL);
	return mtx;
}

static bool
page_trylock_owner(vm_page_t *page)
{
	kwaitstatus_t stat = ke_wait(page_owner_lock(page),
	    "pagedaemon: lock page owner", false, false, 0);
	return (stat == kKernWaitStatusOK);
}

static void
page_unlock_owner(vm_page_t *page)
{
	ke_mutex_release(page_owner_lock(page));
}

static void
cleaner_loop(void *)
{
	ktimer_t timer;
	kwaitstatus_t w;

	ke_timer_init(&timer);

loop:
	ke_timer_set(&timer, NS_PER_S);

	w = ke_wait(&timer, "vm_pagecleaner:timer", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	/* aim to have cleaned all pages within 30s */
	unsigned clean_target = MAX2(pgcleaner_stats.dirty / 30,
	    (pgcleaner_stats.clean + pgcleaner_stats.dirty) / 60);
	ipl_t ipl = vmp_acquire_pfn_lock();

#if 0
	kdprintf("Pagecleaner is awake!\n"
		 "Clean: %u Dirty: %u\n"
		 "Aim to clean: %u\n",
	    pgcleaner_stats.clean, pgcleaner_stats.dirty, clean_target);
#endif

	for (unsigned i = 0; i < clean_target; i++) {
		struct vmp_objpage *opage;
		int r;
		bool dirty;

		if (TAILQ_EMPTY(&dirty_queue)) {
			break;
		}

		opage = TAILQ_LAST(&dirty_queue, vmp_objpage_dirty_queue);

		/*
		 * todo: should wiring forbid write-back? need to think about
		 * how we deal with the dirtying of wired pages.
		 */
		if (opage->page->wirecnt > 0) {
			kdprintf("Page is wired\n");
			goto replace;
		}

		kassert(opage->stat == kVMPObjPageDirty);
		TAILQ_REMOVE(&dirty_queue, opage, dirtyqueue_entry);

		r = page_trylock_owner(opage->page);
		if (!r) {
			kdprintf("Failed to lock owner!\n");
		replace:
			TAILQ_INSERT_TAIL(&dirty_queue, opage,
			    dirtyqueue_entry);
			continue;
		}

		dirty = pmap_pageable_undirty(opage->page);
		if (!dirty) {
			opage->stat = kVMPObjPageClean;
			opage->page->obj->ndirty--;
			pgcleaner_stats.dirty--;
			pgcleaner_stats.clean++;
			TAILQ_INSERT_TAIL(&clean_queue, opage,
			    dirtyqueue_entry);
			page_unlock_owner(opage->page);

			if (opage->page->obj->ndirty == 0)
				/*
				 * the number of dirty pages has dropped to 0;
				 * it's time to drop the reference we made to
				 * the vnode when its first page was dirtied..
				 */
				obj_direct_release(opage->page->obj->vnode);
			continue;
		} else {
			uintptr_t pfn = opage->page->pfn;
			kdprintf("Page %lu is dirty\n",
			    pfn);

			vm_mdl_t *mdl;
			vm_page_t *page = opage->page;
			vnode_t *vnode = obj_direct_retain(page->obj->vnode);

			kassert(!page->obj->is_anonymous);

			opage->stat = kVMPObjPageClean;
			pgcleaner_stats.dirty--;
			pgcleaner_stats.clean++;
			TAILQ_INSERT_TAIL(&clean_queue, opage,
			    dirtyqueue_entry);
			page_unlock_owner(page);
			vmp_release_pfn_lock(ipl);

			vnode = page->obj->vnode;
			mdl = vm_mdl_alloc(1);

			/* should handling of this move into the vnode op? */
			size_t actual_size;

			if (opage->page_index * PGSIZE + PGSIZE > vnode->size)
				actual_size = vnode->size -
				    opage->page_index * PGSIZE;
			else
				actual_size = PGSIZE;

			mdl->pages[0] = page;
			iop_t *iop = iop_new_write(vnode->vfsp->dev, mdl,
			    actual_size, opage->page_index * PGSIZE);
			iop->stack[0].vnode = vnode;

			iop_return_t res = iop_send_sync(iop);
			kassert(res == kIOPRetCompleted);

			/*
			 * !!! xxx we need to make a real solution here.
			 * clean up how we deal with this, how we deal with
			 * the refcounting of dirty pages and so on.
			 * we also probably want a lock around paging i/o for
			 * a file that inhibits the file being shrank etc.
			 */

			page->obj->ndirty--;
			if (page->obj->ndirty == 0) {
				obj_direct_release(vnode);
			}

			obj_direct_release(vnode);

			ipl = vmp_acquire_pfn_lock();
		}
	}

	vmp_release_pfn_lock(ipl);
	goto loop;
}

void
vmp_objpage_created(struct vmp_objpage *opage)
{
	ipl_t ipl = vmp_acquire_pfn_lock();

	pgcleaner_stats.clean++;
	opage->stat = kVMPObjPageClean;
	opage->page->dirty = false;
	TAILQ_INSERT_HEAD(&clean_queue, opage, dirtyqueue_entry);

	vmp_release_pfn_lock(ipl);
}

void
vmp_objpage_dirty(vm_object_t *obj, struct vmp_objpage *opage)
{
	ipl_t ipl = vmp_acquire_pfn_lock();

	if (opage->stat == kVMPObjPageDirty)
		goto out;

	obj->ndirty++;
	if (obj->ndirty == 1)
		/*
		 * note: dirty vnodes are retained until all pages cleaned.
		 * when obj->ndirty hits one, the reference is added.
		 */
		obj_direct_retain(obj->vnode);

	pgcleaner_stats.clean--;
	pgcleaner_stats.dirty++;
	opage->stat = kVMPObjPageDirty;
	TAILQ_REMOVE(&clean_queue, opage, dirtyqueue_entry);
	TAILQ_INSERT_HEAD(&dirty_queue, opage, dirtyqueue_entry);

out:
	vmp_release_pfn_lock(ipl);
}

void
vmp_objpage_free(vm_map_t *map, struct vmp_objpage *opage)
{
	kassert(opage->stat == kVMPObjPageClean);
	vmp_page_free(map, opage->page);
	opage->page = NULL;
	TAILQ_REMOVE(&clean_queue, opage, dirtyqueue_entry);
	kmem_free(opage, sizeof(*opage));
}
