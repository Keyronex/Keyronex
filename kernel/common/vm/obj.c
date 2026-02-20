/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm/obj.c
 * @brief VM Objects and their tree management.
 */

#include <sys/vm.h>
#include <sys/vnode.h>
#include <sys/kmem.h>

#include <libkern/lib.h>

#include <vm/map.h>
#include <vm/page.h>

#define OBJ_N_DIRECT 6

#define OBJ_PTES_PER_TABLE (PGSIZE / sizeof(pte_t))

#if !defined(__m68k__) /* really BITS == 64 */
#define OBJ_LEVEL_SHIFT 9
#else
#define OBJ_LEVEL_SHIFT 10
#endif

#define OBJ_LEVELS 4

#define FIRST_SINGLY_INDIRECT (OBJ_N_DIRECT)
#define FIRST_DOUBLY_INDIRECT (FIRST_SINGLY_INDIRECT + OBJ_PTES_PER_TABLE)
#define FIRST_TRIPLY_INDIRECT \
	(FIRST_DOUBLY_INDIRECT + (OBJ_PTES_PER_TABLE * OBJ_PTES_PER_TABLE))
#define FIRST_QUADLY_INDIRECT \
	(FIRST_TRIPLY_INDIRECT + (OBJ_PTES_PER_TABLE * OBJ_PTES_PER_TABLE * \
	    OBJ_PTES_PER_TABLE))

typedef uintptr_t pgoff_t;

_Static_assert(OBJ_PTES_PER_TABLE == (1 << OBJ_LEVEL_SHIFT),
    "OBJ_PTES_PER_TABLE must be 2^OBJ_LEVEL_SHIFT");

vm_object_t *
vm_obj_new_vnode(vnode_t *vnode)
{
	vm_object_t *obj = kmem_alloc(sizeof(*obj));

	obj->kind = VM_OBJ_VNODE;
	obj->vnode = vnode;
	ke_spinlock_init(&obj->creation_lock);
	ke_spinlock_init(&obj->stealing_lock);
	for (size_t i = 0; i < OBJ_N_DIRECT; i++)
		obj->direct[i].value = 0;
	for (size_t i = 0; i < OBJ_LEVELS; i++)
		obj->indirect[i].value = 0;

	return obj;
}

/*
 * levelp set to 0 for indirect, 1 for doubly indirect, etc.
 */
void
obj_pte_indexes(size_t offset, size_t indexes[OBJ_LEVELS], int *levelp)
{
	size_t pn = offset >> PGSHIFT;
	size_t pn_indirect = pn - OBJ_N_DIRECT;
	size_t entry_count = OBJ_PTES_PER_TABLE;
	int level = 0;

	kassert(pn >= OBJ_N_DIRECT);

	while (pn_indirect >= entry_count && level < 3) {
		pn_indirect -= entry_count;
		entry_count <<= OBJ_LEVEL_SHIFT;
		level++;
	}

	*levelp = level;

	for (int i = 0; i < 4; i++)
		indexes[i] = 0;

	for (size_t i = 0; i <= level; i++) {
		indexes[i] = pn_indirect & (OBJ_PTES_PER_TABLE - 1);
		pn_indirect >>= OBJ_LEVEL_SHIFT;
	}
}

