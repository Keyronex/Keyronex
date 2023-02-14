/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Sat Feb 11 2023.
 */
/*!
 * @file vm.h
 * @brief The memory manager's internal data structures.
 *
 * Working Set Lists
 * -----------------
 *
 * The implementation of these is based very closely on that of Mintia:
 * see https://github.com/xrarch/mintia
 *
 *
 *
 * Locking
 * -------
 *
 * The general principle: the PFN lock is big and covers everything done from
 * within an object down.
 *
 * The VAD list mutex protects process' trees of VAD lists.
 */

#ifndef MLX_VM_VM_H
#define MLX_VM_VM_H

#include <bsdqueue/queue.h>
#include <bsdqueue/tree.h>
#include <stdint.h>

#include "amd64/vm_md.h"
#include "ke/ke.h"
#include "vm/vmem_impl.h"

/*! Fault flags. For convenience, matches amd64 MMU. */
typedef enum vm_fault_flags {
	kMMFaultPresent = 1,
	kMMFaultWrite = 2,
	kMMFaultUser = 4,
	kMMFaultExecute = 16,
} vm_fault_flags_t;

/*!
 * Fault return values.
 */
typedef enum vm_fault_return {
	kMMFaultRetOK = 0,
	kMMFaultRetFailure = -1,
	kMMFaultRetPageShortage = -2,
	kMMFaultRetRetry = -3,
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

/*! Flags that may be passed to vm_kalloc(). */
enum vm_kalloc_flags {
	/*! immediately return NULL if no free pages currently */
	kVMKNoSleep = 0,
	/*! infallible; sleepwait for a page if no pages currently available */
	kVMKSleep = 1,
};

struct vm_stat {
	size_t npfndb;
	size_t nfree;
	size_t nmodified;
	size_t nactive;
	size_t ntransitioning;
	size_t nvmm;
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
	} use : 4;

	/*! Page's physical address. */
	paddr_t address;

	union {
		/*! Virtual page, if ::is_anonymous. */
		struct vm_vpage *vpage;
		/*! File, if ::is_file */
		struct vm_file *file;
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
	/*! physical page frame description if resident */
	vm_page_t *page;
	/*! swap descriptor, if it's been written */
	uintptr_t swapdesc;
	/*! How many section objects contain it? */
} vm_vpage_t;

/*!
 * Working-set list (or WSL). A modified ring buffer which can be grown or
 * shrunk, and employing a freelist so that cleared entries can be used.
 */
typedef struct vm_wsl {
	/*! Pointer to array of virtual addresses/freelist linkage. */
	uintptr_t *entries;
	/*! Actual size of the array. */
	size_t array_size;
	/*! Current maximum size of the working set, may be lower than
	 * array_size.*/
	size_t max_size;
	/*! Currently used number of entries (including cleared ones). */
	size_t cur_size;
	/*! Index of least recently inserted page. */
	size_t head;
	/*! Index of most recently inserted page */
	size_t tail;
	/*! Head of the freelist, if there is one. */
	uintptr_t **freelist_head;
} vm_wsl_t;

/*!
 * Virtual Address Descriptor - a mapping of a section object. Note that
 * copy-on-write is done at the section object level, not here.
 */
typedef struct vm_vad {
	/*! Entry in vm_procstate::vad_queue */
	TAILQ_ENTRY(vm_vad) vad_queue_entry;

	/*! Inheritance attributes for fork. */
	enum vm_vad_inheritance {
		/*! Inherit a shared entry (though not necessarily writeable!)
		 */
		kVADInheritShared,
		/*! Inherit a virtual copy. */
		kVADInheritCopy,
		/*! Special case for stacks - inherit only current thread's. */
		kVADInheritStack
	} inheritance;

	/*!
	 * Maximum protection of the region. (we may want to split out a current
	 * protection from this)
	 */
	vm_protection_t max_protection;
} vm_vad_t;

/*! Per-process memory manager state. */
typedef struct vm_procstate {
	/*! Process' working-set list. */
	vm_wsl_t wsl;
	/*! VAD queue lock. */
	kmutex_t mutex;
	/*! VAD queue. */
	TAILQ_HEAD(, vm_vad) vad_queue;
	/*! VMem allocator state. */
	vmem_t vmem;
	/*! Per-port VM state. */
	struct vm_ps_md md;
} vm_procstate_t;

extern struct vm_stat vmstat;

/*! @brief Acquire the PFN database lock. */
#define vi_acquire_pfn_lock() ke_spinlock_acquire(&vi_pfn_lock)

/*! @brief Release the PFN database lock. */
#define vi_release_pfn_lock(IPL) ke_spinlock_release(&vi_pfn_lock, IPL)

/*! @brief Add a region of physical memory to VMM management. */
void vi_region_add(paddr_t base, size_t length);

/*! @brief Initialise the kernel wired memory system. */
void vi_kernel_init(void);

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
int vi_page_alloc(vm_procstate_t *ps, bool must, enum vm_page_use use,
    vm_page_t **out);

/*!
 * @brief Free a physical page.
 *
 * Page's reference count must be 0.
 *
 * @pre PFN database lock held.
 */
void vi_page_free(vm_procstate_t *ps, vm_page_t *page);

/*! @brief Get the PFN database entry for a physical page address. */
vm_page_t *vi_paddr_to_page(paddr_t paddr);

/*! @brief Allocated kernel wired pages and address space. */
vaddr_t vm_kalloc(size_t npages, enum vm_kalloc_flags wait);

/*! @brief Free kernel wired pages. */
void vm_kfree(vaddr_t addr, size_t npages);

/*! @brief Activate a process' virtual address space. */
void vm_ps_activate(vm_procstate_t *vmps);

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

/*!
 * @brief Adds a virtual address to a working set list.
 *
 * This function adds a virtual address entry to the working set list.
 * If the working set list is below its maximal size, and there is no low-memory
 * condition, it will be appended.
 * Otherwise, if the working set list is at its maximum, it will try to expand
 * its size and add the entry.
 * If the expansion fails, the function will dispose of the least recently added
 * entry in the working set list and add the new entry in its place.
 *
 * @param ws Pointer to the process vm state.
 * @param entry The virtual address entry to add to the working set list.
 */
void mi_wsl_insert(vm_procstate_t *vmps, vaddr_t entry);

/**
 * @brief Trims a specified number of pages from a working set list.
 *
 * This function removes a specified number of pages, starting with the least
 * recently used, from a working set list. If the number of entries to be
 * trimmed is equal to the current size of the working set list, then all the
 * entries will be disposed.
 *
 * @param ws Pointer to the process vm state.
 * @param n Number of entries to be trimmed.
 */
void mi_wsl_trim_n_entries(vm_procstate_t *vmps, size_t n);

extern kspinlock_t vi_pfn_lock;

#endif /* MLX_VM_VM_H */
