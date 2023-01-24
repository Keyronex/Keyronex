#ifndef VM_H_
#define VM_H_

#include <kern/vmem_impl.h>
#include <md/vm.h>

#include <nanokern/kerndefs.h>
#include <nanokern/thread.h>
#include <stddef.h>

#define VADDR_MAX (vaddr_t) UINT64_MAX

#define ROUNDUP(addr, align) ((((uintptr_t)addr) + align - 1) & ~(align - 1))
#define ROUNDDOWN(addr, align) ((((uintptr_t)addr)) & ~(align - 1))

#define PTRROUNDUP(addr) ROUNDUP(addr, (sizeof(uintptr_t)))
/** Round a value up to pointer alignment. */
#define PTRROUNDDOWN(addr) ROUNDDOWN(addr, (sizeof(uintptr_t)))

#define PGROUNDUP(addr) ROUNDUP(addr, PGSIZE)
#define PGROUNDDOWN(addr) ROUNDDOWN(addr, PGSIZE)

/*!
 * Fault flags (matches i386 MMU)
 */
typedef enum vm_fault_flags {
	kVMFaultPresent = 1,
	kVMFaultWrite = 2,
	kVMFaultUser = 4,
	kVMFaultExecute = 16,
} vm_fault_flags_t;

/*!
 * Protection flags for mappings.
 */
typedef enum vm_prot {
	kVMRead = 0x1,
	kVMWrite = 0x2,
	kVMExecute = 0x4,
	kVMAll = kVMRead | kVMWrite | kVMExecute,
} vm_prot_t;

void vm_pagedump(void);

enum vm_page_queue {
	kVMPageFree = 0,
	kVMPageKMem = 1,
	kVMPageWired = 2,
	kVMPageDevBuf = 3,
	kVMPageActive = 4,
	kVMPageInactive = 5,
	kVMPagePMap = 6,
};

#include <nanokern/tree.h>

/*!
 * Represents a physical page of useable general-purpose memory.
 *
 * If neither anon nor obj are set, it may be internally managed (kmem).
 * In the case of tmpfs, both may be set.
 */
typedef struct vm_page {
	/*! Links to its queue: kmem, wired, active, . */
	TAILQ_ENTRY(vm_page) pagequeue;

	/*! Page state. */
	enum vm_page_queue queue : 4;

	/*! for pageable mappings */
	union {
		struct vm_anon	 *anon; /*! if belonging to an anon */
		struct vm_object *obj;	/*! if belonging to non-anon object */
	};

	LIST_HEAD(, pv_entry) pv_table; /*! physical page -> virtual mappings */

	paddr_t paddr; /*! physical address of page */
} vm_page_t;

typedef struct vm_pagequeue {
	TAILQ_HEAD(, vm_page) queue;
	size_t npages;
} vm_pagequeue_t;

/*!
 * Physical region description.
 */
typedef struct vm_pregion {
	/*! Linkage to vm_pregion_queue. */
	TAILQ_ENTRY(vm_pregion) queue;
	/*! Base address of region. */
	paddr_t base;
	/*! Number of pages the region covers. */
	size_t npages;
	/*! Resident page table part for region. */
	vm_page_t pages[0];
} vm_pregion_t;

typedef TAILQ_HEAD(, vm_pregion) vm_pregion_queue_t;

/*! The page queues. */
extern vm_pagequeue_t vm_pgfreeq, vm_pgkmemq, vm_pgwiredq, vm_pgdevbufq, vm_pgactiveq,
    vm_pginactiveq, vm_pgpmapq;

/*! Page region queue. */
extern vm_pregion_queue_t vm_pregion_queue;

/*! Allocate a new page. It is enqueued on the specified queue. */
vm_page_t *vm_pagealloc(bool sleep, vm_pagequeue_t *queue);

/*! Free a page. It is automatically removed from its current queue. */
void vm_page_free(vm_page_t *page);

/*! Get the page corresponding to a particular physical address. */
vm_page_t *vm_page_from_paddr(paddr_t paddr);

/*!
 * Move a page from one queue to another.
 *
 * @param from page queue to move it from. Usually should be NULL (it is
 * determined by this function from the page's flags)
 * @param to page queue to move it to.
 */
void vm_page_changequeue(vm_page_t *page, kx_nullable vm_pagequeue_t *from,
    vm_pagequeue_t *to);

/*! The page queues. */
extern vm_pagequeue_t vm_pgfreeq, vm_pgkmemq, vm_pgwiredq, vm_pgactiveq,
    vm_pginactiveq, vm_pgpmapq;

/*! Page region queue. */
extern vm_pregion_queue_t vm_pregion_queue;

/*!
 * @}
 */

/*!
 * @name Maps
 * @{
 */

/*!
 * Represents an entry within a vm_map_t.
 */
typedef struct vm_map_entry {
	TAILQ_ENTRY(vm_map_entry) queue;
	vaddr_t			  start, end;
	voff_t			  offset;
	struct vm_Object	 *obj;
} vm_map_entry_t;

/*!
 * Virtual address space map - either the global kernel map, or a user
 * process-specific map.
 */
