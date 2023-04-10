/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Feb 11 2023.
 */
/*!
 * @file vm.h
 * @brief Virtual Memory Manager public interface.
 *
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
	size_t nfree;
	size_t nactive;
	size_t ninactive;
	size_t nwired;
	size_t ntransitioning;
	size_t npermwired;
	size_t nvmm;
	size_t ndev;

	size_t ntotal;

	size_t nanon;
	size_t nobject;
};

enum vm_page_use {
	/*! The page is on the freelist. */
	kPageUseFree,
	/*! The page is used by an anon. */
	kPageUseAnonymous,
	/*! The page is used by an object. */
	kPageUseObject,
	/*! The page is used by kernel wired memory. */
	kPageUseWired,
	/*! The page is used by the VMM directly (wired) */
	kPageUseVMM,
	/*! The page is a device buffer. */
	kPageUseDevBuf,
};

enum vm_page_status {
	/*! The page is currently wired. */
	kPageStatusWired,
	/*! The page is on the active LRU queue. */
	kPageStatusActive,
	/*! The page is on the inactive LRU queue. */
	kPageStatusInactive,
	/*! The page is being moved into/out of memory. */
	kPageStatusBusy,
};

/*!
 * Physical page frame description.
 *
 * Most components are locked by the page owner - i.e. either anon or obj.
 * For kernel wired memory the page queues lock is used.
 *
 * (~) invariant forever
 * (o) owner obj/anon mutex
 * (q) page queues lock
 */
typedef struct vm_page {
	/*! (q) Queue linkage: freelist or LRU queue. */
	TAILQ_ENTRY(vm_page) queue_entry;

	/*! (~) Page's physical page frame number. */
	paddr_t pfn : 40;
	/*! (o) What is its current use? */
	enum vm_page_use use : 3;
	/*! (o+sometimes q) What is its status, if use is anonymous/object? */
	enum vm_page_status status : 2;
	/*! (q) Is the page dirty? */
	bool dirty : 1;
	/* Padding */
	uint8_t padding : 2;
	/*!
	 * (o+q) How many requests are there for it to be wired in-memory?
	 * One for each MDL, and another for a busy page.
	 */
	uint16_t wirecnt;

	union {
		/*! Anonymous page, if ::is_anonymous. */
		struct vmp_anon *anon;
		/*! VNode object, if ::is_vnode */
		struct vm_object *obj;
	};

	/* (o) Physical-to-virtual mapping list */
	LIST_HEAD(pv_entry_list, pv_entry) pv_list;
} vm_page_t;

enum vm_inheritance {
	/*! Inherit a shared entry (though not necessarily writeable!) */
	kVMInheritShared,
	/*! Inherit a virtual copy. */
	kVMInheritCopy,
	/*! Special case for stacks - inherit only current thread's. */
	kVMInheritStack
};

struct vm_amap {
	kmutex_t mutex;
	struct vmp_amap_l3 *l3;
};

/*!
 * An entry in an address space map.
 *
 * These can contain either:
 * 1. A VM object only. (Used by mmap() with MAP_SHARED).
 * 2. An amap only. (Used by mmap() with MAP_ANONYMOUS)
 * 3. Both an amap and a VM object, with the anonymous memory "superimposed"
 *    over the VM object. (Used by mmap() with MAP_PRIVATE).
 *
 * Note that the VM object will always be a vnode, never an anonymous VM object.
 * Those are mapped by copying references to the vm_anons instead.
 */
typedef struct vm_map_entry {
	/*! Entry in vm_map::vad_rbtree */
	RB_ENTRY(vm_map_entry) rbtree_entry;

	/*! Start and end vitrual address. */
	vaddr_t start, end;
	/*! Offset into VM object. */
	voff_t offset;

	/*! VM object, if any. */
	struct vm_object *object;
	/*! Anonymous memory, if any. */
	struct vm_amap amap;
	/*! Whether it has anonymous layer. */
	bool has_anonymous;

	/*! Inheritance attributes for fork. */
	enum vm_inheritance inheritance;
	/*! Current protection of the region. */
	vm_protection_t protection;
	/*! Maximum protection of the region. Set at creation. */
	vm_protection_t max_protection;
} vm_map_entry_t;

/*! Per-process memory manager state. */
typedef struct vm_map {
	/*! Mapping tree lock lock. */
	kmutex_t mutex;
	/*! Map entry tree. */
	RB_HEAD(vm_map_entry_rbtree, vm_map_entry) entry_queue;
	/*! VMem allocator state. */
	vmem_t vmem;
	/*! Per-port VM state. */
	struct vm_map_md md;
} vm_map_t;

RB_HEAD(vmp_objpage_rbtree, vmp_objpage);

/*!
 * A mappable VM object. Either regular or anonymous.
 *
 * Note that in vnodes, this is embedded in the vnode structure (as its first
 * element.)
 */