int
obj_wire_pte(vm_object_t *obj, struct obj_pte_wire_state *state, vaddr_t offset,
    bool create, struct table_lock_state *table_lock_state)
{
	size_t indexes[OBJ_LEVELS + 1];
	int indirection_level;
	pte_t *table;
	vm_page_t *table_page;

	kassert(ke_ipl() == IPL_DISP);
	kassert(ke_spinlock_held(&obj->creation_lock));
	kassert(ke_spinlock_held(&obj->stealing_lock));

	kassert(offset % PGSIZE == 0);

	memset(state, 0, sizeof(*state));

	state->offset = offset;

	if ((offset >> PGSHIFT) < OBJ_N_DIRECT) {
		state->pte = &obj->direct[offset >> PGSHIFT];
		return 0;
	}

	obj_pte_indexes(offset, indexes, &indirection_level);

	/* fake up these so we can loop */
	table = &obj->indirect[indirection_level];
	table_page = NULL;
	indexes[indirection_level + 1] = 0;

	for (size_t level = indirection_level + 1;; level--) {
		pte_t *ppte = &table[indexes[level]], pte = pmap_load_pte(ppte);

		if (level == 0) {
			state->pte = ppte;
			return 0;
		}

		switch (pmap_pte_characterise(pte)) {
		case kPTEKindZero: {
			vm_page_t *page;

			if (!create)
				return -level;

			ke_spinlock_exit_nospl(&obj->stealing_lock);

			page = vm_page_alloc(VM_PAGE_OBJ_TABLE, 0,
			    VM_DOMID_LOCAL, 0);
			if (page == NULL)
				kfatal("TODO: Wait on pages avail event.\n");

			memset((void *)vm_page_hhdm_addr(page), 0, PGSIZE);

			ke_spinlock_enter_nospl(&obj->stealing_lock);

			page->pte = ppte;
			page->owner_obj = obj;
			page->objtable.level = level - 1;
			page->objtable.is_root = level == (indirection_level + 1);

			/* refcnt is already 1; add 1 to these to pin. */
			page->objtable.noswap_ptes++;
			page->objtable.nonzero_ptes++;

			if (table_page != NULL) {
				table_page->objtable.noswap_ptes++;
				table_page->objtable.nonzero_ptes++;
			}

			pmap_pte_hwleaf_create(ppte, VM_PAGE_PFN(page),
			    PMAP_L0, 0, level);

			state->pages[level - 1] = page;

			table = (pte_t *)vm_page_hhdm_addr(page);
			table_page = page;

			break;
		}

		case kPTEKindHW: {
			vm_page_t *page = pmap_pte_hwleaf_page(pte, PMAP_L0);

			/* pin the next level */
			page->objtable.noswap_ptes++;
			page->objtable.nonzero_ptes++;

			state->pages[level - 1] = page;
			table = (pte_t *)vm_page_hhdm_addr(page);
			table_page = page;

			break;
		}

		default:
			kfatal("Implement me\n");
		}
	}

	kfatal("Unreachable\n");
}

void
obj_unwire_pte(vm_object_t *obj, struct obj_pte_wire_state *state)
{
	size_t pn = state->offset >> PGSHIFT;

	kassert(ke_ipl() == IPL_DISP);
	kassert(ke_spinlock_held(&obj->creation_lock));
	kassert(ke_spinlock_held(&obj->stealing_lock));

	/* direct PTEs have no table pages to unpin */
	if (pn < OBJ_N_DIRECT)
		return;

	/*
	 * indirection_level = 0 => singly indirect,
	 * indirection_level = 1 => doubly indirect,
	 * etc
	 */
	size_t indexes[OBJ_LEVELS];
	int indirection_level;

	obj_pte_indexes(state->offset, indexes, &indirection_level);

	for (size_t i = 0; i < indirection_level + 1; i++) {
		vm_page_t *page = state->pages[i];
		vm_page_t *dir_page = NULL;
		pte_t *pte = page->pte;

		if (i < indirection_level)
			dir_page = state->pages[i + 1];

		if (--page->objtable.nonzero_ptes != 0) {
			/* not the last nonzero PTE, but maybe  last noswap. */
			kassert_dbg(page->objtable.noswap_ptes >= 1);
			if (--page->objtable.noswap_ptes == 0) {
#if 0
				/* last noswap reference; make it trans. */
				pmap_pte_hwdir_create_soft(pte, kPTEKindTrans,
				    VM_PAGE_PFN(page), true);
				/* todo(low): page table dirtiness tracking? */
				vm_page_release_and_dirty(page, true);
#else
				kfatal("implement me: last noswap");
#endif
			}
		} else {
			/* last nonzero PTE: free the table */

			kassert_dbg(page->objtable.noswap_ptes == 1);
			kassert_dbg(page->ref_count >= 1);

			page->objtable.noswap_ptes = 0;

			if (dir_page != NULL) {
				/* fine: we hold a pin on the next level */
				dir_page->objtable.nonzero_ptes--;
				dir_page->objtable.noswap_ptes--;
			}

			pmap_pte_zeroleaf_create(pte, PMAP_L0);
			vm_page_delete(page, true);
		}
	}
}

