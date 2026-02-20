
/*
 * notes:
 *
 * truncation logic will acquire the same lock exclusive as lock_for_vc_io does
 * - that means only writeback ios can be ongoing while truncation is happening.
 * - so (i think) we only need to have a way to wait for writeback ios to finish
 * - truncation can otherwise make away with views beyond truncation point
 * without any issue.
 *
 * while a view has refcnt > 0, it's in the kernel map. so just having refcnt
 * > 0 on a view stabilises its existence in the virtual address space.
 *
 * If we need a wait to wait for a view without holding a spinlock, for example,
 * to wait for a view to be written back, we could have a
 * struct view_waiter { TAILQ_ENTRY(view_waiter) queue_entry; kevent_t ev; };
 * and have a TAILQ_HEAD(view_waiter_list, view_waiter) waiters; in struct view.
 * and then when the view is written back, chase that list and wake up the
 * waiters who tied themselves to the chain.
 */

#include <sys/vm.h>
#include <libkern/lib.h>
#include <sys/tree.h>
#include <sys/vnode.h>
#include <sys/k_wait.h>

#include <stdint.h>

#include <vm/vc_support.h>
#include "sys/k_cpu.h"
#include "sys/kmem.h"
#include "sys/proc.h"

#define VIEW_SIZE (64 * 1024) /* 64 KiB views */

/*
 * A view retains its vnode, iff the view is dirty or being written back.
 */
struct view {
	size_t refcnt;
	uint64_t offset; /* byte offset in file */
	vnode_t *vnode;	 /* associated vnode; */

	RB_ENTRY(view) rb_entry;       /* link in tree of views for vnode */
	TAILQ_ENTRY(view) queue_entry; /* link in lru, dirty, or free list */

	enum view_dirtiness { VIEW_CLEAN, VIEW_DIRTY, VIEW_WRITEBACK } dirty;
	kabstime_t dirty_time; /* time when marked dirty after being clean */
};

TAILQ_HEAD(view_tq, view);
RB_HEAD(view_tree, view);

struct vn_vc_state {
	struct view_tree view_tree; /* views of this vnode; vc_lock guards it */
};

static size_t view_count;
static struct view *views;
static kspinlock_t vc_lock = KSPINLOCK_INITIALISER;

/*
 * free_queue stores views that are not currently in use.
 * lru_queue stores clean views whose refcnt is at 0.
 * dirty_queue stores dirty views, ordered by time they were marked dirty; their
 * refcnt can be 0 or greater.
 */

static struct view_tq free_queue = TAILQ_HEAD_INITIALIZER(free_queue),
		      lru_queue = TAILQ_HEAD_INITIALIZER(lru_queue),
		      dirty_queue = TAILQ_HEAD_INITIALIZER(dirty_queue);

static inline int
view_cmp(struct view *x, struct view *y)
{
	if (x->offset < y->offset)
		return -1;
	else if (x->offset > y->offset)
		return 1;
	else
		return 0;
}

RB_GENERATE_STATIC(view_tree, view, rb_entry, view_cmp);

static inline vaddr_t
view_addr(struct view *view)
{
	return FILE_MAP_BASE + (uintptr_t)(view - views) * VIEW_SIZE;
}

static inline struct view *
addr_to_view(vaddr_t addr)
{
	return &views[(addr - FILE_MAP_BASE) / VIEW_SIZE];
}

static void
viewcache_writeback_thread(void *)
{
	kevent_t ev;
	ke_event_init(&ev, false);

	while (true) {
		ipl_t ipl;
		struct view *view;

		ke_wait1(&ev, "viewcache_writeback_thread", false,
		    ke_time() + NS_PER_S);

		ipl = ke_spinlock_enter(&vc_lock);
		view = TAILQ_FIRST(&dirty_queue);
		if (view == NULL) {
			ke_spinlock_exit(&vc_lock, ipl);
			continue;
		}

#if 0
		kprintf("viewcache_writeback_thread: writing back view "
		    "offset 0x%zx of vnode %p\n",
		    view->offset, view->vnode);
#endif

		view->dirty = VIEW_WRITEBACK;
		view->dirty_time = ABSTIME_NEVER;
		TAILQ_REMOVE(&dirty_queue, view, queue_entry);
		ke_spinlock_exit(&vc_lock, ipl);

		vm_vc_clean(view->vnode->file.vmobj, view->offset,
		    view_addr(view), VIEW_SIZE);

		ipl = ke_spinlock_enter(&vc_lock);
		kassert(view->dirty == VIEW_WRITEBACK);
		if (view->dirty_time != ABSTIME_NEVER) {
			/* dirtied again while writeback was ongoing */
			view->dirty = VIEW_DIRTY;
			TAILQ_INSERT_TAIL(&dirty_queue, view, queue_entry);
			ke_spinlock_exit(&vc_lock, ipl);
		} else {
			/* now clean */
			view->dirty = VIEW_CLEAN;
			TAILQ_INSERT_TAIL(&lru_queue, view, queue_entry);
			ke_spinlock_exit(&vc_lock, ipl);
		}
	}
}

