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
typedef struct vm_section    vm_section_t;

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
	kPageUseInvalid,
	kPageUsePFNDB,
	kPageUseFree,
	kPageUseDeleted,
	kPageUseKWired,
	kPageUseAnonPrivate,
	kPageUseFileShared,
	/*! root pagetable */
	kPageUsePML4,
	/* upper middle pagetable */
	kPageUsePML3,
	/*! lower middle pagetable */
	kPageUsePML2,
	/*! leaf pagetable */
	kPageUsePML1,
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

	/* second word */
	union __attribute__((packed)) {
		/* pagetable pages */
		struct __attribute__((packed)) {
			uint16_t used_ptes;
			uint16_t valid_ptes;
		};
		/* file/aonymous pages: offset into section */
		uint64_t offset : 48;
	};
	uint16_t refcnt;

	/* third word */
	paddr_t referent_pte;

	/* 4th, 5th words */
	union __attribute__((packed)) {
		TAILQ_ENTRY(vm_page)	  queue_link;
		struct vmp_pager_state *pager_request;
	};

	/* 6th word */
	void *owner;

	/* 7th word */
	uintptr_t swap_descriptor;
} vm_page_t;

typedef struct vm_account {
	size_t nalloced;
	size_t nwires;
} vm_account_t;

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
	vm_page_t *pages[0];
} vm_mdl_t;

extern struct vm_stat vmstat;

/*! @brief Allocate an MDL complete with pages. */
vm_mdl_t *vm_mdl_buffer_alloc(size_t npages);

/*! @brief Allocate an MDL complete with pages. */
vm_mdl_t *vm_mdl_alloc_with_pages(size_t npages, enum vm_page_use use,
    vm_account_t *account, bool pfnlock_held);

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
 * @param account An optional account to debit for the pages. If an account is
 * specified, then vm_page_delete should be called when the allocator has no
 * further need of the page; this will credit the account for its page
 * allocation. The account is also charged for a wiring.
 *
 * @pre PFNDB lock must not be held.
 */
int vm_page_alloc(vm_page_t **out, size_t order, vm_account_t *account,
    enum vm_page_use use, bool must);

/*!
 * @brief Release and mark a page for deletion.
 *
 * This changes the page's use to kPageUseDeleted and then calls
 * vm_page_release() on the page if its refcount is greater than 0. Otherwise,
 * it immediately frees the page.
 *
 * @param account Account to credit for deallocating a page.
 * @param release Whether account holds a reference which is being released. If
 * so, its wire quota is also credited.
 *
 * @pre PFNDB lock must not be held.
 */
void vm_page_delete(vm_page_t *page, vm_account_t *account, bool release);

/*!
 * @brief Retain a reference to a page.
 *
 * This preserves the page from being freed by wiring it in-memory. It will
 * remain possible to access it until vm_page_release() is called. Access must
 * be via a private mapping or the direct map; any other mappings of the page
 * are not necessarily preserved.
 *
 * @param account An optional account to debit for wiring the page.
 *
 * @pre PFNDB lock must not be held.
 */
vm_page_t *vm_page_retain(vm_page_t *page, vm_account_t *account);

/*!
 * @brief Release a reference to a page.
 *
 * @param account An optional account to credit for unwiring the page.
 *
 * @pre PFNDB lock must not be held.
 */
void vm_page_release(vm_page_t *page, vm_account_t *account);

/*!
 * @brief Get the page frame structure for a given physical address.
 */
vm_page_t *vm_paddr_to_page(paddr_t paddr);

/*! Allocate anonymous memory in a process. */
int vm_ps_allocate(vm_procstate_t *vmps, vaddr_t *vaddrp, size_t size,
    bool exact);

/*! Map a section view into a process. */
int vm_ps_map_section_view(vm_procstate_t *vmps, void *section,
    vaddr_t *vaddrp, size_t size, uint64_t offset,
    vm_protection_t initial_protection, vm_protection_t max_protection,
    bool inherit_shared, bool cow, bool exact);

/*! Dump the VAD tree of a process.*/
int vm_ps_dump_vadtree(vm_procstate_t *vmps);

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

extern struct vm_stat vmstat;
extern vm_account_t   general_account, deleted_account;

/*! @brief Allocated kernel wired pages and address space. */
vaddr_t vm_kalloc(size_t npages, enum vmem_flag flags);

/*! @brief Free kernel wired pages. */
void vm_kfree(vaddr_t addr, size_t npages, enum vmem_flag flags);


#endif /* KRX_KDK_VM_H */
