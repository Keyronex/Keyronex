#include "kdk/kmem.h"
#include "kdk/nanokern.h"
#include "kdk/vm.h"
#include "nanokern/ki.h"
#include "vm/vmp_dynamics.h"
#include "vmp.h"

#define MAX_PROBING_ATTEMPTS 4
#define WS_EXPANSION_COUNT 16
#define WSE_PER_PAGE (PGSIZE / sizeof(struct wse))

typedef struct wse {
	uintptr_t vpfn
	    : PFN_BITS,	  /* virtual address or next free  */
	      spare : 6,  /* spare bits */
	      age : 3,	  /* age from 0 to 7 */
	      hashed : 1, /* whether inserted into hashtable */
	      shared : 1, /* shared page? if 1, can't use ws_vaddr_to_wse() */
	      free : 1;	  /* set to 0 */
} wse_t;

static void wse_evict(struct vmp_wsl *ws, struct wse *wse);

#if BITS == 64
static inline uintptr_t
vpfn_hash(uintptr_t key, size_t hash_size)
{
	key = (~key) + (key << 21);
	key = key ^ (key >> 24);
	key = (key + (key << 3)) + (key << 8);
	key = key ^ (key >> 14);
	key = (key + (key << 2)) + (key << 4);
	key = key ^ (key >> 28);
	key = key + (key << 31);
	return key % (hash_size - 1);
}
#else
static inline uintptr_t
vpfn_hash(uintptr_t key, size_t hash_size)
{
	uintptr_t c2 = 0x27d4eb2d;
	key = (key ^ 61) ^ (key >> 16);
	key = key + (key << 3);
	key = key ^ (key >> 4);
	key = key * c2;
	key = key ^ (key >> 15);
	return key % (hash_size - 1);
}
#endif

int
vmp_wsl_init(vm_procstate_t *vmps, struct vmp_wsl *ws)
{
	ws->vmps = vmps;
	ws->nodes = (wse_t *)vm_kalloc(1, 0);
	ws->capacity = WSE_PER_PAGE;
	ws->limit = 6;
	ws->size = 0;
	ws->head = 0;
	ws->freelist = NIL_WSE;
	/*
	 * todo:
	 * could do kmem_alloc(sizeof(wsindex_t) * WSE_PER_PAGE);
	 * but need to disentangel
	 */
	ws->hash = (wsindex_t *)vm_kalloc(1, 0);
	ws->hash_size = WSE_PER_PAGE;
	for (int i = WSE_PER_PAGE; i > 0; i--) {
		wsindex_t idx = i - 1;
		ws->nodes[idx].free = 1;
		ws->nodes[idx].vpfn = ws->freelist;
		ws->freelist = idx;
		ws->hash[i] = NIL_WSE;
	}
	return 0;
}

void
hash_insert(struct vmp_wsl *ws, wsindex_t index)
{
	uintptr_t hash_index = vpfn_hash(ws->nodes[index].vpfn, ws->hash_size);
	for (int i = 0; i < MAX_PROBING_ATTEMPTS; i++) {
		if (ws->hash[hash_index] == NIL_WSE) {
			ws->hash[hash_index] = index;
			ws->nodes[index].hashed = true;
			return;
		}
		hash_index = (hash_index + 1) % ws->hash_size;
	}

	ws->nodes[index].hashed = false;
}

wsindex_t
hash_lookup(struct vmp_wsl *ws, uintptr_t vpfn, bool delete)
{
	uintptr_t hash_index = vpfn_hash(vpfn, ws->hash_size);

	for (int i = 0; i < MAX_PROBING_ATTEMPTS; i++) {
		wsindex_t idx = ws->hash[hash_index];
		if (idx != NIL_WSE && ws->nodes[idx].vpfn == vpfn) {
			if (delete) {
				kassert(ws->nodes[idx].hashed);
				ws->hash[hash_index] = NIL_WSE;
				ws->nodes[idx].hashed = false;
			}
			return idx;
		}
		hash_index = (hash_index + 1) % ws->hash_size;
	}

	for (wsindex_t idx = 0; idx < ws->capacity; idx++) {
		if (ws->nodes[idx].vpfn == vpfn && !ws->nodes[idx].free) {
			kassert(!ws->nodes[idx].hashed);
			return idx;
		}
	}

	return NIL_WSE;
}

