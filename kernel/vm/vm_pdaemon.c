/*!
 * @file vm_pdaemon.c
 * @brief The page daemon.
 */

#include <nanokern/kernmisc.h>
#include <nanokern/thread.h>
#include <vm/vm.h>



/*!
 * (~) => const forever
 */
struct vm_pdaemon {
	/*!
	 * at how few pages in the inactive queue do we want to migrate pages
	 * from active to inactive?
	 *
	 * cared about by: the page allocator(?)
	 */
	size_t inactive_lowat;
	/*!
	 * at how many pages in the inactive queue do we want to cease migrating
	 * pages from active to inactive?
	 */
	size_t inactive_hiwat;
	/*!
	 * at how few pages on the freeq do we want to start putting back
	 * pages from the inactive queue to the freeq?
	 *
	 * page allocation is frozen at this point until free_hiwat is reached
	 */
	size_t free_lowat;
	/*!
	 * at how many pages on the freeq do we want to cease putting back pages
	 * from the inactive queue to the freeq?
	 */
	size_t free_hiwat;
	/*!
	 * event to wait on before activating pagedaemon
	 */
	kevent_t wanted;
	/*!
	 * event to wait on to wait for free pages
	 */
	kevent_t free_event;
};

struct vm_pdaemon pgd_state;

/*! \pre vm_pgq_lock held */
static kmutex_t *
page_owner_lock(vm_page_t *page)
{
	if (page->anon)
		return &page->anon->lock;
	else
		return &page->obj->lock;
}

/*!
 * @param try whether to only *try* to lock the page (this is probably always
 * the right course of action)
 * \pre vm_pgq_lock held.
 * @retval true page owner was locked.
 */
static bool
page_lock_owner(vm_page_t *page, bool try)
{
	nk_assert(try);
	kwaitstatus_t stat = nk_wait(page_owner_lock(page),
	    "pagedaemon: lock page owner", false, false, -1);
	return (stat == kKernWaitStatusOK);
}

/*!
 *
 */
static void
page_unlock_owner(vm_page_t *page)
{
	nk_mutex_release(page_owner_lock(page));
}

void
scan_active(void)
{
	vm_page_t *page;

	nk_dbg("scan_active()\n");

	/* sweep 1; if accessed, then skip */
	while (true) {
		ipl_t ipl;

		/* todo: i think this can loop forever */

		if (vm_pginactiveq.npages >= pgd_state.inactive_hiwat)
			break;

		ipl = VM_PGQ_LOCK();
		page = TAILQ_LAST(&vm_pgactiveq.queue, pagequeue);

		if (page->busy)
			goto next;

		if (page_lock_owner(page, true)) {
			if (pmap_page_accessed(page, true)) {
				/* replace it to head of active queue */
				vm_page_changequeue(page, &vm_pgactiveq,
				    &vm_pgactiveq);
				nk_dbg("keeping active page %p\n", page);
			} else {
				/* put to head of inactive queue*/
				vm_page_changequeue(page, &vm_pgactiveq,
				    &vm_pginactiveq);
			}

			page_unlock_owner(page);
		}

	next:
		VM_PGQ_UNLOCK(ipl);
	}
}

void
scan_inactive(void)
{
	vm_page_t *page;

	nk_dbg("scan_inactive()\n");

	/* sweep 1; if accessed, then skip */
	while (true) {
		ipl_t ipl;

		/* todo: i think this can loop forever */

		if (vm_pgfreeq.npages >= pgd_state.inactive_hiwat)
			break;

		ipl = VM_PGQ_LOCK();
		page = TAILQ_LAST(&vm_pginactiveq.queue, pagequeue);

		if (page->busy) {
			nk_dbg("page 0x%lx is busy\n", page->paddr);
			goto next;
		}

		if (page_lock_owner(page, true)) {
			nk_assert(!page->busy);
			if (pmap_page_accessed(page, false)) {
				/* replace to head of active queue */
				vm_page_changequeue(page, &vm_pginactiveq,
				    &vm_pgactiveq);
				/* reset accessed bits for tracking */
				(void)pmap_page_accessed(page, true);
				nk_dbg("keeping inactive page 0x%lx\n", page->paddr);

			} else {
				page->busy = true;
				/* remove mappings */
				pmap_unenter_all(page);
				/* page out .... */
				vm_pager_ret_t r=  vm_swp_pageout(page);
				/* unbusy page etc */
				nk_dbg("swapping out page 0x%lx: %d\n", page->paddr, r);
			}
		} else {
			nk_dbg("failed to lock owner of page 0x%lx, continuing\n",
			    page->paddr);
		}

		page_unlock_owner(page);

	next:
		VM_PGQ_UNLOCK(ipl);
	}
}

void
vm_pdaemon(void *unused)
{
	/* this stupid algorithm will at least let us test the paging code */

	nk_event_init(&pgd_state.wanted, false);
	nk_event_init(&pgd_state.free_event, true);

	/* begin swapping heavily at 10% total pages free */
	pgd_state.free_lowat = vm_npages / 10;
	/* stop swapping at 14.5%ish total pages free */
	pgd_state.free_hiwat = vm_npages / 7;

	/*
	 * begin migrating from active to inactive queue at <=20% total pages on
	 * inactive
	 */
	pgd_state.inactive_lowat = vm_npages / 5;
	/*
	 * stop migrating at >= 25% total pages on inactive
	 */
	pgd_state.inactive_hiwat = vm_npages / 4;

	nk_dbg(
	    "pagedaemon: free lowat %lu, hiwat %lu; inactive lowat %lu, hiwat %lu\n",
	    pgd_state.free_lowat, pgd_state.free_hiwat,
	    pgd_state.inactive_lowat, pgd_state.inactive_hiwat);

	while (true) {
		nk_wait(&pgd_state.wanted, "pagedaemon: wait for need", false,
		    false, -1);

		nk_dbg("vm_pagedaemon: scanning queues\n");

		nk_dbg(
		    "pagedaemon: free lowat %lu, hiwat %lu; inactive lowat %lu, hiwat %lu\n",
		    pgd_state.free_lowat, pgd_state.free_hiwat,
		    pgd_state.inactive_lowat, pgd_state.inactive_hiwat);
		nk_dbg("active: %lu inactive: %lu free: %lu\n",
		    vm_pgactiveq.npages, vm_pginactiveq.npages,
		    vm_pgfreeq.npages);

		if (vm_pginactiveq.npages <= pgd_state.inactive_lowat) {
			scan_active();
		}
		if (vm_pgfreeq.npages < pgd_state.free_hiwat) {
			scan_inactive();
		}

		if (vm_pgfreeq.npages >= pgd_state.free_hiwat) {
			nk_event_clear(&pgd_state.wanted);
			nk_event_signal(&pgd_state.free_event);
		} else {
			// nk_fatal("failed to free enough memory\n");
			/* carry on */
		}
	}
}

bool
vm_enoughfree(void)
{
	if (vm_pgfreeq.npages <= pgd_state.free_lowat) {
		vm_pd_signal();
		return false;
	}
	return true;
}

void
vm_pd_signal(void)
{
	nk_event_clear(&pgd_state.free_event);
	nk_event_signal(&pgd_state.wanted);
}

void
vm_pd_wait(void)
{
	nk_wait(&pgd_state.free_event, "vm_pd_wait", false, false, -1);
}
