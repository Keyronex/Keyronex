/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Wed Apr 17 2024.
 */
/*!
 * @file uacpi.m
 * @brief uACPI kernel API implementation.
 */

#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include <limine.h>
#include <uacpi/kernel_api.h>

#include "dev/pci/ecam.h"
#include "vm/vmp.h"

extern struct limine_rsdp_request rsdp_request;

#if defined(__amd64__)
#include <kdk/amd64/portio.h>

uint8_t
pci_readb(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	return inb(0xCFC + (offset & 3));
}

uint16_t
pci_readw(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	return inw(0xCFC + (offset & 3));
}

uint32_t
pci_readl(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	return inl(0xCFC + (offset & 3));
}

void
pci_writeb(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint8_t value)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	outb(0xCFC + (offset & 3), value);
}

void
pci_writew(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint16_t value)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	outw(0xCFC + (offset & 3), value);
}

void
pci_writel(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint32_t value)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	outl(0xCFC + (offset & 3), value);
}

#else

#define PCI_CONFIG_OFFSET(slot, function, offset) \
	((slot << 15) | (function << 12) | (offset))

uint8_t
pci_readb(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	return *(volatile uint8_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset));
}

uint16_t
pci_readw(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	return *(volatile uint16_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset));
}

uint32_t
pci_readl(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	return *(volatile uint32_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset));
}

void
pci_writeb(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint8_t value)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	*(volatile uint8_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset)) = value;
}

void
pci_writew(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint16_t value)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	*(volatile uint16_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset)) = value;
}

void
pci_writel(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint32_t value)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	*(volatile uint32_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset)) = value;
}

#endif

void
uacpi_kernel_vlog(enum uacpi_log_level lvl, const char *msg, uacpi_va_list va)
{
	const char *lvlStr;

	switch (lvl) {
	case UACPI_LOG_DEBUG:
		lvlStr = "debug";
		break;
	case UACPI_LOG_TRACE:
		lvlStr = "trace";
		break;
	case UACPI_LOG_INFO:
		lvlStr = "info";
		break;
	case UACPI_LOG_WARN:
		lvlStr = "warn";
		break;
	case UACPI_LOG_ERROR:
		lvlStr = "error";
		break;
	default:
		lvlStr = "<invalid>";
	}

	if (lvl <= UACPI_LOG_INFO) {
		kprintf("uacpi %s: ", lvlStr);
		kvpprintf(msg, va);
	}
}

void
uacpi_kernel_log(uacpi_log_level lvl, const uacpi_char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);

	uacpi_kernel_vlog(lvl, fmt, va);

	va_end(va);
}

void *
uacpi_kernel_alloc(uacpi_size size)
{
	return kmem_alloc(size);
}

void *
uacpi_kernel_calloc(uacpi_size count, uacpi_size size)
{
	return kmem_zalloc(count * size);
}

void
uacpi_kernel_free(void *ptr, uacpi_size size)
{
	if (ptr == NULL)
		return;
	return kmem_free(ptr, size);
}

void *
uacpi_kernel_map(uacpi_phys_addr physical, uacpi_size length)
{
	paddr_t paddr = PGROUNDDOWN(physical);
	size_t offset = physical & (PGSIZE - 1);
	size_t vsize = PGROUNDUP(offset + length);
	vaddr_t vaddr;
	int r;

#if TRACE_UACPI_MAPPINGS
	kprintf("mapping 0x%zx length 0x%lx\n", physical, length);
#endif

	r = vm_ps_map_physical_view(&kernel_procstate, &vaddr, vsize, paddr,
	    kVMRead | kVMWrite, kVMRead | kVMWrite, false);
	kassert(r == 0);

	return (void *)(vaddr + offset);
}

void
uacpi_kernel_unmap(void *ptr, uacpi_size length)
{
}

_Static_assert(sizeof(uacpi_pci_address) <= sizeof(uacpi_handle),
    "uacpi_pci_address must fit in uacpi_handle");

uacpi_status
uacpi_kernel_pci_device_open(uacpi_pci_address address,
    uacpi_handle *out_handle)
{
	kassert(sizeof(uacpi_pci_address) <= sizeof(uacpi_handle));
	memcpy(out_handle, &address, sizeof(address));
	return UACPI_STATUS_OK;
}

void
uacpi_kernel_pci_device_close(uacpi_handle)
{
	/* epsilon */
}

