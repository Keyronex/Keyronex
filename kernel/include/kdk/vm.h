#ifndef KRX_KDK_VM_H
#define KRX_KDK_VM_H

#include <stdint.h>

#include "kdk/nanokern.h"
#include "kdk/port.h"
#include "kdk/queue.h"
#include "kdk/tree.h"
#include "kdk/vmem.h"
#include "kdk/vmem_impl.h"
#include "stdbool.h"

#if BITS == 32
#define PFN_BITS 20
#else
#define PFN_BITS 52
#endif

enum vmem_flag;
struct eprocess;
struct vnode;

/*! Physical address. */
typedef uintptr_t paddr_t;
/*! Virtual address. */
typedef uintptr_t vaddr_t;
/*! Virtual offset */
typedef int64_t pgoff_t, voff_t;
/*! Page frame number. */
typedef uintptr_t pfn_t;
/*! Process VM state. */
typedef struct vm_procstate vm_procstate_t;
/*! Mappable object. */
typedef struct vm_object vm_object_t;

/*!
 * Protection flags.
 */
typedef enum vm_protection {
	kVMRead = 0x1,
	kVMWrite = 0x2,
	kVMExecute = 0x4,
	kVMAll = kVMRead | kVMWrite | kVMExecute,
} vm_protection_t;

/*!
 * Global VM statistics.
 */
struct vm_stat {
	/*! memory by state */
	size_t npwired, nactive, nfree, nmodified, nstandby;

	/*! total pages */
	size_t ntotal;

	/*! free-reservation, may be below 0 */
	intptr_t nreservedfree;

	/*! memory by use; nfree still counts free. */
	size_t ndeleted, nanonprivate, nanonfork, nfileshared, nanonshare,
	    nprocpgtable, nprotopgtable, nkwired;
};

enum vm_page_use {
	kPageUseInvalid = 0,

	kPageUsePFNDB,
	kPageUseFree,
	kPageUseDeleted,
	kPageUseKWired,
	kPageUseAnonPrivate,
	kPageUseFileShared,

	/*! Page is a pagetable (leaf). */
	kPageUsePML1,
	/*! Page is a pagetable (3rd-closest to root). */
	kPageUsePML2,
	/* Page is a pagetable (2nd-closest to root). */
	kPageUsePML3,
	/*! Page is a pagetable (closest to root). */
	kPageUsePML4,

	/*! Page is a synthetic pagetable (leaf). */
	kPageUseVPML1,
	/*! Page is a synthetic pagetable (3rd-closest to root). */
	kPageUseVPML2,
	/* Page is a synthetic pagetable (2nd-closest to root). */
	kPageUseVPML3,
	/*! Page is a synthetic pagetable (closest to root). */
	kPageUseVPML4,
};

/*!
 * PFN database element. Mainly for private use by the VMM, but published here
 * publicly for efficiency.
 */
typedef struct vm_page {
	/* first word */
	struct __attribute__((packed)) {
		uintptr_t pfn : PFN_BITS;
		enum vm_page_use use : 4;
		bool dirty : 1;
		bool busy : 1;
		uintptr_t order : 5;
		bool on_freelist : 1;
	};

	/* 32-bit: word 2-3; 64-bit: word 2 */
	union __attribute__((packed)) {
		/* pagetable pages */
		struct __attribute__((packed)) {
			/*! Reasons to keep the page existent at all. */
			uint16_t nonzero_ptes;
			/*! Reasons to keep the page in-core. */
			uint16_t noswap_ptes;
		};
		/* file/aonymous pages: offset into object */
		uint64_t offset : 48;
	};
	uint16_t refcnt;

	/* 32-bit: word 4; 64-bit: word 3 */
	paddr_t referent_pte;

	/* 32-bit: words 5-6, 64-bit: words 4-5 */
	union __attribute__((packed)) {
		TAILQ_ENTRY(vm_page)	  queue_link;
		struct vmp_pager_state *pager_request;
	};

	/* 32-bit: word 7, 64-bit: word 6 */
	union {
		void *owner;
		struct eprocess *process;
	};

	/* 32-bit: word 8; 64-bit: word 7 */
	uintptr_t drumslot;
} vm_page_t;

typedef struct vm_mdl_view_entry {
	paddr_t paddr;
	size_t bytes;
} vm_mdl_entry_t;

/*!
 * Memory descriptor list.
 */
typedef struct vm_mdl {
	size_t nentries;
	size_t offset;
	bool write;
	vm_page_t *pages[0];
} vm_mdl_t;

#define STATIC_MDL(NPAGES) struct {	\
	vm_mdl_t mdl;			\
	vm_page_t *static_pages[NPAGES];\
}