pte_t *
obj_fetch_pte(vm_object_t *obj, vaddr_t offset)
{
	size_t indexes[OBJ_LEVELS + 1];
	int indirection_level;
	pte_t *table;

	kassert(ke_spinlock_held(&obj->stealing_lock));
	kassert(ke_ipl() == IPL_DISP);
	kassert(offset % PGSIZE == 0);

	if ((offset >> PGSHIFT) < OBJ_N_DIRECT)
		return &obj->direct[offset >> PGSHIFT];

	obj_pte_indexes(offset, indexes, &indirection_level);

	table = &obj->indirect[indirection_level];
	indexes[indirection_level + 1] = 0;

	for (size_t level = indirection_level + 1;; level--) {
		pte_t *ppte = &table[indexes[level]], pte;

		if (level == 0)
			return ppte;

		pte = pmap_load_pte(ppte);

		if (pmap_pte_characterise(pte) != kPTEKindHW)
			return NULL;

		table = (pte_t *)p2v(pmap_pte_hwleaf_paddr(pte, PMAP_L0));
	}
}

void
obj_new_ptes_created(struct obj_pte_wire_state *cursor, size_t n)
{
	if ((cursor->offset >> PGSHIFT) >= OBJ_N_DIRECT) {
		cursor->pages[0]->objtable.nonzero_ptes += n;
		cursor->pages[0]->objtable.noswap_ptes += n;
	}
}

static void
obj_valid_ptes_zeroed_table(vm_object_t *obj, vm_page_t *table_page, size_t n)
{
	vm_page_t *dir_page;

	kassert(ke_ipl() == IPL_DISP);
	kassert(ke_spinlock_held(&obj->stealing_lock));
	kassert(table_page->owner_obj == obj);
	kassert(table_page->use == VM_PAGE_OBJ_TABLE);

	kassert(table_page->objtable.nonzero_ptes >= (int)n);
	table_page->objtable.nonzero_ptes -= (int)n;

	if (table_page->objtable.nonzero_ptes == 0) {
		/*
		 * This is the last non-zero PTE in this object page table.
		 * We zero the directory PTE that points to this table and free
		 * the table page itself.
		 */

		bool is_root = table_page->objtable.is_root;
		pte_t *dir_pte = table_page->pte;

		if (!is_root) {
			dir_page = VM_PAGE_FOR_HHDM_ADDR((vaddr_t)dir_pte);
			kassert(dir_page->owner_obj == obj);
			kassert(dir_page->use == VM_PAGE_OBJ_TABLE);
		}

		pmap_pte_zeroleaf_create(dir_pte, PMAP_L0);
		vm_page_delete(table_page, true);

		/*
		 * If this was not a root table, recurse and drop the directory
		 * PTE's non-zero count too.
		 */
		if (!is_root)
			obj_valid_ptes_zeroed_table(obj, dir_page, 1);
	} else {
		/*
		 * At least one non-zero PTE remains in this table.
		 * So we decrement the noswap count. If that reaches zero, and
		 * this isn't a root table, the table is made swappable by
		 * making transitional the directory PTE that points to this
		 * table.
		 */
		kassert(table_page->objtable.noswap_ptes >= (int)n);
		table_page->objtable.noswap_ptes -= (int)n;

		if (table_page->objtable.noswap_ptes == 0 &&
		    !table_page->objtable.is_root) {
#if 0
			pmap_pte_hwdir_create_soft(table_page->pte,
			    kPTEKindTrans, VM_PAGE_PFN(table_page), true);
			vm_page_release_and_dirty(table_page, true);
#else
			kfatal("todo make trans obj pde");
#endif
		}
	}
}

void
obj_page_zeroed(vm_object_t *obj, vm_page_t *page)
{
	pgoff_t pgoff;

	kassert(ke_ipl() == IPL_DISP);
	kassert(ke_spinlock_held(&obj->stealing_lock));
	kassert(page->owner_obj == obj);
	kassert(page->use == VM_PAGE_FILE || page->use == VM_PAGE_ANON_SHARED);

	pgoff = page->shared.offset;

	/* If it is a direct PTE, nothing more needs to be done. */
	if (pgoff < OBJ_N_DIRECT)
		return;

	pte_t *leaf_pte = page->pte;
	vm_page_t *leaf_table = VM_PAGE_FOR_HHDM_ADDR((vaddr_t)leaf_pte);

	kassert(leaf_table->owner_obj == obj);
	kassert(leaf_table->use == VM_PAGE_OBJ_TABLE);

	/* One valid leaf PTE became zero in the table. */
	obj_valid_ptes_zeroed_table(obj, leaf_table, 1);
}