void
hash_remove(struct vmp_wsl *ws, wsindex_t index)
{
	uintptr_t vpfn = ws->nodes[index].vpfn;
	uintptr_t hash_index = vpfn_hash(vpfn, ws->hash_size);
	for (int i = 0; i < MAX_PROBING_ATTEMPTS; i++) {
		if (ws->hash[hash_index] == index) {
			ws->hash[hash_index] = NIL_WSE;
			ws->nodes[index].hashed = false;
			return;
		}
		hash_index = (hash_index + 1) % ws->hash_size;
	}
	kfatal("unreached\n");
}

void
ws_expand(struct vmp_wsl *ws)
{
#if 0
	size_t new_capacity = ws->capacity + WSE_PER_PAGE;
	wse_t *new_entries = (wse_t *)vm_krealloc((vaddr_t)ws->nodes,
	    ws->capacity * sizeof(wse_t), sizeof(wse_t) * new_capacity, 0);

	kprintf("Expanding!...");

	if (!new_entries) {
		kfatal("Fixme: working set expansion failed\n");
	}

	for (size_t i = new_capacity; i > ws->capacity; i--) {
		wsindex_t idx = i - 1;
		new_entries[idx].free = 1;
		new_entries[idx].vpfn = ws->freelist;
		ws->freelist = idx;
	}

	ws->nodes = new_entries;
	ws->capacity = new_capacity;
#else
	kfatal("Implement me\n");
#endif
}

static bool
ws_can_expand(struct vmp_wsl *ws)
{
	/* can't expand beyond one page yet*/
	return ws->limit != ws->capacity && !vmp_avail_pages_fairly_low();
}

wsindex_t
vmp_wsl_insert(vm_procstate_t *ps, vaddr_t vaddr, bool shared)
{
	wsindex_t idx;
	wse_t *wse;
	pfn_t vpfn = vaddr >> VMP_PAGE_SHIFT;
	struct vmp_wsl *ws = &ps->wsl;

#if TRACE_WS
	kprintf("Inserting vpfn %zu...", vpfn);
#endif

	if (ws->size < ws->limit || ws_can_expand(ws)) {

		if (ws->size == ws->limit) {
			if (ws->limit == ws->capacity) {
				/*
				 * working set has NO room for more WSEs, but we
				 * are allowed to expand - carry out an actual
				 * expansion of the working set list, this will
				 * put more entries onto the freelist.
				 */

				ws_expand(ws);
			}

			ws->limit = MIN2(ws->limit + WS_EXPANSION_COUNT,
			    ws->capacity);
			kprintf("WS limit raised to %zu\n", ws->limit);
		}

		/* take an entry from the freelist */
#if TRACE_WS
		kprintf("taking freelist entry (%u)\n", ws->freelist);
#endif

		ws->size++;

		kassert(ws->freelist != NIL_WSE);
		idx = ws->freelist;
		wse = &ws->nodes[idx];

		wse->free = 0;
		ws->freelist = wse->vpfn;
	} else {
		wsindex_t iter;

		iter = ws->head;
		while (true) {
			idx = iter;
			wse = &ws->nodes[idx];
			iter = (iter + 1) % ws->capacity;

			if (wse->free)
				continue;

			break;
		}
		ws->head = iter;
		wse_evict(ws, wse);
		if (wse->shared && wse->hashed)
			hash_remove(ws, idx);
	}

	wse->vpfn = vpfn;
	wse->shared = shared;
	if (shared)
		hash_insert(ws, idx);

	return 0;
}

void
vmp_wsl_remove(vm_procstate_t *ps, vaddr_t vaddr, wsindex_t hint)
{
	wsindex_t idx = hint;
	wse_t *wse = NULL;
	pfn_t vpfn = vaddr >> VMP_PAGE_SHIFT;
	struct vmp_wsl *ws = &ps->wsl;

	if (idx != NIL_WSE && idx < ws->capacity) {
		wse = &ws->nodes[idx];
		/* hint was wrong */
		if (wse->vpfn != vpfn)
			wse = NULL;
	}

	if (wse == NULL) {
		idx = hash_lookup(ws, vpfn, true);
		kassert(idx != NIL_WSE);
		wse = &ws->nodes[idx];
	}

#if TRACE_WS
	kprintf("Freeing vpfn %zu from index %u\n", vpfn, idx);
#endif
	wse->free = 1;
	wse->vpfn = ws->freelist;
	ws->freelist = idx;
	ws->size--;
}

