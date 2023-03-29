/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 29 2023.
 */

#include "kdk/kernel.h"
#include "kdk/process.h"
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

	while (true) {
		ke_timer_set(&timer, NS_PER_S);

		w = ke_wait(&timer, "vm_pagecleaner:timer", false, false, -1);
		kassert(w == kKernWaitStatusOK);

		/* aim to have cleaned all pages within 30s */
		unsigned clean_target = MAX2(pgcleaner_stats.dirty / 30,
		    (pgcleaner_stats.clean + pgcleaner_stats.dirty) / 60);
		ipl_t ipl = vmp_acquire_pfn_lock();

		kdprintf(
		    "Pagecleaner is awake!\nClean: %u Dirty: %u\nAim to clean: %u\n",
		    pgcleaner_stats.clean, pgcleaner_stats.dirty, clean_target);

		for (unsigned i = 0; i < clean_target; i++) {
			struct vmp_objpage *opage;
			int r;
			bool dirty;

			if (TAILQ_EMPTY(&dirty_queue)) {
				kdprintf("Dirty queue empty!\n");
				break;
			}

			opage = TAILQ_LAST(&dirty_queue,
			    vmp_objpage_dirty_queue);

			if (opage->page->wirecnt > 0) {
				kdprintf("Page is wired\n") goto replace;
			}

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
				pgcleaner_stats.dirty--;
				pgcleaner_stats.clean++;
				TAILQ_INSERT_TAIL(&clean_queue, opage,
				    dirtyqueue_entry);
				page_unlock_owner(opage->page);
				continue;
			} else {
				kdprintf("Page %lu is dirty\n",
				    (uintptr_t)opage->page->pfn);
				page_unlock_owner(opage->page);
			}
		}

		vmp_release_pfn_lock(ipl);
	}
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
vmp_objpage_dirty(struct vmp_objpage *opage)
{
	ipl_t ipl = vmp_acquire_pfn_lock();

	if (opage->stat == kVMPObjPageDirty)
		goto out;

	pgcleaner_stats.clean--;
	pgcleaner_stats.dirty++;
	opage->stat = kVMPObjPageDirty;
	TAILQ_REMOVE(&clean_queue, opage, dirtyqueue_entry);
	TAILQ_INSERT_HEAD(&dirty_queue, opage, dirtyqueue_entry);

out:
	vmp_release_pfn_lock(ipl);
}