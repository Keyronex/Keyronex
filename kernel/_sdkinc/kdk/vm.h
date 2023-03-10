/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Feb 11 2023.
 */
/*!
 * @file vm.h
 * @brief Virtual Memory Manager public interface.
 *
 * Locking
 * -------
 *
 * The general principle: the PFN lock is big and covers everything done from
 * within an object down.
 *
 * The VAD list mutex protects process' trees of VAD lists.
 */

#ifndef KRX_KDK_VM_H
#define KRX_KDK_VM_H

#include <bsdqueue/queue.h>
#include <bsdqueue/tree.h>
#include <stdint.h>

#ifdef __amd64
#include "./amd64/vmamd64.h"
#else
#error "Port Virtual Memory to this platform"
#endif

#include "./kernel.h"
#include "./objhdr.h"
#include "./vmem_impl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*! Fault flags. For convenience, matches amd64 MMU. */
typedef enum vm_fault_flags {
	kVMFaultPresent = 1,
	kVMFaultWrite = 2,
	kVMFaultUser = 4,
	kVMFaultExecute = 16,
} vm_fault_flags_t;

/*!
 * Fault return values.
 */
typedef enum vm_fault_return {
	kVMFaultRetOK = 0,
	kVMFaultRetFailure = -1,
	kVMFaultRetPageShortage = -2,
	kVMFaultRetRetry = -3,
} vm_fault_return_t;

/*!
 * Protection flags.
 */
typedef enum vm_protection {
	kVMRead = 0x1,
	kVMWrite = 0x2,
	kVMExecute = 0x4,
	kVMAll = kVMRead | kVMWrite | kVMExecute,
} vm_protection_t;

typedef uint64_t drumslot_t;

struct vm_stat {
	size_t npfndb;
	size_t nfree;
	size_t nmodified;
	size_t nactive;
	size_t ntransitioning;
	size_t nvmm;
};

enum vm_page_use {
	/*! The page is on the freelist. */
	kPageUseFree,
	/*! The page is mapped in at least one working set. */
	kPageUseActive,
	/*! The page is on the modified or standby list. */
	kPageUseModified,
	/*! The page is in transition (I/O going on). */
	kPageUseTransition,
	/*! The page is used by kernel wired memory. */
	kPageUseWired,
	/*! The page is used by the VMM directly (wired) */
	kPageUseVMM,
};

enum vm_pageable_page_use {
	/*! Process private anonymous memory. */
	kPageableUseProcessPrivate,
	/*! VNode page. */
	kPageableUseVNode,
	/*! Fork object use. */
	kPageableUseFork,
	/*! Anonymous object use. */
	kPageableUseAnonObj,
};

/*!
 * Resident page description. Obviously non-pageable.
 */
typedef struct vm_page {
	uint64_t
	    /*! What is the use of this page? */
	    /*enum vm_page_use */
	    use : 4,
	    /*! Is it dirty? */
	    dirty : 1,
	    /*! Spare slot. */
	    unused : 1,
	    /*! If active/modified/transition, owned by anonymous obj? */
	    /* enum vm_pageable_page_use */
	    pageable_use : 2,
	    /*! How many reasons are there to keep it in-memory? */
	    refcnt : 16,
	    /*! Physical address - multipled by PGSIZE. Gives 4 PiB. */
	    address : 40;

	/*! Linkage in free/modified/standby queue. */
	STAILQ_ENTRY(vm_page) queue_entry;

	/*! first use and pageable_use dependent-field */
	union {
		/*! kPageableUseProcessPrivate */
		struct vm_procstate *proc;
		/*! kPageableUseVNode */
		struct vnode *vnode;
		/*! kPageableUseAnonObj */
		struct vm_aobj *anonobj;
		/*! kPageableUseFork */
		struct vmp_forkobj *forkobj;

		/*! use = kPageUseTransition */
		struct vmp_paging_state *paging_state;
	};

	/*! second use and pageable_use dependent field */
	union {
		/*! For private, vnode, and anonobj. */
		uintptr_t offset;
		/*! For kPageableUseFork. */
		struct vmp_forkpage *forkpage;
	};
} vm_page_t;

/*!
 * Page description either for anonymous objects (in which case the "resident"
 * bit is meaningful) or for vnode objects (in which case the page must always
 * be resident).
 *
 * Non-pageable for now.
 */