void
obj_page_swapped(vm_object_t *obj, vm_page_t *page)
{
	pgoff_t pgoff;

	kassert(ke_ipl() == IPL_DISP);
	kassert(ke_spinlock_held(&obj->stealing_lock));
	kassert(page->owner_obj == obj);
	kassert(page->use == VM_PAGE_ANON_SHARED);

	pgoff = page->shared.offset;

	/* If it is a direct PTE, nothing more needs to be done. */
	if (pgoff < OBJ_N_DIRECT)
		return;

	vm_page_t *leaf_table = VM_PAGE_FOR_HHDM_ADDR((vaddr_t)page->pte);

	kassert(leaf_table->owner_obj == obj);
	kassert(leaf_table->use == VM_PAGE_OBJ_TABLE);

	/*
	 * One valid PTE became a swap PTE. The table that maps it might be
	 * eligible for unpinning now.
	 */
	kassert(leaf_table->objtable.noswap_ptes > 0);
	leaf_table->objtable.noswap_ptes--;

	if (leaf_table->objtable.noswap_ptes == 0 &&
	    !leaf_table->objtable.is_root) {
#if 0
		pmap_pte_hwdir_create_soft(leaf_table->pte, kPTEKindTrans,
		    VM_PAGE_PFN(leaf_table), true);
		/* todo(low): page table dirtiness tracking? */
		vm_page_release_and_dirty(leaf_table, true);
#else
		kfatal("todo make trans obj pde");
#endif
	}
}

/*!
 * One PTE in the object table \p table_page changed from a "noswap" kind (valid
 * or trans) to swap.
 */
void
obj_table_pte_did_become_swap(vm_object_t *obj, vm_page_t *table_page)
{
	kassert(ke_spinlock_held(&obj->stealing_lock));
	kassert(table_page->owner_obj == obj);
	kassert(table_page->use == VM_PAGE_OBJ_TABLE);

	kassert(table_page->objtable.noswap_ptes > 0);
	table_page->objtable.noswap_ptes--;

	if (table_page->objtable.noswap_ptes == 0 &&
	    !table_page->objtable.is_root) {
#if 0
		/*
		 * No more noswap PTEs in this table. Its own directory PTE
		 * can now be made a Trans PTE, and the table page can move
		 * to standby via vm_page_release.
		 */
		pmap_pte_hwdir_create_soft(table_page->pte, kPTEKindTrans,
		    VM_PAGE_PFN(table_page), true);
		/* todo(low): page table dirtiness tracking? */
		vm_page_release_and_dirty(table_page, true);
#else
		kfatal("todo make trans obj pde");
#endif
	}
}

size_t
obj_max_readahead(struct obj_pte_wire_state *cursor, pgoff_t pgoff,
    size_t max_pages)
{
	size_t obj_zero_n = 1;

#if 0
	kprintf("Max readahead from pgoff %zu max_pages %zu\n", pgoff,
	    max_pages);
#endif

	if (pgoff < OBJ_N_DIRECT)
		max_pages = MIN2(max_pages, OBJ_N_DIRECT - pgoff);

	if (max_pages == 1)
		return 1;

	for (size_t i = 1; i < max_pages; i++) {
		pte_t *obj_ppte = cursor->pte + i, pte;

		/* stop if we cross a page boundary */
		if (pgoff < OBJ_N_DIRECT &&
		    ((uintptr_t)(obj_ppte) & (PGSIZE - 1)) == 0)
			break;

		pte = pmap_load_pte(obj_ppte);
		if (pmap_pte_characterise(pte) != kPTEKindZero)
			break;

		obj_zero_n++;
	}

	return obj_zero_n;
}

void
vm_obj_purge(vm_object_t *obj)
{
}
