#include <amd64/amd64.h>
#include <kern/kmem.h>
#include <libkern/libkern.h>
#include <md/spl.h>
#include <nanokern/thread.h>
#include <vm/vm.h>

enum {
	kPML4Shift = 0x39,
	kPDPTShift = 0x30,
	kPDIShift = 0x21,
	kPTShift = 0x12,
};

enum {
	kMMUPresent = 0x1,
	kMMUWrite = 0x2,
	kMMUUser = 0x4,
	kMMUWriteThrough = 0x8,
	kMMUCacheDisable = 0x10,
	kMMUAccessed = 0x20, /* bit 5*/
	kMMUDirty = 0x40,    /* bit 6 */
	kPageGlobal = 0x100,

	kMMUDefaultProt = kMMUPresent | kMMUWrite | kMMUUser,

	kMMUFrame = 0x000FFFFFFFFFF000
};

struct pmap {
	paddr_t pml4;
};

typedef uint64_t pml4e_t, pdpte_t, pde_t, pte_t;

static uint64_t *pte_get_addr(uint64_t pte);
static void	 pte_set(uint64_t *pte, paddr_t addr, uint64_t flags);

vm_map_t      kmap;
static pmap_t kpmap;

void
x64_vm_init(paddr_t kphys)
{
	nk_mutex_init(&kmap.lock);
	TAILQ_INIT(&kmap.entries);
	kmap.pmap = &kpmap;
	kpmap.pml4 = (paddr_t)read_cr3();
	/* pre-allocate the top 256. they are globally shared. */
	for (int i = 255; i < 511; i++) {
		uint64_t *pml4 = P2V(kpmap.pml4);
		if (pte_get_addr(pml4[i]) == NULL) {
			vm_page_t *page = vm_pagealloc(true, &vm_pgpmapq);
			pte_set(&pml4[i], page->paddr, kMMUDefaultProt);
		}
	}
}

/*! Free a level of page tables. \p table is a physical address. */
void
pmap_free_sub(uint64_t *table, int level)
{
	vm_page_t *page;

	if (table == NULL)
		return;

	table = P2V(table);

	/*
	 * we don't free the individual mappings (there shouldn't *be*
	 * any left, as they should've been removed by vm_deallocate).
	 * Only the page tables themselves are freed.
	 */
	if (level > 1)
		for (int i = 0; i < 512; i++) {
			pte_t *entry = &table[i];
			pmap_free_sub(pte_get_addr(*entry), level - 1);
		}

	page = vm_page_from_paddr((uintptr_t)V2P(table));
	vm_page_free(page);
}

void
pmap_free(pmap_t *pmap)
{
	uint64_t *vpml4 = P2V(pmap->pml4);
	for (int i = 0; i < 255; i++) {
		pte_t *entry = &vpml4[i];
		pmap_free_sub(pte_get_addr(*entry), 3);
	}
}

/* get the flags of a pte */
uint64_t *
pte_get_flags(uint64_t pte)
{
	return (uint64_t *)(pte & ~kMMUFrame);
}

/* get the physical address to which a pte points */
static uint64_t *
pte_get_addr(uint64_t pte)
{
	return (uint64_t *)(pte & kMMUFrame);
}

/* reset a pte to a given addy and flags. pte must be a virt addr. */
static void
pte_set(uint64_t *pte, paddr_t addr, uint64_t flags)
{
	uintptr_t a = (uintptr_t)addr;
	a &= kMMUFrame;
	*pte = 0x0;
	*pte = a | flags;
}

void
vm_activate(vm_map_t *map)
{
	uint64_t val = (uint64_t)map->pmap->pml4;
	write_cr3(val);
}

static uint64_t
vm_prot_to_i386(vm_prot_t prot)
{
	return (prot & kVMRead ? kMMUPresent : 0) |
	    (prot & kVMWrite ? kMMUWrite : 0) | kMMUUser;
}

/*!
 * Get the physical address at which a page table entry points. optionally
 * allocate a new entry, setting appropriate flags. Table should be a pointer
 * to the physical location of the table.
 */
uint64_t *
pmap_descend(uint64_t *table, size_t index, bool alloc, uint64_t mmuprot)
{
	uint64_t *entry = P2V((&table[index]));
	uint64_t *addr = NULL;

	if (*entry & kMMUPresent) {
		addr = pte_get_addr(*entry);
	} else if (alloc) {
		vm_page_t *page = vm_pagealloc(true, &vm_pgpmapq);
		if (!page)
			kfatal("out of pages");
		addr = (uint64_t *)page->paddr;
		pte_set(entry, (paddr_t)addr, mmuprot);
	}

	return addr;
}