typedef struct vmp_vpage {
	/*! PTE */
	pte_t pte;
	/*! Offset within the vnode/aobj. */
	voff_t offset;
	/*! vnode/aobj rbtree linkage */
	RB_ENTRY(vm_aobj_page) rb_entry;
} vmp_vpage_t;

RB_HEAD(vmp_vpage_rb, vmp_vpage);

/*!
 * Anonymous VM object.
 */
typedef struct vm_aobj {
	struct vmp_vpage_rb page_rb;
} vm_aobj_t;

struct vmp_forkpage {
	/*! the PTE */
	uint64_t pte;
	/*! offset within vmp_forkobj->pages; gives us 16 tib */
	uint32_t offset;
	/*! number of references to it, controls CoW behaviour */
	uint32_t refcnt;
};

/*!
 * Fork shared-pages object.
 */
typedef struct vmp_forkobj {
	/*! Mutex - needed??? */
	kmutex_t mutex;
	/*! How many pages are in it? */
	size_t npages;
	/*! How many pages of it are referenced? */
	size_t npages_referenced;
	/*! Pointer to array (in packed kernel memory) of the shared pages */
	struct vmp_fork_page *pages;
} vmp_forkobj_t;

/*!
 * Working-set list (or WSL). The set of pageable pages resident in a process.
 * (this really needs rewriting to be less space-inefficient)
 * Locked by PFN DB lock currently, in future should be its own mutex.
 */
typedef struct vm_wsl {
#if 0
	/*! Lock on WSL and on page tables of the associated process. */
	kmutex_t mutex;
#endif
	/*! Number of entries in the working set list. */
	size_t count;
	/*! RB tree of entries. */
	RB_HEAD(vmp_wsle_rbtree, vmp_wsle) rbtree;
	/*! Tailqueue of entries, for FIFO replacement. */
	TAILQ_HEAD(, vmp_wsle) queue;
} vm_wsl_t;

enum vm_vad_inheritance {
	/*! Inherit a shared entry (though not necessarily writeable!)
	 */
	kVADInheritShared,
	/*! Inherit a virtual copy. */
	kVADInheritCopy,
	/*! Special case for stacks - inherit only current thread's. */
	kVADInheritStack
};

/*!
 * Virtual Address Descriptor - a mapping of a section object. Note that
 * copy-on-write is done at the section object level, not here.
 */
typedef struct vm_vad {
	/*! Entry in vm_procstate::vad_rbtree */
	RB_ENTRY(vm_vad) rb_entry;

	/*! Start and end vitrual address. */
	vaddr_t start, end;
	/*! Offset into section object. */
	voff_t offset;

	/*! Inheritance attributes for fork. */
	enum vm_vad_inheritance inheritance;

	/*!
	 * Current protection of the region.
	 */
	vm_protection_t protection;

	/*!
	 * Maximum protection of the region. Set at creation.
	 */
	vm_protection_t max_protection;
} vm_vad_t;

/*! Per-process memory manager state. */
typedef struct vm_procstate {
	/*! Process' working-set list. */
	vm_wsl_t wsl;
	/*! VAD queue lock. */
	kmutex_t mutex;
	/*! VAD tree. */
	RB_HEAD(vm_vad_rbtree, vm_vad) vad_queue;
	/*! VMem allocator state. */
	vmem_t vmem;
	/*! Per-port VM state. */
	struct vm_ps_md md;
} vm_procstate_t;

/*!
 * Memory descriptor list.
 */
typedef struct vm_mdl {
	size_t npages;
	vm_page_t *pages[0];
} vm_mdl_t;

extern struct vm_stat vmstat;

/*! @brief Acquire the PFN database lock. */
#define vmp_acquire_pfn_lock() ke_spinlock_acquire(&vmp_pfn_lock)

/*! @brief Release the PFN database lock. */
#define vmp_release_pfn_lock(IPL) ke_spinlock_release(&vmp_pfn_lock, IPL)

/*! @brief Add a region of physical memory to VMM management. */
void vmp_region_add(paddr_t base, size_t length);

/*! @brief Initialise the kernel wired memory system. */
void vmp_kernel_init(void);

