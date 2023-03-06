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
	/*! The page is in transition. */
	kPageUseTransition,
	/*! The page is used by kernel wired memory. */
	kPageUseWired,
	/*! The page is used by the VMM directly (wired) */
	kPageUseVMM,
};

/*!
 * Physical page frame description. Whole thing (?) locked by the PFN lock.
 */
typedef struct vm_page {
	/*! Queue linkage. */
	STAILQ_ENTRY(vm_page) queue_entry;
	/*! By how many PTEs is it mapped? */
	uint16_t reference_count;
	/*! What is its current use? */
	enum vm_page_use use : 4;
	/*! Flags */
	unsigned
	    /*! the page (may) have been written to; needs to be laundered */
	    dirty : 1,
	    /*! the page is being moved in, or out, of memory */
	    busy : 1;

	/*! Page's physical address. */
	paddr_t address;

	union {
		/*! Virtual page, if ::is_anonymous. */
		struct vm_vpage *vpage;
		/*! VNode section, if ::is_vnode */
		struct vnode *vnode;
	};
} vm_page_t;

/*!
 * Virtual page frame for anonymous pages, which are created both for anonymous
 * memory proper as well as for to provide the copy-on-write layer for mapped
 * files mapped copy-on-write.
 *
 * Whole thing (?) locked by the PFN lock.
 */
typedef struct vm_vpage {
#if 0
	/*! mutex, locks most its fields */
	kmutex_t mutex;
#endif
	/*! physical page frame description if resident */
	vm_page_t *page;
	/*! swap descriptor, if it's been written */
	uintptr_t swapdesc;
	/*!
	 * How many section objects contain it? Note not 'how many times is it
	 * mapped into working sets'. That's what vm_page_t tracks. This
	 * reference count drives the copy-on-write logic.
	 */
	size_t refcount;
} vm_vpage_t;

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

/*!
 * Section - an object which can be mapped into a process' address space.
 * Contents mostly locked by the PFN DB lock..
 */
typedef struct vm_section {
	object_header_t header;

#if 0
	/*! object lock */
	kmutex_t mutex;
#endif

	/*! size in bytes */
	size_t size;

	/*! what kind is it? */
	enum vm_section_kind {
		kSectionFile,
		kSectionAnonymous,
	} kind;

	/*! RB tree of references to vpages (for anonymous) or pages (for file).
	 */
	RB_HEAD(vmp_page_ref_rbtree, vmp_page_ref) page_ref_rbtree;

	union {
		/*! If kind = kSectionAnonymous, the file section we copied, if
		 * this is a virtual copy of a file section. */
		struct vm_section *parent;
		/* If this is a file, a (?non-owning) pointer to the vnode,  */
		struct vnode *vnode;
	};
} vm_section_t;

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
	RB_ENTRY(vm_vad) rbtree_entry;

	/*! Start and end vitrual address. */
	vaddr_t start, end;
	/*! Offset into section object. */
	voff_t offset;

	/*! Section object. */
	vm_section_t *section;

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
 * @brief Map a view of a section into a process.
 */
int vm_ps_map_section_view(vm_procstate_t *ps, vm_section_t *section,
    krx_in krx_out vaddr_t *vaddrp, size_t size, voff_t offset,
    vm_protection_t initial_protection, vm_protection_t max_protection,
    enum vm_vad_inheritance inheritance, bool exact);

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

/*! @brief Allocate a new anonymous section and charge \p vmps for it.*/
int vm_section_new_anonymous(vm_procstate_t *vmps, size_t size,
    vm_section_t **out);

extern kspinlock_t vmp_pfn_lock;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_KDK_VM_H */
