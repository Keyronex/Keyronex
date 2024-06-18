#ifndef KRX_VM_VMP_H
#define KRX_VM_VMP_H

#include "kdk/executive.h"
#include "kdk/nanokern.h"
#include "kdk/vm.h"

typedef union pte pte_t;
struct eprocess;

#if defined(__m68k__)
#include "m68k/vmp_m68k.h"
#elif defined(__aarch64__)
#include "aarch64/vmp_aarch64.h"
#elif defined(__amd64__)
#include "amd64/vmp_amd64.h"
#else
#error "Unknown port!"
#endif

struct vmp_pte_wire_state {
	/*! the process */
	vm_procstate_t *vmps;
	/*! References to page table pages for wiring. */
	vm_page_t *pgtable_pages[5];
	/*! the faulting address */
	vaddr_t addr;
	/*! the PTE (wired via pgtable_pages) */
	pte_t *pte;
};

/*!
 *  Page fault return values.
 */
typedef enum vm_fault_return {
	kVMFaultRetOK = 0,
	kVMFaultRetFailure = -1,
	kVMFaultRetPageShortage = -2,
	kVMFaultRetRetry = -3,
} vm_fault_return_t;

/*!
 * Virtual Address Descriptor - a mapping of a object object.
 */
typedef struct vm_map_entry {
	struct vm_map_entry_flags {
		/*! current protection, and maximum legal protection */
		vm_protection_t protection : 3, max_protection : 3;
		/*! whether shared on fork (if false, copied) */
		bool inherit_shared : 1;
		/*! (!private only) whether the mapping is copy-on-write */
		bool cow : 1;
		/*! if !private, page-unit offset into object (max 256tib) */
		int64_t offset : 36;
	} flags;
	/*! object; if flags.anonymous = false */
	vm_object_t *object;
	/*! Entry in vm_procstate::map_entry_rbtree */
	RB_ENTRY(vm_map_entry) rb_entry;
	/*! Entry in vm_object::map_entry_list */
	LIST_ENTRY(vm_map_entry) object_entry;
	/*! Start and end vitrual address. */
	vaddr_t start, end;
} vm_map_entry_t;

#if BITS == 64
typedef uint32_t wsindex_t;
#else
typedef uint16_t wsindex_t;
#endif

#define NIL_WSE ((wsindex_t)(-1))

struct vmp_wsl {
	vm_procstate_t *vmps;
	struct wse *nodes;  /*!< Entry array. */
	size_t capacity;    /*!< Working set cap - can be grown/shrunk */
	size_t limit;	    /*!< Current working set size limit */
	size_t size;	    /*!< Number of pages currently in the set. */
	wsindex_t freelist; /*!< Next WSE index to replace. */
	wsindex_t head; /*!< Index to begin search for entry to replace from. */
	size_t hash_size; /*!< Size of hash table. */
	wsindex_t *hash;  /*!< Hash table for looking up shared WSEs. */
};

/*!
 * Per-process state.
 * (n_anonymous is supposedly protected by ws_mutex, but effectively also by
 * map_lock held exclusive, because the creation of new anonymous pages happens
 * only while the map lock is held in the fault handler.)
 */
typedef struct vm_procstate {
	/*! Working set list lock. */
	kmutex_t ws_mutex;
	/*! Mapping lock. */
	ex_rwlock_t map_lock;
	/*! VMem. */
	vmem_t vmem;
	/*! Working set list. */
	struct vmp_wsl wsl;
	/*! VAD tree. */
	RB_HEAD(vm_map_entry_rbtree, vm_map_entry) vad_queue;

	/*! Count of private anonymous pages, including paged-out ones. */
	size_t n_anonymous;

	/*! Entry in the trimming queue. Protected by trimmer lock. */
	TAILQ_ENTRY(vm_procstate) balance_set_entry;
	/*! Value of trim counter at last trim. Protected by trimmer lock. */
	uint32_t last_trim_counter;

	/*! Per-arch stuff. */
	struct vmp_md_procstate md;
} vm_procstate_t;

/*! roto-level shared anonymous table */
struct vmp_amap_l3 {
	struct vmp_amap_l2 *entries[512];
};

/*! mid-level shared anonymous table */
struct vmp_amap_l2 {
	struct vmp_amap_l1 *entries[512];
};

/*! leaf-level shared anonymous table */
struct vmp_amap_l1 {
	pte_t entries[512];
};

struct vmp_pager_state {
	uint32_t refcnt;
	kevent_t event;
	SLIST_ENTRY(vmp_pager_state) slist_entry;
	pfn_t vpfn : PFN_BITS;
	uint16_t length : 5;
	vm_mdl_t mdl;
	vm_page_t *pages[1];
};

struct vmp_forkpage {
	pte_t pte;
	uintptr_t refcount;
};

struct vmp_filepage {
	uint64_t offset;
	vm_page_t *page;
	RB_ENTRY(vmp_filepage) rb_entry;
};

struct vm_object {
	/*! What kind of object is this? */
	enum vm_object_kind {
		kFile,
		kAnon,
	} kind;
	paddr_t vpml4;
	size_t size; /*< or, highest byte to be written back */
	union {
		struct {
			struct vnode *vnode;
			size_t n_dirty_pages;
		} file;
		struct {
			struct ex_memory_object *object;
		} anon;
	};