enum vmp_pte_kind {
	kPTEKindZero,
	kPTEKindTrans,
	kPTEKindSwap,
	kPTEKindBusy,
	kPTEKindValid,
};

/*! @brief Allocated kernel wired pages and address space. */
vaddr_t vm_kalloc(size_t npages, enum vmem_flag flags);

/*! @brief Free kernel wired pages. */
void vm_kfree(vaddr_t addr, size_t npages, enum vmem_flag flags);

/*! @brief Allocate an MDL complete with pages. */
vm_mdl_t *vm_mdl_buffer_alloc(size_t npages);

/*! @brief Allocate an MDL complete with pages. */
vm_mdl_t *vm_mdl_alloc_with_pages(size_t npages, enum vm_page_use use,
    bool pfnlock_held);

/*! @brief Create an MDL from the given address. */
vm_mdl_t *vm_mdl_create(void *address, size_t size);

/*! @brief Get the physical address of an offset in an MDL. */
paddr_t vm_mdl_paddr(vm_mdl_t *mdl, voff_t off);

/*!
 * @brief Add a region of memory to the VMM's management.
 */
void vm_region_add(paddr_t base, size_t length);

/*!
 * @brief Allocate physical page frames.
 *
 * @pre PFNDB lock must not be held.
 */
int vm_page_alloc(vm_page_t **out, size_t order, enum vm_page_use use,
    bool must);

/*!
 * @brief Mark a page for deletion.
 *
 * This changes the page's use to kPageUseDeleted. If the page has a refcount of
 * 0, it is immediately put onto the freelist; otherwise, it awaits its final
 * refcount drop to 0 before this happens.
 *
 * @pre PFNDB lock must not be held.
 */
void vm_page_delete(vm_page_t *page);

/*!
 * @brief Retain a reference to a page.
 *
 * This preserves the page from being freed by wiring it in-memory. It will
 * remain possible to access it until vm_page_release() is called. Access must
 * be via a private mapping or the direct map; any other mappings of the page
 * are not necessarily preserved.
 *
 * @pre PFNDB lock must not be held.
 */
vm_page_t *vm_page_retain(vm_page_t *page);

/*!
 * @brief Release a reference to a page.
 *
 * @pre PFNDB lock must not be held.
 */
void vm_page_release(vm_page_t *page);

/*!
 * @brief Get the page frame structure for a given physical address.
 */
vm_page_t *vm_paddr_to_page(paddr_t paddr);

/*! Initialise a new VM process state structure */
void vm_ps_init(struct eprocess *ps);

/*! Allocate anonymous memory in a process. */
int vm_ps_allocate(vm_procstate_t *vmps, vaddr_t *vaddrp, size_t size,
    bool exact);

/*! Map an object view into a process. */
int vm_ps_map_object_view(vm_procstate_t *vmps, vm_object_t *object,
    vaddr_t *vaddrp, size_t size, uint64_t offset,
    vm_protection_t initial_protection, vm_protection_t max_protection,
    bool inherit_shared, bool cow, bool exact);

/*! Deallocate part of address space. */
int vm_ps_deallocate(vm_procstate_t *vmps, vaddr_t start, size_t size);

/*! Dump the VAD tree of a process.*/
int vm_ps_dump_vadtree(vm_procstate_t *vmps);

/*! Set up a new pagefile. */
int vm_pagefile_add(struct vnode *vnode);

extern struct vm_stat vmstat;

/*! Test whether there are plenty of free pages. */
static inline bool
vm_plentiful_pages(void)
{
	return vmstat.nreservedfree >= 256;
}

/*! Test whether there are enough free pages to be worth allocate. */
static inline bool
vm_adequate_pages(void)
{
	return vmstat.nreservedfree >= 64;
}

static inline uint32_t
log2(uint32_t val)
{
	if (val == 0)
		return 0;
	return 32 - __builtin_clz(val - 1);
}

static inline size_t
vm_npages_to_order(size_t npages)
{
	size_t order = log2(npages);
	if ((1 << order) < npages)
		order++;
	return order;
}

static inline size_t
vm_order_to_npages(size_t order)
{
	return 1 << order;
}

static inline paddr_t
vm_page_paddr(vm_page_t *page)
{
	return PFN_TO_PADDR(page->pfn);
}

static inline vaddr_t
vm_page_direct_map_addr(vm_page_t *page)
{
	return P2V(vm_page_paddr(page));
}

#define vm_pfn_to_page(PFN_) (vm_paddr_to_page(vmp_pfn_to_paddr(PFN_)))

#endif /* KRX_KDK_VM_H */