void
viewcache_init(void)
{
	view_count = 1024;

	views = kmem_alloc(view_count * sizeof(*views));

	for (size_t i = 0; i < view_count; i++) {
		struct view *v = &views[i];
		TAILQ_INSERT_TAIL(&free_queue, v, queue_entry);
	}

	thread_t *thread = proc_new_system_thread(viewcache_writeback_thread, NULL);
	ke_thread_resume(&thread->kthread, false);
}

struct vn_vc_state *
viewcache_alloc_vnode_state(vnode_t *vn)
{
	struct vn_vc_state *state;
	state = kmem_alloc(sizeof(*state));
	RB_INIT(&state->view_tree);
	return state;
}

static void
view_retain_locked(struct view *view)
{
	kassert(ke_spinlock_held(&vc_lock));
	view->refcnt++;
	if (view->refcnt == 1 && view->dirty == VIEW_CLEAN)
		TAILQ_REMOVE(&lru_queue, view, queue_entry);
}

static void
view_release(struct view *view)
{
	ipl_t ipl = ke_spinlock_enter(&vc_lock);
	kassert(view->refcnt > 0);
	view->refcnt--;
	if (view->refcnt == 0 && view->dirty == VIEW_CLEAN)
		TAILQ_INSERT_TAIL(&lru_queue, view, queue_entry);
	ke_spinlock_exit(&vc_lock, ipl);
}

static void
view_dirty_and_release(struct view *view)
{
	ipl_t ipl = ke_spinlock_enter(&vc_lock);

	if (view->dirty == VIEW_CLEAN) {
		view->dirty_time = ke_time();
		view->dirty = VIEW_DIRTY;
		TAILQ_INSERT_TAIL(&dirty_queue, view, queue_entry);
	} else if (view->dirty == VIEW_WRITEBACK) {
		/* this lets the writeback thread know it's dirty again */
		view->dirty_time = ke_time();
	}

	view->refcnt--;
	if (view->refcnt == 0 && view->dirty == VIEW_CLEAN)
		TAILQ_INSERT_TAIL(&lru_queue, view, queue_entry);

	ke_spinlock_exit(&vc_lock, ipl);
}

static void
view_replace(struct view *view)
{
	kassert(view->dirty == VIEW_CLEAN);

	RB_REMOVE(view_tree, &view->vnode->file.vc_state->view_tree, view);

	vm_vc_unmap(view_addr(view), VIEW_SIZE);
}

static struct view *
view_get(vnode_t *vn, uint64_t offset)
{
	struct vn_vc_state *vc_state = vn->file.vc_state;
	struct view key;
	struct view *view;
	ipl_t ipl;

	key.vnode = vn;
	key.offset = offset;

	ipl = ke_spinlock_enter(&vc_lock);

retry:
	view = RB_FIND(view_tree, &vc_state->view_tree, &key);
	if (view != NULL) {
		view_retain_locked(view);
		ke_spinlock_exit(&vc_lock, ipl);
		return view;
	}

	view = TAILQ_FIRST(&free_queue);
	if (view != NULL) {
		TAILQ_REMOVE(&free_queue, view, queue_entry);
		/* factor (1) */
		view->refcnt = 1;
		view->vnode = vn;
		view->offset = offset;
		view->dirty = VIEW_CLEAN;
		RB_INSERT(view_tree, &vc_state->view_tree, view);
		ke_spinlock_exit(&vc_lock, ipl);
		return view;
	}

	view = TAILQ_FIRST(&lru_queue);
	if (view != NULL) {
		TAILQ_REMOVE(&lru_queue, view, queue_entry);
		view_replace(view);
		/* factor (1) */
		view->refcnt = 1;
		view->vnode = vn;
		view->offset = offset;
		view->dirty = VIEW_CLEAN;
		RB_INSERT(view_tree, &vc_state->view_tree, view);
		ke_spinlock_exit(&vc_lock, ipl);
		return view;
	}

	/* kick the writeback thread and wait for a free or lru view? */
	kfatal("implement me: no views\n");

	goto retry;
}

