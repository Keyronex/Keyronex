#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/nanokern.h"
#include "kdk/queue.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "ubc.h"
#include "vm/vmp.h"

/*
 * quick notes
 * - vm_objects don't themselves have refcnts here. it may be better to leave
 * them without....
 * - anonymous vm_objects probably should be wrapped in something with a refcnt
 * - file vm_objects could be tied to lifespan of vnode
 * - each file vm_object mapping, then, adds a reference to the vnode.
 * - therefore  we don't need to worry about dirty pages in working sets
 * - we do need to worry about dirty pages NOT in the working set though
 * - at eviction, we either have an object still mapped, or we are in the
 * process of, but not finished, unmapping it
 * - that means the vnode is still referenced, so we can still additionally
 * retain the vnode.
 * - what we probably want to do is this: when we evict a file page from a
 * working set, iff the underlying page is NOT yet marked dirty, then we should
 * mark it dirty, and increment the vnode refcnt
 * - at page cleaning time, while pfn lock is still held, we can unref the vnode
 * ref (pfn lock held = page isn't dirtied again meantime); or rather we can do
 * that later, without PFN lock held, secure in the knowledge that because of
 * the above principles, the vnode will still be referenced if someone else is
 * pushing out a dirty page to it.
 *
 * - the UBC however distinctively does NOT reference the vnode...
 * - it's no problem because there are only 3 circumstances in which the UBC
 * will touch the vnode:
 * 1. cached read/write: the vnode must be referenced before this operation...
 * 2. window replacement
 * 3. vnode teardown + ubc window destruction
 * - window replacement is done with UBC window lock + pfn lock as each page
 * evicted from working set;
 * - vnode teardown takes the UBC window lock; it's thereby synchronised against
 * window replacement, and it's already synchronized against cached read/write
 * because vnodes are not destroyed while referenced.
 *
 * - eager writeback still needs some thought.
 * - the UBC can help out by pushing out the dirty bits to each page after it's
 * dealt with a window.
 * - this is actually NECESSARY for vnodes to stay alive while dirty, i think,
 * because of UBC not referencing vnodes itself... thus important that dirtiness
 * is then pushed out before we drop the vnode reference around a cached write,
 * because above consideration that dirty pages within a working set always have
 * a refernece by virtue of there being a mapping does not apply.
 * - i don't see any reason this shouldn't be possible and can't think of any
 * problems at present with this...
 *
 * - file truncation might (or might not!) be a nightmare especially if it's
 * supposed to truncate existing mappings. i will figure it out...
 */

static inline intptr_t
ubc_window_cmp(ubc_window_t *x, ubc_window_t *y)
{
	if (x->offset < y->offset)
		return -1;
	else if (x->offset > y->offset)
		return 1;
	else
		return 0;
}

kspinlock_t ubc_lock = KSPINLOCK_INITIALISER;
TAILQ_HEAD(, ubc_window) ubc_lruqueue = TAILQ_HEAD_INITIALIZER(ubc_lruqueue),
			 ubc_freequeue = TAILQ_HEAD_INITIALIZER(ubc_freequeue);
ubc_window_t *window_array;
size_t window_count;

RB_GENERATE(ubc_window_tree, ubc_window, rb_entry, ubc_window_cmp);