typedef struct vm_map {
	TAILQ_HEAD(, vm_map_entry) entries;
	kmutex_t     lock;
	vmem_t	     vmem;
	struct pmap *pmap;
} vm_map_t;

/*! Activate a given map. */
void vm_activate(vm_map_t *map);
/*!
 * Allocate anonymous memory and map it into the given map.
 *
 * @param[in,out] vaddrp pointer to a vaddr specifying where to map at. If the
 * pointed-to value is VADDR_MAX, then first fit is chosen; in this case, the
 * address at which the object was mapped is written out to this parameter..
 * @param[out] out if non-null, the [non-retained] vm_object_t is written out.
 * @param size size in bytes to allocate.
 *
 * @returns 0 for success.
 */
int vm_allocate(vm_map_t *map, struct vm_object **out, vaddr_t *vaddrp,
    size_t size);
/*!
 * Deallocate address space from a given map. For now only handles deallocation
 * of whole objects; they are released and accordingly
 */
int vm_deallocate(vm_map_t *map, vaddr_t start, size_t size);

/*!
 * Handle a page fault.
 */
int vm_fault(md_intr_frame_t *frame, vm_map_t *map, vaddr_t vaddr,
    vm_fault_flags_t flags);

/**
 * Map a VM object into an address space either at a given virtual address, or
 * (if \p vaddr points to vaddr of VADDR_MAX) pick a suitable place to put it.
 *
 * @param size is the size of area to be mapped in bytes - it must be a multiple
 * of the PAGESIZE.
 * @param obj the object to be mapped. It is retained.
 * @param offset offset into object at which to map (must be PGSIZE aligned)
 * @param[in,out] vaddrp points to a vaddr_t describing the preferred address.
 * If VADDR_MAX, then anywhere is fine. The result is written to its referent.
 * @param copy whether to copy \p obj instead of mapping it shared
 * (copy-on-write optimisation may be employed.)
 */
int vm_map_object(vm_map_t *map, struct vm_object *obj, vaddr_t *vaddrp,
    size_t size, voff_t offset, bool copy);

/*! Global kernel map. */
extern vm_map_t kmap;

/*!
 * @}
 */

/*!
 * @name Kernel memory
 * @{
 */

/*! Flags that may be passed to vm_kalloc(). */
enum vm_kalloc_flags {
	/*! immediately return NULL if no free pages currently */
	kVMKNoSleep = 0,
	/*! infallible; sleepwait for a page if no pages currently available */
	kVMKSleep = 1,
};

/** Set up the kernel memory subsystem. */
void vm_kernel_init();

/*!
 * Allocate pages of kernel heap.
 *
 * @param flags see vm_kalloc_flags
 */
vaddr_t vm_kalloc(size_t npages, enum vm_kalloc_flags flags);

/*!
 * Free pages of kernel heap.
 */
void vm_kfree(vaddr_t addr, size_t pages);

/*! @} */

/*!
 * @name PMap
 */

/*! Port-specific physical map. */
typedef struct pmap pmap_t;

/*!
 * Create a new pmap. It will share the higher half with the kernel pmap kpmap.
 */
pmap_t *pmap_new();

/*!
 * Free a pmap.
 */
void pmap_free(pmap_t *pmap);

/*!
 * Pageable mapping of a virtual address to a page.
 */
void pmap_enter(vm_map_t *map, struct vm_page *page, vaddr_t virt,
    vm_prot_t prot);

/*!
 * Low-level mapping a single physical page to a virtual address. Mappings
 * are not tracked.
 */
void pmap_enter_kern(struct pmap *pmap, paddr_t phys, vaddr_t virt,
    vm_prot_t prot);

/**
 * Reset the protection flags for an existing pageable mapping. Does not carry
 * out TLB shootdowns.
 */
void pmap_reenter(vm_map_t *map, struct vm_page *page, vaddr_t virt,
    vm_prot_t prot);

/*!
 * Reenter all mappings of a page read-only. Carries out TLB shootdowns.
 */
void pmap_reenter_all_readonly(struct vm_page *page);

/*!
 * Unmap a single page of a pageable mapping. CPU local TLB invalidated; not
 * others. TLB shootdown may therefore be required afterwards. Page's pv_table
 * updated accordingly.
 *
 * @param map from which map to remove the mapping.
 * @param page which page to unmap from \p map.
 * @param virt virtual address to be unmapped from \p map.
 * @param pv optionally specify which pv_entry_t represents this mapping, if
 * already known, so it does not have to be found again.
 */
void pmap_unenter(vm_map_t *map, struct vm_page *page, vaddr_t virt,
    pv_entry_t *pv);

/*!
 * Low-level unmapping of a page. Invalidates local TLB but does not do a TLB
 * shootdown. Tracking is not touched.
 */
struct vm_page *pmap_unenter_kern(struct vm_map *map, vaddr_t virt);

/*!
 * Invalidate a page mapping for the virtual address \p addr in the current
 * address space.
 */
void pmap_invlpg(vaddr_t addr);

/*!
 * Invalidate a page mapping for virtual address \p addr in all address spaces.
 */
void pmap_global_invlpg(vaddr_t vaddr);

/*!
 * @}
 */

#endif /* VM_H_ */