uacpi_status
uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset, uacpi_u8 *value)
{
	uacpi_pci_address address;
	memcpy(&address, &device, sizeof(address));
	*value = pci_readb(address.segment, address.bus, address.device,
	    address.function, offset);
	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset,
    uacpi_u16 *value)
{
	uacpi_pci_address address;
	memcpy(&address, &device, sizeof(address));
	*value = pci_readw(address.segment, address.bus, address.device,
	    address.function, offset);
	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset,
    uacpi_u32 *value)
{
	uacpi_pci_address address;
	memcpy(&address, &device, sizeof(address));
	*value = pci_readl(address.segment, address.bus, address.device,
	    address.function, offset);
	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset, uacpi_u8 value)
{
	uacpi_pci_address address;
	memcpy(&address, &device, sizeof(address));
	pci_writeb(address.segment, address.bus, address.device,
	    address.function, offset, value);
	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset,
    uacpi_u16 value)
{
	uacpi_pci_address address;
	memcpy(&address, &device, sizeof(address));
	pci_writew(address.segment, address.bus, address.device,
	    address.function, offset, value);
	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset,
    uacpi_u32 value)
{
	uacpi_pci_address address;
	memcpy(&address, &device, sizeof(address));
	pci_writel(address.segment, address.bus, address.device,
	    address.function, offset, value);
	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
    uacpi_handle *out_handle)
{
	*out_handle = (uacpi_handle)base;
	return UACPI_STATUS_OK;
}

void
uacpi_kernel_io_unmap(uacpi_handle)
{
	/* epsilon */
}

uacpi_status
uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset,
    uacpi_u8 *out_value)
{
#if defined(__amd64__)
	*out_value = inb((uintptr_t)handle + offset);
	return UACPI_STATUS_OK;
#else
	kfatal("IO operations not supported on this architecture");
	return UACPI_STATUS_UNIMPLEMENTED;
#endif
}

uacpi_status
uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset,
    uacpi_u16 *out_value)
{
#if defined(__amd64__)
	*out_value = inw((uintptr_t)handle + offset);
	return UACPI_STATUS_OK;
#else
	kfatal("IO operations not supported on this architecture");
	return UACPI_STATUS_UNIMPLEMENTED;
#endif
}

uacpi_status
uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset,
    uacpi_u32 *out_value)
{
#if defined(__amd64__)
	*out_value = inl((uintptr_t)handle + offset);
	return UACPI_STATUS_OK;
#else
	kfatal("IO operations not supported on this architecture");
	return UACPI_STATUS_UNIMPLEMENTED;
#endif
}

uacpi_status
uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset,
    uacpi_u8 in_value)
{
#if defined(__amd64__)
	outb((uintptr_t)handle + offset, in_value);
	return UACPI_STATUS_OK;
#else
	kfatal("IO operations not supported on this architecture");
	return UACPI_STATUS_UNIMPLEMENTED;
#endif
}

uacpi_status
uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset,
    uacpi_u16 in_value)
{
#if defined(__amd64__)
	outw((uintptr_t)handle + offset, in_value);
	return UACPI_STATUS_OK;
#else
	kfatal("IO operations not supported on this architecture");
	return UACPI_STATUS_UNIMPLEMENTED;
#endif
}

uacpi_status
uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset,
    uacpi_u32 in_value)
{
#if defined(__amd64__)
	outl((uintptr_t)handle + offset, in_value);
	return UACPI_STATUS_OK;
#else
	kfatal("IO operations not supported on this architecture");
	return UACPI_STATUS_UNIMPLEMENTED;
#endif
}

uacpi_u64
uacpi_kernel_get_nanoseconds_since_boot(void)
{
	return ke_get_nanos();
}

void
uacpi_kernel_stall(uacpi_u8 usec)
{
	kfatal("Implement me\n");
}

void
uacpi_kernel_sleep(uacpi_u64 msec)
{
	kevent_t nothing;
	ke_event_init(&nothing, false);
	ke_wait(&nothing, "uacpi_kernel_sleep", false, false, msec * 1000000);
}

struct uacpi_interrupt {
	struct intr_entry entry;
	uacpi_interrupt_handler handler;
	uacpi_handle ctx;
};

static bool
uacpi_intr_handler(md_intr_frame_t *frame, void *context)
{
	struct uacpi_interrupt *intr = context;
	return intr->handler(intr->ctx);
}