static void
window_replace(ubc_window_t *window)
{
	pte_t *pte = NULL;
	vaddr_t window_addr = ubc_window_addr(window);

	window->refcnt++;
	vn_retain(window->vnode);
	TAILQ_REMOVE(&ubc_lruqueue, window, queue_entry);
	RB_REMOVE(ubc_window_tree, &window->vnode->ubc_windows, window);
	ke_spinlock_release(&ubc_lock, kIPLAST);

	KE_WAIT(&kernel_procstate.ws_mutex, false, false, -1);
	(void)vmp_acquire_pfn_lock();

	vmp_fetch_pte(&kernel_procstate, window_addr, &pte);
	kassert(pte != NULL);

	for (int i = 0; i < UBC_WINDOW_SIZE / PGSIZE; i++) {
		kassert(vmp_pte_characterise(pte + i) == kPTEKindValid ||
		    vmp_pte_characterise(pte + i) == kPTEKindZero);
		if (vmp_md_pte_is_valid(pte + i)) {
			vm_page_t *pte_page = vm_paddr_to_page(
			    PGROUNDDOWN(V2P(pte)));
			vmp_wsl_remove(&kernel_procstate,
			    window_addr + i * PGSIZE, true);
			vmp_page_evict(&kernel_procstate, pte + i, pte_page,
			    window_addr + i * PGSIZE);
		}
	}

	vmp_release_pfn_lock(kIPLAST);
	vn_release(window->vnode);
	ke_mutex_release(&kernel_procstate.ws_mutex);

	ke_spinlock_acquire(&ubc_lock);
	window->refcnt = 0;
	TAILQ_INSERT_HEAD(&ubc_freequeue, window, queue_entry);
}

static ubc_window_t *
take_window(vnode_t *vnode, io_off_t offset)
{
	ubc_window_t *window, key;

	kassert(offset % UBC_WINDOW_SIZE == 0);
	key.offset = offset / UBC_WINDOW_SIZE;

retry:
	window = RB_FIND(ubc_window_tree, &vnode->ubc_windows, &key);
	if (window != NULL) {
		if (window->refcnt == 0)
			TAILQ_REMOVE(&ubc_lruqueue, window, queue_entry);
		window->refcnt++;
		return window;
	}

	window = TAILQ_FIRST(&ubc_freequeue);
	if (window != NULL) {
		TAILQ_REMOVE(&ubc_freequeue, window, queue_entry);
		window->refcnt = 1;
	} else {
		window = TAILQ_FIRST(&ubc_lruqueue);
		kassert(window != NULL);
		window_replace(window);
		/*
		 * retry, as we dropped the spinlock so someone else may have
		 * made a window in the intervening period
		 */
		goto retry;
	}

	window->vnode = vnode;
	window->offset = offset / UBC_WINDOW_SIZE;

	return window;
}

static void
put_window(ubc_window_t *window)
{
	window->refcnt--;
	if (window->refcnt == 0)
		TAILQ_INSERT_TAIL(&ubc_lruqueue, window, queue_entry);
}

int
ubc_io(vnode_t *vnode, vaddr_t user_addr, io_off_t off, size_t size, bool write)
{
	size_t done = 0;

	/*
	 * The vnode general rwlock is acquired to keep file size from
	 * manipulation during the I/O.
	 */
	KE_WAIT(vnode->rwlock, false, false, -1);

	while (done != size) {
		io_off_t window_off = ROUNDDOWN(off + done, UBC_WINDOW_SIZE);
		size_t window_internal_off = (off + done) % UBC_WINDOW_SIZE;
		size_t size_from_window = UBC_WINDOW_SIZE - window_internal_off;
		vaddr_t window_addr;
		ubc_window_t *window;
		ipl_t ipl;

		size_from_window = MIN2(size_from_window, size - done);

		ipl = ke_spinlock_acquire(&ubc_lock);
		window = take_window(vnode, window_off);
		ke_spinlock_release(&ubc_lock, ipl);

		window_addr = ubc_window_addr(window);
		if (write)
			memcpy((void *)(window_addr + window_internal_off), (void *)user_addr,
			    size_from_window);
		else
			memcpy((void *)user_addr, (void *)(window_addr + window_internal_off),
			    size_from_window);

		ipl = ke_spinlock_acquire(&ubc_lock);
		put_window(window);
		ke_spinlock_release(&ubc_lock, ipl);

		done += size_from_window;
	}

	ke_mutex_release(vnode->rwlock);

	return done;
}

void
ubc_init(void)
{
	window_count = 2;

	window_array = kmem_alloc(sizeof(ubc_window_t) * window_count);

	kprintf("ubc_init: %zu windows in Unified Buffer Cache\n",
	    window_count);
	for (int i = 0; i < window_count; i++)
		TAILQ_INSERT_TAIL(&ubc_freequeue, &window_array[i],
		    queue_entry);
}