typedef struct vm_object {
	object_header_t objhdr;
	bool is_anonymous;
	kmutex_t mutex;
	union {
		struct vm_amap amap;
		struct vmp_objpage_rbtree page_rbtree;
	};
} vm_object_t;

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
 * Page's wire count is initially set to one, and status to kPageStatusWired.
 *
 * @param[optional] map Process to charge for the allocation.
 * @param must Whether the allocaton must be served.
 * @param[out] out Allocated page's PFN db entry will be written here.
 * @param use What the page will be used for.
 *
 * @retval 0 Page allocated.
 * @retval 1 Low memory, drop locks and wait on the low-memory event.
 */
int vmp_page_alloc(vm_map_t *ps, bool must, enum vm_page_use use,
    vm_page_t **out);

int vmp_page_alloc_locked(vm_map_t *ps, bool must, enum vm_page_use use,
    vm_page_t **out);

/*!
 * @brief Free a physical page.
 *
 * Page's reference count must be 0.
 *
 * @pre PFN database lock held.
 */
void vmp_page_free(vm_map_t *ps, vm_page_t *page);

void vmp_page_free_locked(vm_map_t *map, vm_page_t *page);

/*! @brief Get the PFN database entry for a physical page address. */
vm_page_t *vmp_paddr_to_page(paddr_t paddr);

/*! @brief Copy the contents of one page to another. */
void vmp_page_copy(vm_page_t *from, vm_page_t *to);

/*! @brief Increment the wiring count of an in-core page. */
void vm_page_wire(vm_page_t *page);

/*! @brief Decrement wire-count of a resident page, may return to LRU if 0. */
void vm_page_unwire(vm_page_t *page);

/*! @brief Bring a page to the front of the LRU queue. */
void vm_page_activate(vm_page_t *page);

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

/*! @brief Get the physical address of an offset in an MDL. */
paddr_t vm_mdl_paddr(vm_mdl_t *mdl, voff_t off);

/*! @brief Allocated kernel wired pages and address space. */
vaddr_t vm_kalloc(size_t npages, vmem_flag_t flags);

/*! @brief Free kernel wired pages. */
void vm_kfree(vaddr_t addr, size_t npages, vmem_flag_t flags);

/*! @brief Activate a process' virtual address space. */
void vm_map_activate(vm_map_t *map);

/*! @brief Free a map. */
void vm_map_free(vm_map_t *map);

/*! @brief Handle a page fault. */
vm_fault_return_t vm_fault(vm_map_t *map, vaddr_t vaddr, vm_fault_flags_t flags,
    vm_page_t **out);

/*! @brief Initialise a new map. */
int vm_map_init(vm_map_t *map);

/*! @brief Allocate anonymous memory in a map. */
int vm_map_allocate(vm_map_t *ps, vaddr_t *vaddrp, size_t size, bool exact);

/*! @brief Deallocate a range of virtual address space in a map. */
int vm_map_deallocate(vm_map_t *map, vaddr_t start, size_t size);

/*!
 * @brief Map a view of an object into a map.
 */
int vm_map_object(vm_map_t *ps, vm_object_t *object, krx_inout vaddr_t *vaddrp,
    size_t size, voff_t offset, vm_protection_t initial_protection,
    vm_protection_t max_protection, enum vm_inheritance inheritance, bool exact,
    bool copy);

/*!
 * @brief Forks a virtual address space map.
 *
 * This function forks a process' virtual address space according to POSIX
 * semantics. It should be called only from the context of a thread of that
 * process. The existing process' virtual address space structure
 * \p map is used as a template to fill entries in a new map, the address of
 * which will be written to the out parameter \p map_new.
 *
 * Map entries are established in the new map which either share or virtual copy
 * (according to the map entry inheritance attributes) the memory referred to by
 * the corresponding map entry in the old process. The reason why this function
 * must be invoked from the context of a thread which already exists is because
 * only the stack of the invoking thread is copied.
 *
 * Parts of the old process' address space which have been virtual-copied will
 * be mapped read-only if they are currently mapped read-write.
 *
 * @pre The map lock of both \p map and \p map_new must be held, and
 * the PFN database lock must not be held.
 * @post The map locks are released.
 *
 * @param map The virtual address space structure of the existing process.
 * @param map_new Where to write the pointer to the newly-created virtual
 * address space structure.
 *
 * @return 0 on success, negative error code on failure
 */
int vm_map_fork(vm_map_t *map, vm_map_t **map_new);

/*! @brief Allocate a new anonymous object and charge \p map for it.*/
int vm_object_new_anonymous(vm_map_t *map, size_t size, vm_object_t **out);

extern kspinlock_t vmp_pfn_lock;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_KDK_VM_H */