uacpi_status
uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,
    uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle)
{
#if 0
	int r;
	struct uacpi_interrupt *entry;
	dk_interrupt_source_t source;

	entry = kmem_alloc(sizeof(*entry));
	entry->handler = handler;
	entry->ctx = ctx;

#ifdef __amd64__
	source.id = isa_intr_overrides[irq].gsi;
	source.edge = isa_intr_overrides[irq].edge;
	source.low_polarity = isa_intr_overrides[irq].lopol;

	r = [IOApic handleSource:&source
		     withHandler:uacpi_intr_handler
			argument:entry
		      atPriority:kIPLHigh
			   entry:&entry->entry];
#else
	(void)source;
	(void)uacpi_intr_handler;
	kprintf("uacpi_install_interrupt_handler\n");
	r = 0;
#endif

	kassert (r == 0);

	*out_irq_handle = ctx;
#endif

	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler, uacpi_handle)
{
	kfatal("Implement me\n");
}

uacpi_handle
uacpi_kernel_create_spinlock(void)
{
	void *lock = kmem_alloc(sizeof(kspinlock_t));
	ke_spinlock_init(lock);
	return lock;
}

void
uacpi_kernel_free_spinlock(uacpi_handle handle)
{
	kmem_free(handle, sizeof(kspinlock_t));
}

uacpi_cpu_flags
uacpi_kernel_lock_spinlock(uacpi_handle handle)
{
	return ke_spinlock_acquire(handle);
}

void
uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags ipl)
{
	ke_spinlock_release(handle, ipl);
}

typedef struct alloc_cache {
	void *objects[16];
} alloc_cache_t;

uacpi_status
uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler,
    uacpi_handle ctx)
{
	kfatal("Implement me\n");
}

uacpi_status
uacpi_kernel_wait_for_work_completion(void)
{
	kfatal("Implement me\n");
}

uacpi_status
uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req)
{
	kfatal("Implement me\n");
}

uacpi_handle
uacpi_kernel_create_mutex(void)
{
	kmutex_t *mutex = kmem_alloc(sizeof(kmutex_t));
	ke_mutex_init(mutex);
	return mutex;
}

void
uacpi_kernel_free_mutex(uacpi_handle opaque)
{
	kmem_free(opaque, sizeof(kmutex_t));
}

uacpi_status
uacpi_kernel_acquire_mutex(uacpi_handle opaque, uacpi_u16 timeout)
{
	kmutex_t *mutex = opaque;
	kwaitresult_t w = ke_wait(mutex, "uacpi_kernel_acquire_mutex", false,
	    false, timeout == 0xffff ? -1 : (nanosecs_t)timeout * NS_PER_S);
	kassert(w == kKernWaitStatusOK || w == kKernWaitStatusTimedOut);
	return w == kKernWaitStatusOK ? UACPI_STATUS_OK : UACPI_STATUS_TIMEOUT;
}

void
uacpi_kernel_release_mutex(uacpi_handle opaque)
{
	kmutex_t *mutex = opaque;
	ke_mutex_release(mutex);
}

uacpi_handle
uacpi_kernel_create_event(void)
{
	ksemaphore_t *semaphore = kmem_alloc(sizeof(ksemaphore_t));
	ke_semaphore_init(semaphore, 0);
	return semaphore;
}

void
uacpi_kernel_free_event(uacpi_handle opaque)
{
	kmem_free(opaque, sizeof(ksemaphore_t));
}

uacpi_thread_id
uacpi_kernel_get_thread_id(void)
{
	return curthread();
}

uacpi_bool
uacpi_kernel_wait_for_event(uacpi_handle opaque, uacpi_u16 timeout)
{
	ksemaphore_t *semaphore = opaque;
	kwaitresult_t w = ke_wait(semaphore, "uacpi_kernel_wait_for_event",
	    false, false,
	    timeout == 0xffff ? -1 : (nanosecs_t)timeout * NS_PER_S);
	kassert(w == kKernWaitStatusOK || w == kKernWaitStatusTimedOut);
	return w == kKernWaitStatusOK ? true : false;
}

void
uacpi_kernel_signal_event(uacpi_handle opaque)
{
	ksemaphore_t *semaphore = opaque;
	ke_semaphore_release(semaphore, 1);
}

void
uacpi_kernel_reset_event(uacpi_handle opaque)
{
	ksemaphore_t *semaphore = opaque;
	ke_semaphore_reset(semaphore, 0);
}

uacpi_status
uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rdsp_address)
{
	*out_rdsp_address = V2P(rsdp_request.response->address);
	return UACPI_STATUS_OK;
}