void
vmp_wsl_dump(vm_procstate_t *ps)
{
	struct vmp_wsl *ws = &ps->wsl;

	for (int i = 0; i < ws->capacity; i++) {
		uintptr_t vpfn = (uintptr_t)ws->nodes[i].vpfn;

		if (ws->nodes[i].free) {
#if WRACE_WS_INCLUDE_FREE
			if (vpfn == NIL_WSE)
				kprintf(" Free %d: next=(nothing)\n", i);
			else
				kprintf(" Free %d: next=%zu\n", i, vpfn);
#endif
		} else {

			kprintf("Entry %d: vpfn=0x%zx\n", i, vpfn);
		}
	}
	kprintf("Capacity: %zu, Limit: %zu, Size: %zu\n", ws->capacity,
	    ws->limit, ws->size);
}

void
vmp_wsl_trim(struct vmp_wsl *ws, size_t n)
{
	size_t pages_trimmed = 0;

	ke_mutex_assert_held(&ws->vmps->ws_mutex);

	while (pages_trimmed < n && ws->size > 0) {
		wsindex_t iter = ws->head;

		while (true) {
			if (!ws->nodes[iter].free) {
				break;
			}
			iter = (iter + 1) % ws->capacity;
		}

		wse_t *wse = &ws->nodes[iter];
		ws->head = (iter + 1) % ws->capacity;
		wse_evict(ws, wse);
		if (wse->shared && wse->hashed)
			hash_remove(ws, iter);

		wse->free = 1;
		wse->vpfn = ws->freelist;
		ws->freelist = iter;
		ws->size--;

		pages_trimmed++;
	}
}

static void
wse_evict(struct vmp_wsl *ws, wse_t *wse)
{
	vm_page_t *pte_page;
	pte_t *pte;
	int r;
	ipl_t ipl;
	vaddr_t vaddr = wse->vpfn << VMP_PAGE_SHIFT;

	ipl = vmp_acquire_pfn_lock();
	r = vmp_fetch_pte(ws->vmps, vaddr, &pte);
	kassert(r == 0);
	kassert(vmp_md_pte_is_valid(pte));

	pte_page = vm_paddr_to_page(PGROUNDDOWN(V2P(pte)));

	vmp_page_evict(ws->vmps, pte, pte_page, vaddr);
	vmp_release_pfn_lock(ipl);
}

void
vmp_page_evict(vm_procstate_t *vmps, pte_t *pte, vm_page_t *pte_page,
    vaddr_t vaddr)
{
	bool dirty = vmp_md_hw_pte_is_writeable(pte);
	vm_page_t *page = vmp_pte_hw_page(pte, 1);
	bool became_dirty = false;

	kassert(vmp_pte_characterise(pte) == kPTEKindValid);

	if (!page->dirty && dirty)
		became_dirty = true;

	page->dirty |= dirty;

	switch (page->use) {
	case kPageUseAnonPrivate: {
		/*
		 * we need to replace this with a transition PTE then.
		 * used_ptes and noswap_ptes count is as such unchanged.
		 */
#if 0 /* bad check - could be referenced elsewhere... */

		kassert(page->refcnt == 1);
#endif
		kassert(page->referent_pte == V2P(pte));
		vmp_md_pte_create_trans(pte, page->pfn);
		ki_tlb_flush_vaddr_globally(vaddr);
		vmp_page_release_locked(page);
		break;
	}

	case kPageUseAnonShared:
	case kPageUseFileShared:
		(void)became_dirty; /* could do refcnt stuff here */
		vmp_md_pte_create_zero(pte);
		vmp_pagetable_page_pte_deleted(vmps, pte_page, false);
		ki_tlb_flush_vaddr_globally(vaddr);
		vmp_page_release_locked(page);
		break;

	default:
		kfatal("Unhandled page use in working set eviction\n");
	}
}