int
viewcache_io(vnode_t *vn, uint64_t offset, size_t length, bool write, void *buf)
{
	size_t done = 0;

	/*
	 * Lock the vnode for viewcache I/O.
	 * This prevents truncation or extension of the file while viewcache I/O
	 * is ongoing.
	 * (Note, this is a DIFFERENT lock than is locked for writeback! Because
	 * we may have to arrange writeback of dirty views of other vnodes while
	 * holding the vc_io lock of the vnode. We make the guarantee in FS
	 * driver truncation logic that truncation first gets rid of views
	 * beyond truncation point, so that we don't need to hold the VC_IO lock
	 * while writeback is happening. Instead, LOCK_FOR_DPW is taken for
	 * writeback. (But we do we even need to LOCK_FOR_DPW in that case? Once
	 * we mark the view as being written back, truncation has to wait for
	 * it!))
	 *
	 * note re above comment: we don't do the writeback of other vnodes from
	 * this context, we kick the writeback thread to do that.
	 */
	VOP_LOCK_FOR_VC_IO(vn, write);

	while (done < length) {
		io_off_t view_off = rounddown2(offset + done, VIEW_SIZE);
		size_t view_internal_off = (offset + done) % VIEW_SIZE;
		size_t size_from_view = MIN2(VIEW_SIZE - view_internal_off,
		    length - done);

		struct view *view = view_get(vn, view_off);
		vaddr_t vaddr = view_addr(view) + view_internal_off;

		if (write) {
			memcpy((void *)vaddr, (void *)((uintptr_t)buf + done),
			    size_from_view);
			view_dirty_and_release(view);
		} else {
			memcpy((void *)((uintptr_t)buf + done), (void *)vaddr,
			    size_from_view);
			view_release(view);
		}

		done += size_from_view;
	}

	VOP_UNLOCK_FROM_VC_IO(vn, write);

	return done;
}

void
viewcache_vmm_get_fault_info(vaddr_t addr, struct vm_object **out_object,
    vaddr_t *out_mapping_start, vaddr_t *out_mapping_end,
    size_t *out_mapping_offset)
{
	struct view *view = addr_to_view(addr);
	*out_object = view->vnode->file.vmobj;
	*out_mapping_start = FILE_MAP_BASE +
	    (uintptr_t)(view - views) * VIEW_SIZE;
	*out_mapping_end = *out_mapping_start + VIEW_SIZE;
	*out_mapping_offset = view->offset;
}

void
viewcache_purge_vnode(vnode_t *vn)
{
	struct vn_vc_state *vc_state = vn->file.vc_state;
	struct view *view;
	ipl_t ipl;

	/*
	 * We assume no one else bar the FS logic, namecache, or dirty page
	 * writer is able to do anything with the vnode, therefore no one is
	 * going to do viewcache I/O on it. So all windows must have a 0 refcnt.
	 *
	 * Note we don't initiate viewcache directed writeback here, because
	 * this function is only called when the vnode is being reclaimed; the
	 * FS logic will have to call on the VMM to write back the dirty pages
	 * of the VM object associated with the vnode after it's called us; we
	 * just use vm_vc_unmap() which pushes out the dirty bits to the VM
	 * pages.
	 */

	ipl = ke_spinlock_enter(&vc_lock);

	while ((view = RB_MIN(view_tree, &vc_state->view_tree)) != NULL) {
		kassert(view->refcnt == 0);

	waited:
		if (view->dirty == VIEW_WRITEBACK) {
			kfatal("wait for writeback...");
			goto waited;
		} else if (view->dirty == VIEW_DIRTY) {
			TAILQ_REMOVE(&dirty_queue, view, queue_entry);
		} else {
			TAILQ_REMOVE(&lru_queue, view, queue_entry);
		}

		RB_REMOVE(view_tree, &vc_state->view_tree, view);

		vm_vc_unmap(view_addr(view), VIEW_SIZE);

		TAILQ_INSERT_TAIL(&free_queue, view, queue_entry);
	}

	ke_spinlock_exit(&vc_lock, ipl);
}
