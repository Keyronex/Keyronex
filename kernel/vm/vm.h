#ifndef VM_H_
#define VM_H_

#include <kern/vmem_impl.h>
#include <md/vm.h>
#include <nanokern/kerndefs.h>
#include <nanokern/thread.h>
#include <nanokern/tree.h>

#include <stddef.h>

#include "kern/obj.h"

#define VADDR_MAX (vaddr_t) UINT64_MAX

#define ROUNDUP(addr, align) ((((uintptr_t)addr) + align - 1) & ~(align - 1))
#define ROUNDDOWN(addr, align) ((((uintptr_t)addr)) & ~(align - 1))

#define PTRROUNDUP(addr) ROUNDUP(addr, (sizeof(uintptr_t)))
/** Round a value up to pointer alignment. */
#define PTRROUNDDOWN(addr) ROUNDDOWN(addr, (sizeof(uintptr_t)))

#define PGROUNDUP(addr) ROUNDUP(addr, PGSIZE)
#define PGROUNDDOWN(addr) ROUNDDOWN(addr, PGSIZE)

typedef uintptr_t drumslot_t;

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
 * Fault return values.
 */
typedef enum vm_fault_ret {
	kVMFaultRetOK = 0,
	kVMFaultRetFailure = -1,
	kVMFaultRetPageShortage = -2,
} vm_fault_ret_t;

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

/*!
 * Represents a physical page of useable general-purpose memory.
 *
 * If neither anon nor obj are set, it may be internally managed (kmem).
 * In the case of tmpfs, both may be set.
 *
 * (~) constant
 * (q) vm_pgq_lock
 */
typedef struct vm_page {
	/*! (q) Links to its queue: kmem, wired, active, . */
	TAILQ_ENTRY(vm_page) pagequeue;

	/*! (q) Page state. */
	enum vm_page_queue queue : 4;

	bool
	    /*! (q) Has it been written to since page-in/allocation? */
	    dirty : 1,
	    /*! (q) Has it been read since going on its queue? */
	    accessed : 1,
	    /*! (q) Is it being paged in/out? */
	    busy : 1,
	    /*!
	     * (q) If it's on a pageable queue, is it an anon's page? (if not,
	     * it must be a regular VM object's page)
	     */
	    is_anon : 1;

	union {
		struct vm_anon *anon; /*! (q) if belonging to an anon */
		struct vm_object
		    *obj; /*! (q) if belonging to non-anon object */
	};

	/*! (q) physical page -> virtual mappings */
	LIST_HEAD(, pv_entry) pv_table;

	/*! (~) physical address of page */
	paddr_t paddr;
} vm_page_t;