/*!
 * @returns pointer to the pte for this virtual address, or NULL if none exists
 */
pte_t *
pmap_fully_descend(pmap_t *pmap, vaddr_t virt)
{
	uintptr_t virta = (uintptr_t)virt;
	pml4e_t	 *pml4 = (void *)pmap->pml4;
	int	  pml4i = ((virta >> 39) & 0x1FF);
	int	  pdpti = ((virta >> 30) & 0x1FF);
	int	  pdi = ((virta >> 21) & 0x1FF);
	int	  pti = ((virta >> 12) & 0x1FF);
	pdpte_t	 *pdptes;
	pde_t	 *pdes;
	pte_t	 *ptes;

	pdptes = pmap_descend(pml4, pml4i, false, 0);
	if (!pdptes) {
		return 0x0;
	}

	pdes = pmap_descend(pdptes, pdpti, false, 0);
	if (!pdes) {
		return 0x0;
	}

	ptes = pmap_descend(pdes, pdi, false, 0);
	if (!ptes) {
		return 0x0;
	}

	return &ptes[pti];
}

paddr_t
pmap_trans(pmap_t *pmap, vaddr_t virt)
{
	uintptr_t virta = (uintptr_t)virt;
	pml4e_t	 *pml4 = (void *)pmap->pml4;
	int	  pml4i = ((virta >> 39) & 0x1FF);
	int	  pdpti = ((virta >> 30) & 0x1FF);
	int	  pdi = ((virta >> 21) & 0x1FF);
	int	  pti = ((virta >> 12) & 0x1FF);
	int	  pi = ((virta)&0xFFF);

	pdpte_t *pdpte;
	pde_t	*pde;
	pte_t	*pte;

	pdpte = pmap_descend(pml4, pml4i, false, 0);
	if (!pdpte) {
		// kprintf("no pml4 entry\n");
		return 0x0;
	}

	pde = pmap_descend(pdpte, pdpti, false, 0);
	if (!pde) {
		// kprintf("no pdpt entry\n");
		return 0x0;
	}

	pte = pmap_descend(pde, pdi, false, 0);
	if (!pte) {
		// kprintf("no pte entry\n");
		return 0x0;
	}

	pte = P2V(pte);

	if (!(pte[pti] & kMMUPresent))
		return 0x0;
	else
		return (paddr_t)pte_get_addr(pte[pti]) + pi;
}

void
pmap_enter(vm_map_t *map, vm_page_t *page, vaddr_t virt, vm_prot_t prot)
{
	pv_entry_t *ent = kmem_alloc(sizeof(*ent));

	ent->map = map;
	ent->vaddr = virt;

	pmap_enter_kern(map->pmap, page->paddr, virt, prot);
	LIST_INSERT_HEAD(&page->pv_table, ent, pv_entries);
}

void
pmap_enter_kern(pmap_t *pmap, paddr_t phys, vaddr_t virt, vm_prot_t prot)
{
	uintptr_t virta = (uintptr_t)virt;
	int	  pml4i = ((virta >> 39) & 0x1FF);
	int	  pdpti = ((virta >> 30) & 0x1FF);
	int	  pdi = ((virta >> 21) & 0x1FF);
	int	  pti = ((virta >> 12) & 0x1FF);
	pml4e_t	 *pml4 = (void *)pmap->pml4;
	pdpte_t	 *pdpte;
	pde_t	 *pde;
	pte_t	 *pte;
	pte_t	 *pti_virt;

	pdpte = pmap_descend(pml4, pml4i, true, kMMUDefaultProt);
	pde = pmap_descend(pdpte, pdpti, true, kMMUDefaultProt);
	pte = pmap_descend(pde, pdi, true, kMMUDefaultProt);

	pti_virt = P2V(&pte[pti]);

	if (pte_get_addr(*pti_virt) != NULL)
		/* TODO(med): do we care about this case? */
		;

	pte_set(pti_virt, phys, vm_prot_to_i386(prot));
}

void
pmap_reenter(vm_map_t *map, vm_page_t *page, vaddr_t virt, vm_prot_t prot)
{
	pmap_enter_kern(map->pmap, page->paddr, virt, prot);
}

void
pmap_reenter_all_readonly(vm_page_t *page)
{
	pv_entry_t *pv, *tmp;

	LIST_FOREACH_SAFE (pv, &page->pv_table, pv_entries, tmp) {
		pmap_reenter(pv->map, page, pv->vaddr, kVMRead | kVMExecute);
		pmap_global_invlpg(pv->vaddr);
	}
}