	/*! Protects map_entry_list. */
	kmutex_t map_entry_list_lock;
	/*! All mappings of this object. (please see map.c for notes) */
	LIST_HEAD(, vm_map_entry) map_entry_list;
};

typedef TAILQ_HEAD(, vm_page) page_queue_t;

/*! @brief Acquire the PFN database lock. */
#define vmp_acquire_pfn_lock() ke_spinlock_acquire(&vmp_pfn_lock)

/*! @brief Release the PFN database lock. */
#define vmp_release_pfn_lock(IPL) ke_spinlock_release(&vmp_pfn_lock, IPL)

/* paddr_t vmp_page_paddr(vm_page_t *page) */
#define vmp_page_paddr(PAGE) ((paddr_t)(PAGE)->pfn << VMP_PAGE_SHIFT)

/* paddr_t vmp_pfn_to_paddr(pfn_t pfn) */
#define vmp_pfn_to_paddr(PFN) ((paddr_t)PFN << VMP_PAGE_SHIFT)

/* NOTE: we require that we can always get to the *page* this way, regardless of
 * level, even though on e.g. 68040 the root and 2nd levels have a more specific
 * pointer in their PTEs */
/* vm_page_t *vmp_pte_hw_page(pte_t *pte, int level) */
#define vmp_pte_hw_page(PTE, LVL) vm_paddr_to_page(vmp_pte_hw_paddr(PTE, LVL))

/*! Initialise kernel virtual memory. */
void vmp_kernel_init(void);
/*! Initialise paging. */
void vmp_paging_init(void);

/*! Initialise kernel virtual memory, platform-specific part. */
void vmp_md_kernel_init(void);
/*! @brief Convert a virtual address to a physical. Must be mapped! */
paddr_t vmp_md_translate(vaddr_t addr);

enum vmp_table_old_state { kWasZero, kWasSwap, kWasTrans };
void vmp_md_setup_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    vm_page_t *tablepage, pte_t *dirpte, enum vmp_table_old_state old_state);
void vmp_md_busy_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    pte_t *dirpte, vm_page_t *tablepage);
void vmp_md_trans_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    pte_t *dirpte, vm_page_t *tablepage);
void vmp_md_swap_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    pte_t *dirpte, uintptr_t drumslot);
void vmp_md_delete_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    pte_t *pte);

void vmp_pagetable_page_noswap_pte_created(vm_procstate_t *ps, vm_page_t *page,
    bool is_new);
void vmp_pagetable_page_pte_deleted(vm_procstate_t *ps, vm_page_t *page,
    bool was_swap);
void vmp_pagetable_page_pte_became_swap(vm_procstate_t *ps, vm_page_t *page);

void vmp_pages_dump(void);

int vmp_page_alloc_locked(vm_page_t **out, enum vm_page_use use, bool must);
int vmp_pages_alloc_locked(vm_page_t **out, size_t order, enum vm_page_use use,
    bool must);
void vmp_page_free_locked(vm_page_t *page);
void vmp_page_delete_locked(vm_page_t *page);
vm_page_t *vmp_page_retain_locked(vm_page_t *page);
void vmp_page_release_locked(vm_page_t *page);

int vmp_fault(md_intr_frame_t *frame, vaddr_t vaddr, bool write, vm_page_t **out);

vm_map_entry_t *vmp_ps_vad_find(vm_procstate_t *ps, vaddr_t vaddr);

int vmp_wsl_init(vm_procstate_t *vmps, struct vmp_wsl *ws);
wsindex_t vmp_wsl_insert(vm_procstate_t *ps, vaddr_t vaddr, bool shared);
void vmp_wsl_remove(vm_procstate_t *ps, vaddr_t vaddr, wsindex_t hint);
/*! @brief Dump info on a working set. */
void vmp_wsl_dump(vm_procstate_t *ps);
void vmp_wsl_trim(struct vmp_wsl *ws, size_t n);
/*!
 * @brief Evict a valid page
 * \pre Working set mutex held
 * \pre PFN lock held
 */
void vmp_page_evict(vm_procstate_t *vmps, pte_t *pte, vm_page_t *pte_page,
    vaddr_t vaddr);

/*!
 * \pre Working set mutex held
 * \pre PFN database lock held
 * @retval -n (only if !create) At PMLn, no PML(n - 1) was found.
 * @retval 0 PTE wired with no locks released.
 * @retval 1 PTE wired but locks were released at least once.
 */
int vmp_wire_pte(struct eprocess *ps, vaddr_t vaddr, vm_object_t *object,
    struct vmp_pte_wire_state *state, bool create);
void vmp_pte_wire_state_release(struct vmp_pte_wire_state *state,
    bool prototype);

int vmp_fetch_pte(vm_procstate_t *ps, vaddr_t vaddr, pte_t **pte_out);

/*!
 * @brief Unmap a range of a process.
 * \pre Map control lock held.
 */
int vmp_unmap_range(vm_procstate_t *vmps, vaddr_t start, vaddr_t end);

/*!
 * \pre PFN database lock held
 */
void vmp_pager_state_retain(struct vmp_pager_state *state);
void vmp_pager_state_release(struct vmp_pager_state *state);

extern kspinlock_t vmp_pfn_lock;

extern struct vmem vmem_kern_nonpaged;
extern page_queue_t vm_pagequeue_modified, vm_pagequeue_standby;

extern vm_procstate_t kernel_procstate;

#endif /* KRX_VM_VMP_H */