typedef struct vm_pagequeue {
	TAILQ_HEAD(pagequeue, vm_page) queue;
	size_t			       npages;
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
extern vm_pagequeue_t vm_pgfreeq, vm_pgkmemq, vm_pgwiredq, vm_pgdevbufq,
    vm_pgactiveq, vm_pginactiveq, vm_pgpmapq;

/*! Page region queue. */
extern vm_pregion_queue_t vm_pregion_queue;

/*! Page queues lock. */
extern kspinlock_t vm_pgq_lock;

#define VM_PGQ_LOCK() nk_spinlock_acquire(&vm_pgq_lock)
#define VM_PGQ_UNLOCK(IPL) nk_spinlock_release(&vm_pgq_lock, IPL)

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
 * \pre vm_pgq_lock held
 */
void vm_page_changequeue(vm_page_t *page, kx_nullable vm_pagequeue_t *from,
    vm_pagequeue_t *to);

/*! The page queues. */
extern vm_pagequeue_t vm_pgfreeq, vm_pgkmemq, vm_pgwiredq, vm_pgactiveq,
    vm_pginactiveq, vm_pgpmapq;

/*! Page region queue. */
extern vm_pregion_queue_t vm_pregion_queue;

/*! Total number of usable main-memory pages. */
extern size_t vm_npages;

/*!
 * @}
 */

/*!
 * @name Memory Descriptor Lists
 * @{
 */

/*!
 * A Memory Descriptor List (MDL) - handle to a set of pages which represent a
 * virtually contiguous region of some sort. The pages are busied while
 * referenced by an MDL (XXX so a page can only belong to one MDL at a time? is
 * that a good policy, and should we ensure it, i.e. test for page business
 * before putting one in an MDL? I think we ought to test that anyway.)
 * guaranteeing they remain resident.
 *
 * An MDL is the only means by which busy pages contents may legitimately be
 * written to.
 *
 * @todo vm_mdl_new_with_range(map, object, start, end)
 */
typedef struct vm_mdl {
	voff_t		offset;
	size_t		nBytes;
	size_t		nPages;
	struct vm_page *pages[0];
} vm_mdl_t;

/*!
 * Expand (if necessary) a buffer MDL such that it's large enough to store
 * \p nBytes bytes.
 *
 * @returns -ENOMEM if there wasn't enough memory to expand it.
 */
int vm_mdl_expand(vm_mdl_t **mdl, size_t nBytes);

/*!
 * Create a buffer MDL large enough to store \p nBytes bytes.
 * @todo accept options to be given to vm_pagealloc
 */
int vm_mdl_new_with_capacity(vm_mdl_t **mdl, size_t nBytes);

/*!
 * Get the capacity in bytes of an MDL.
 */
size_t vm_mdl_capacity(vm_mdl_t *mdl);

/*!
 * Copy data from an MDL into a buffer.
 */
void vm_mdl_copy(vm_mdl_t *mdl, void *buf, size_t nBytes, voff_t off);

/*!
 * Zero out an entire MDL.
 */
void vm_mdl_zero(vm_mdl_t *mdl);

/*!
 * @} ()
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
	struct vm_object	 *obj;
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
vm_fault_ret_t vm_fault(md_intr_frame_t *frame, vm_map_t *map, vaddr_t vaddr,
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

/*!
 * Fork a VM map.
 */
vm_map_t *vm_map_fork(vm_map_t *map);

/*! Release and free a VM map. */
void vm_map_release(vm_map_t *map);

/*! Create a new empty vm_map. */
vm_map_t *vm_map_new();

/*! Global kernel map. */
extern vm_map_t kmap;

/*!
 * @}
 */

/*!
 * @name Objects
 * @{
 */
/**
 * Represents a pager-backed virtual memory object.
 */
typedef struct vm_object {
	objectheader_t hdr;
	kmutex_t       lock;

	enum {
		kVMObjAnon,
		kVMObjFile,
	} type;
	size_t size; /**< size in bytes */

	union {
		struct {
			struct vm_amap *amap;
			/** if not 0, the maximum size of this object */
			size_t maxsize;
			/*! parent vm object */
			struct vm_object *parent;
		} anon;
	};
} vm_object_t;

/**
 * Represents a logical page of pageable memory. May be resident or not.
 */
typedef struct vm_anon {
	kmutex_t lock;
	int refcnt : 24, /** number of amaps referencing it; if >1, must COW. */
	    resident : 1; /** whether currently resident in memory */

	union {
		struct vm_page *physpage; /*!< physical page if resident */
		drumslot_t	drumslot; /*!< drumslot if nonresident */
	};
} vm_anon_t;

#define kAMapChunkNPages 32

/* Entry in a vm_amap_t. Locked by the vm_object's lock */
typedef struct vm_amap_chunk {
	vm_anon_t *anon[kAMapChunkNPages];
} vm_amap_chunk_t;

/**
 * An anonymous map - map of anons. These are always paged by the default pager
 * (vm_compressor).
 */
typedef struct vm_amap {
	vm_amap_chunk_t **chunks;    /**< sparse array pointers to chunks */
	size_t		  curnchunk; /**< number of slots in chunks */
} vm_amap_t;

/*! Release an anon. */
void anon_release(vm_anon_t *anon);

/*!
 * Allocate a new anonymous VM object of size \p size bytes.
 */
vm_object_t *vm_aobj_new(size_t size);
/*!
 * Create a (copy-on-write optimised) copy of a VM object.
 *
 * The exact semantics of a copy vary depending on what sort of object is
 * copied:
 * - Copying an anonymous object copies all the pages belonging to that
 * anonymous object (albeit with copy-on-write optimisation)
 * - Copying another type of object yields a new anonymous object with no pages;
 * the new object is assigned the copied object as parent, and when a page is
 * absent from the copied object, its parent is checked to see whether it holds
 * the page. Changes to the parent are therefore reflected in the copied object;
 * unless and until the child object tries to write to one of these pages, which
 * copies it.
 */
vm_object_t *vm_object_copy(vm_object_t *obj);
/*!
 * Free a VM object. This is only used by the refcounted objects mechanism,
 * not directly, so by this point the object is no longer referenced by any maps
 * or indeed anything else.
 */
void vmx_object_release(vm_object_t *obj);

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
 * \pre page object lock held //vm_pgq_lock held
 */
void pmap_enter(vm_map_t *map, struct vm_page *page, vaddr_t virt,
    vm_prot_t prot);

/*!
 * Low-level mapping a single physical page to a virtual address. Mappings
 * are not tracked.
 * \pre page object lock held (if relevant)
 */
void pmap_enter_kern(struct pmap *pmap, paddr_t phys, vaddr_t virt,
    vm_prot_t prot);

/**
 * Reset the protection flags for an existing pageable mapping. Does not carry
 * out TLB shootdowns.
 * \pre page object lock held //vm_pgq_lock held
 */
void pmap_reenter(vm_map_t *map, struct vm_page *page, vaddr_t virt,
    vm_prot_t prot);

/*!
 * Reenter all mappings of a page read-only. Carries out TLB shootdowns.
 * \pre page object lock held //vm_pgq_lock held
 */
void pmap_reenter_all_readonly(struct vm_page *page);

/*!
 * Unmap a single mapping of a pageable page. CPU local TLB invalidated; not
 * others. TLB shootdown may therefore be required afterwards. Page's pv_table
 * updated accordingly.
 *
 * @param map from which map to remove the mapping.
 * @param page which page to unmap from \p map.
 * @param virt virtual address to be unmapped from \p map.
 * @param pv optionally specify which pv_entry_t represents this mapping, if
 * already known, so it does not have to be found again.
 *
 * \pre page object lock held //vm_pgq_lock held
 */
void pmap_unenter(vm_map_t *map, struct vm_page *page, vaddr_t virt,
    pv_entry_t *pv);

/*!
 * Unmap all mappings of a pageable page. TLB shootdown will be issued.
 *
 * @param page which page to unmap
 *
 * \pre page object lock held //vm_pgq_lock held
 */
void pmap_unenter_all(struct vm_page *page);

/*!
 * Low-level unmapping of a page. Invalidates local TLB but does not do a TLB
 * shootdown. Tracking is not touched.
 */
struct vm_page *pmap_unenter_kern(struct vm_map *map, vaddr_t virt);

/*!
 * Check if a page was accessed. Returns true if it was.
 * @param reset whether to reset this bit to 0 in all mappings (and issue
 * appropriate TLB shootdowns.)
 * \pre page object lock held //vm_pgq_lock held
 */
bool pmap_page_accessed(struct vm_page *page, bool reset);

/*!
 * Invalidate a page mapping for the virtual address \p addr in the current
 * address space.
 * \pre page object lock held //vm_pgq_lock held
 */
void pmap_invlpg(vaddr_t addr);

/*!
 * Invalidate a page mapping for virtual address \p addr in all address spaces.
 * \pre page object lock held for page to which this corresponds (if relevant)
 */
void pmap_global_invlpg(vaddr_t vaddr);

/*!
 * @}
 */

/*!
 * Paging
 * @{
 */

typedef enum vm_pager_ret {
	/*! pager successfully paged in/out the page */
	kVMPagerOK,
	/*! pager has no space left */
	kVMPagerExhausted,
} vm_pager_ret_t;

/*! The page daemon thread entry point. Only to be used once, in start(). */
void vm_pdaemon(void *unused);

/*! Are there enough free pages according to the pagedaemon's criterion? */
bool vm_enoughfree(void);

/*! Fallible allocation of a page. */
vm_fault_ret_t vm_pagetryalloc(vm_page_t **out, vm_pagequeue_t *queue);

/*! Signal the pagedaemon that a low-free-pages situation has developed. */
void vm_pd_signal(void);

/*! Wait for the pagedaemon to notify that memory is free. */
void vm_pd_wait(void);

/*!
 * @}
 */

/*!
 * @name swap
 * @{
 */

int vm_swapon(const char *name);

vm_pager_ret_t vm_swp_pageout(vm_page_t *page);

void vm_swapstat(void);

/*!
 * @} (swap)
 */

#endif /* VM_H_ */