void
pmap_unenter(vm_map_t *map, vm_page_t *page, vaddr_t vaddr, pv_entry_t *pv)
{
	/** \todo free no-longer-needed page tables */
	pte_t  *pte = pmap_fully_descend(map->pmap, vaddr);
	paddr_t paddr;

	/*
	 * todo(med): maybe we should be more careful about this
	 * perhaps instead of bulk pmap_unenter'ing on deallocation, instead
	 * we should iterate through the object's page queue (amap for anon
	 * objects). would it add significant cost?
	 */
	if (pte == NULL)
		return;

	pte = P2V(pte);
	paddr = (paddr_t)pte_get_addr(*pte);
	if (*pte == 0)
		return;
	*pte = 0x0;

	pmap_invlpg(vaddr);

	if (!page) {
		page = vm_page_from_paddr(paddr);
	}

	kassert(page);

	if (!pv) {
		LIST_FOREACH (pv, &page->pv_table, pv_entries) {
			if (pv->map == map && pv->vaddr == vaddr) {
				goto next;
			}
		}

		kfatal(
		    "pmap_unenter: no mapping of frame 0x%lx at vaddr 0x%lx in map %p\n",
		    page->paddr, vaddr, map);
	}

next:
	LIST_REMOVE(pv, pv_entries);
	kmem_free(pv, sizeof(*pv));
}

void
pmap_unenter_all(vm_page_t *page)
{
	pv_entry_t *pv, *tmp;

	LIST_FOREACH_SAFE (pv, &page->pv_table, pv_entries, tmp) {
		pmap_unenter(pv->map, page, pv->vaddr, pv);
		pmap_global_invlpg(pv->vaddr);
	}
}

bool
pmap_page_accessed(struct vm_page *page, bool reset)
{
	pv_entry_t *pv, *tmp;
	bool	    accessed = false;

	LIST_FOREACH_SAFE (pv, &page->pv_table, pv_entries, tmp) {
		pte_t *pte = pmap_fully_descend(pv->map->pmap, pv->vaddr);
		nk_assert(pte != NULL);
		pte = P2V(pte);

		if (!(*pte & kMMUAccessed))
			continue;

		accessed = true;

		if (reset) {
			*pte = *pte & ~kMMUAccessed;
			pmap_global_invlpg(pv->vaddr);
		} else
			return true; /* no need to scan all in this case */
	}

	return accessed;
}

vm_page_t *
pmap_unenter_kern(vm_map_t *map, vaddr_t vaddr)
{
	pte_t	  *pte = pmap_fully_descend(map->pmap, vaddr);
	paddr_t	   paddr;
	vm_page_t *page;

	kassert(pte);
	pte = P2V(pte);
	paddr = (paddr_t)pte_get_addr(*pte);
	kassert(*pte != 0x0);
	*pte = 0x0;

	pmap_invlpg(vaddr);

	page = vm_page_from_paddr(paddr);
	kassert(page);

	return page;
}

pmap_t *
pmap_new()
{
	pmap_t *pmap = kmem_alloc(sizeof(*pmap));
	pmap->pml4 = vm_pagealloc(1, &vm_pgpmapq)->paddr;
	for (int i = 255; i < 512; i++) {
		uint64_t *pml4 = P2V(pmap->pml4);
		uint64_t *kpml4 = P2V(kmap.pmap->pml4);
		pte_set(&pml4[i], kpml4[i], kMMUDefaultProt);
	}
	return pmap;
}

void
pmap_invlpg(vaddr_t addr)
{
	asm volatile("invlpg %0" : : "m"(*((const char *)addr)) : "memory");
}

/* todo(low) probably unnecessary now that pgq lock held for un/mapping */
static kspinlock_t  invlpg_global_lock = KSPINLOCK_INITIALISER;
vaddr_t		    invlpg_addr;
volatile atomic_int invlpg_done_cnt;

void
pmap_global_invlpg(vaddr_t vaddr)
{
	int iff = md_intr_disable();

	nk_spinlock_acquire_nospl(&invlpg_global_lock);
	invlpg_addr = vaddr;
	invlpg_done_cnt = 1;
	for (int i = 0; i < ncpus; i++) {
		if (all_cpus[i] == curcpu())
			continue;

		md_ipi_invlpg(all_cpus[i]);
	}
	pmap_invlpg(vaddr);
	while (invlpg_done_cnt != ncpus)
		__asm__("pause");
	nk_spinlock_release_nospl(&invlpg_global_lock);

	md_intr_x(iff);
}
