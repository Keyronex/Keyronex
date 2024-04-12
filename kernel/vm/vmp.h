#ifndef KRX_VM_VMP_H
#define KRX_VM_VMP_H

#include "kdk/nanokern.h"
#include "kdk/vm.h"

typedef union pte pte_t;

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
 * Virtual Address Descriptor - a mapping of a section object.
 */
typedef struct vm_vad {
	struct vm_vad_flags {
		/*! current protection, and maximum legal protection */
		vm_protection_t protection : 3, max_protection : 3;
		/*! whether shared on fork (if false, copied) */
		bool inherit_shared : 1;
		/*! whether mapping is private anonymous memory. */
		bool private : 1;
		/*! (!private only) whether the mapping is copy-on-write */
		bool cow : 1;
		/*! if !private, page-unit offset into section (max 256tib) */
		int64_t offset : 36;
	} flags;
	/*! Entry in vm_procstate::vad_rbtree */
	RB_ENTRY(vm_vad) rb_entry;
	/*! Start and end vitrual address. */
	vaddr_t start, end;
	/*! Section object; if flags.anonymous = false */
	vm_section_t *section;
} vm_vad_t;

struct vmp_wsl {
	/*! Working set entry queue - tail most recently added, head least. */
	TAILQ_HEAD(, vmp_wsle) queue;
	/*! Working set entry tree. */
	RB_HEAD(vmp_wsle_tree, vmp_wsle) tree;
	/*! Count of pages in working set list. */
	size_t ws_current_count;
	/*! Count of pages locked into working set list. */
	size_t locked_count;
};

/*!
 * Per-process state.
 */
typedef struct vm_procstate {
	/*! VAD queue + working set list lock. */
	kmutex_t mutex;
	/*! VMem. */
	vmem_t vmem;
	/*! Working set list. */
	struct vmp_wsl wsl;
	/*! VAD tree. */
	RB_HEAD(vm_vad_rbtree, vm_vad) vad_queue;
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
	kevent_t event;
	SLIST_ENTRY(vmp_pager_state) slist_entry;
	pfn_t vpfn: PFN_BITS;
	uint16_t length: 5;
};

struct vmp_forkpage {
	pte_t pte;
	uint32_t refcount;
};

struct vmp_filepage {
	uint64_t offset;
	vm_page_t *page;
	RB_ENTRY(vmp_filepage) rb_entry;
};

struct vm_section {
	/*! What kind of section is this? */
	enum vm_section_kind {
		kFile,
		kAnon,
	} kind;
	union {
		struct {
			RB_HEAD(vmp_file_page_tree, vmp_filepage) page_tree;
			struct vnode *vnode;
		} file;
		struct {
			struct vmp_amap_l3 *l3;
		} anon;
	};
};

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

/*!
 * @post If kVMFaultRetOK, reference held to each pagetable level and used PTE
 * count incremented on leaf table.
 */
vm_fault_return_t vmp_md_wire_pte(vm_procstate_t *vmps, vaddr_t vaddr,
    struct vmp_pte_wire_state *state);
/*! @brief Release the wires made by vmp_md_wire_pte(). */
void vmp_md_pte_wire_release(vm_procstate_t *vmps,
    struct vmp_pte_wire_state *state);
/*! @brief Called when a PTE goes from used to zeroed. */
void vmp_md_pagetable_pte_zeroed(vm_procstate_t *vmps, vm_page_t *pgtable_page);
/*! @brief Called when PTE in a pagetable page go from unused to used. */
void vmp_md_pagetable_ptes_created(struct vmp_pte_wire_state *state,
    size_t count);
/*! @brief Convert a virtual address to a physical. Must be mapped! */
paddr_t vmp_md_translate(vaddr_t addr);
/*! @brief Quickly get a PTE pointer. */
int pmap_get_pte_ptr(void *pmap, vaddr_t vaddr, pte_t **out,
    vm_page_t **out_page);
/*! @brief Locally invalidate a page. */
void pmap_invlpg(vaddr_t addr);
void vmp_md_setup_table_pointers(kprocess_t *ps, vm_page_t *dirpage,
    vm_page_t *tablepage, pte_t *dirpte, bool is_new);

void vmp_pages_dump(void);

int vmp_page_alloc_locked(vm_page_t **out, enum vm_page_use use, bool must);
int vmp_pages_alloc_locked(vm_page_t **out, size_t order, enum vm_page_use use,
    bool must);
void vmp_page_free_locked(vm_page_t *page);
void vmp_page_delete_locked(vm_page_t *page);
vm_page_t *vmp_page_retain_locked(vm_page_t *page);
void vmp_page_release_locked(vm_page_t *page);

int vmp_fault(vaddr_t vaddr, bool write, vm_page_t **out);

vm_vad_t *vmp_ps_vad_find(vm_procstate_t *ps, vaddr_t vaddr);

void vmp_wsl_insert(vm_procstate_t *ps, vaddr_t vaddr, bool locked);
void vmp_wsl_remove(vm_procstate_t *ps, vaddr_t vaddr);
/*! @brief Evict one entry from a working set list @pre PFN HELD*/
void wsl_evict_one(vm_procstate_t *ps);

/*!
 * Note: WS lock and PFN lock will be locked and unlocked regularly here.
 * \pre VAD list mutex held
 * \pre WS mutex held
 * \pre PFN database lock held
 */
int vmp_wire_pte(kprocess_t *ps, vaddr_t vaddr,
    struct vmp_pte_wire_state *state);
void vmp_pte_wire_state_release(struct vmp_pte_wire_state *state);

extern kspinlock_t vmp_pfn_lock;

extern struct vmem vmem_kern_nonpaged;
extern kspinlock_t vmp_pfn_lock;

#endif /* KRX_VM_VMP_H */