/*!
 * @brief Allocate a physical page.
 *
 * Reference count of page is set to 1.
 *
 * @param[optional] vmps Process to charge for the allocation.
 * @param must Whether the allocaton must be served.
 * @param[out] out Allocated page's PFN db entry will be written here.
 * @param use What the page will be used for.
 *
 * @pre PFN database lock held.
 *
 * @retval 0 Page allocated.
 * @retval 1 Low memory, drop locks and wait on the low-memory event.
 */
int vmp_page_alloc(vm_procstate_t *ps, bool must, enum vm_page_use use,
    vm_page_t **out);

/*!
 * @brief Free a physical page.
 *
 * Page's reference count must be 0.
 *
 * @pre PFN database lock held.
 */
void vmp_page_free(vm_procstate_t *ps, vm_page_t *page);

/*! @brief Get the PFN database entry for a physical page address. */
vm_page_t *vmp_paddr_to_page(paddr_t paddr);

/*! @brief Copy the contents of one page to another. */
void vmp_page_copy(vm_page_t *from, vm_page_t *to);

/*! @brief Translate wired virtual to physical address in current process. */
paddr_t vm_translate(vaddr_t vaddr);

/*! @brief Allocate an MDL of a given size without initialising page entries. */
vm_mdl_t *vm_mdl_alloc(size_t npages);

/*! @brief Allocate an MDL populated with pages for a buffer. */
vm_mdl_t *vm_mdl_buffer_alloc(size_t npages);

/*! @brief Allocate an MDL to describe kernel wired memory. */
vm_mdl_t *vm_mdl_kwired_alloc(void *addr, size_t len);

/*! @brief Map an MDL so the kernel can access it. */
void vm_mdl_map(vm_mdl_t *mdl, void **out);

/*! @brief Memcpy out of an MDL. */
void vm_mdl_memcpy(void *dest, vm_mdl_t *mdl, voff_t off, size_t n);

/*! @brief Allocated kernel wired pages and address space. */
vaddr_t vm_kalloc(size_t npages, vmem_flag_t flags);

/*! @brief Free kernel wired pages. */
void vm_kfree(vaddr_t addr, size_t npages, vmem_flag_t flags);

/*! @brief Activate a process' virtual address space. */
void vm_ps_activate(vm_procstate_t *vmps);

/*! @brief Handle a page fault. */
vm_fault_return_t vm_fault(vm_procstate_t *vmps, vaddr_t vaddr,
    vm_fault_flags_t flags, vm_page_t **out);

/*! @brief Allocate anonymous memory in a process. */
int vm_ps_allocate(vm_procstate_t *ps, vaddr_t *vaddrp, size_t size,
    bool exact);

/*! @brief Deallocate a range of virtual address space in a process. */
int vm_ps_deallocate(vm_procstate_t *vmps, vaddr_t start, size_t size);

/*!
 * @brief Forks a process' virtual address space
 *
 * This function forks a process' virtual address space according to POSIX
 * semantics. It should be called only from the context of a thread of that
 * process. The existing process' virtual memory process support structure \p
 * vmps is used as a template to fill entries in the new process' VMPS, \p
 * vmps_new, which should have been initialized with `vm_ps_init()`.
 *
 * VADs are established in the new VMPS which either share or virtual copy
 * (according to the VAD inheritance attributes) the section referred to by the
 * corresponding VAD in the old process. The reason why this function must be
 * invoked from the context of a thread which already exists is because, per
 * POSIX semantics, only the stack of the invoking thread is copied.
 *
 * Parts of the old process' address space which have been virtual-copied will
 * be mapped read-only if they are currently mapped read-write.
 *
 * @pre The VAD queue lock of both \p vmps and \p vmps_new must be held, and
 * the PFN database lock must not be held.
 * @post The VAD queue locks and the PFN database locks are not held.
 *
 * @param vmps The virtual memory process support structure of the existing
 * process
 * @param vmps_new The virtual memory process support structure of the new
 * process
 *
 * @return 0 on success, negative error code on failure
 */
int vm_ps_fork(vm_procstate_t *vmps, vm_procstate_t *vmps_new);

static inline paddr_t
vm_page_paddr(const vm_page_t *page)
{
	return page->address;
}

static inline vaddr_t
vm_page_vaddr(const vm_page_t *page)
{
	return (vaddr_t)P2V(vm_page_paddr(page));
}

extern kspinlock_t vmp_pfn_lock;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_KDK_VM_H */
