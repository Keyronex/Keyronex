/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file uacpi.m
 * @brief uACPI dependencies.
 */

#include <sys/k_log.h>
#include <sys/k_cpu.h>
#include <sys/kmem.h>
#include <sys/k_thread.h>
#include <sys/limine.h>
#include <sys/vm.h>

#include <libkern/lib.h>

#include <uacpi/kernel_api.h>

#if defined(__amd64)
#include "asm/io.h"
#endif

extern __attribute__((section(".requests")))
volatile struct limine_rsdp_request rsdp_request;

extern int kern_initlevel;

uacpi_status
uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rdsp_address)
{
	*out_rdsp_address = v2p((vaddr_t)rsdp_request.response->address);
	return UACPI_STATUS_OK;
}

static uint32_t pci_address(uacpi_pci_address *address, uint32_t offset)
{
	return (address->bus << 16) | (address->device << 11) |
	    (address->function << 8) | (offset & ~(uint32_t)(3)) | 0x80000000;
}

uacpi_status
uacpi_kernel_pci_read(uacpi_pci_address *address, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 *value)
{
	outl(0xCF8, pci_address(address, offset));
	switch (byte_width) {
	case 1:
		*value = inb(0xCFC + (offset & 3));
		return UACPI_STATUS_OK;
	case 2:
		*value = inw(0xCFC + (offset & 3));
		return UACPI_STATUS_OK;
	case 4:
		*value = inl(0xCFC + (offset & 3));
		return UACPI_STATUS_OK;
	default:
		kfatal("no");
	}
}

uacpi_status
uacpi_kernel_pci_write(uacpi_pci_address *address, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 value)
{
	outl(0xCF8, pci_address(address, offset));
	switch (byte_width) {
	case 1:
		outb(0xCFC + (offset & 3), value);
		return UACPI_STATUS_OK;
	case 2:
		outw(0xCFC + (offset & 3), value);
		return UACPI_STATUS_OK;
	case 4:
		outl(0xCFC + (offset & 3), value);
		return UACPI_STATUS_OK;
	default:
		kfatal("no");
	}
	return UACPI_STATUS_OK;
	;
}

uacpi_status
uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
    uacpi_handle *out_handle)
{
	*out_handle = (uacpi_handle)base;
	return UACPI_STATUS_OK;
}

void
uacpi_kernel_io_unmap(uacpi_handle handle)
{
	kfatal("Implement me\n");
}

/* fixme */
static bool
vm_paddr_is_ram(paddr_t)
{
	return true;
}

void *
uacpi_kernel_map(uacpi_phys_addr pa, uacpi_size len)
{
	if (kern_initlevel < 1) {
		return (void *)p2v(pa);
	} else {
		paddr_t paddr = rounddown2(pa, PGSIZE);
		size_t offset = pa & (PGSIZE - 1);
		size_t vsize = roundup2(offset + len, PGSIZE);
		vaddr_t vaddr;
		int r;
		r = vm_k_map_phys(&vaddr, paddr, vsize,
		    vm_paddr_is_ram(pa) ? kCacheModeDefault :
#if defined(__amd64__)
					  kCacheModeUC
#elif defined(__riscv)
#if 0 /* broken, qemu no svpbmt? */
					  kCacheModeNC
#endif
					  kCacheModeDefault
#endif
		);
		kassert(r == 0);
		return (void *)vaddr + offset;
	}
}

void
uacpi_kernel_unmap(void *addr, uacpi_size len)
{

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
uacpi_kernel_free(void *mem, uacpi_size size_hint)
{
	kmem_free(mem, size_hint);
}

void
uacpi_kernel_log(uacpi_log_level level, const uacpi_char *msg)
{
	kdprintf("[uacpi]: %s", msg);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
	return ke_time();
}

void
uacpi_kernel_stall(uacpi_u8 usec)
{
	kfatal("Implement me\n");
}

void
uacpi_kernel_sleep(uacpi_u64 msec)
{
	kfatal("Implement me\n");
}

uacpi_handle
uacpi_kernel_create_mutex(void)
{
	kmutex_t *mtx = kmem_alloc(sizeof(kmutex_t));
	ke_mutex_init(mtx);
	return mtx;
}

void
uacpi_kernel_free_mutex(uacpi_handle mutex)
{
	kmem_free(mutex, sizeof(kmutex_t));
}

uacpi_handle
uacpi_kernel_create_event(void)
{
	kevent_t *event = kmem_alloc(sizeof(kevent_t));
	ke_event_init(event, false);
	return event;
}

void
uacpi_kernel_free_event(uacpi_handle event)
{
	kmem_free(event, sizeof(kevent_t));
}

uacpi_thread_id
uacpi_kernel_get_thread_id(void)
{
	return ke_curthread();
}

uacpi_status
uacpi_kernel_acquire_mutex(uacpi_handle mutex, uacpi_u16 timeout)
{
	if (timeout == 0x0) {
		return ke_mutex_tryenter(mutex) ? UACPI_STATUS_OK :
						  UACPI_STATUS_TIMEOUT;
	} else {
		ke_mutex_enter(mutex);
		return UACPI_STATUS_OK;
	}
}

void
uacpi_kernel_release_mutex(uacpi_handle mutex)
{
	return ke_mutex_exit(mutex);
}

uacpi_bool
uacpi_kernel_wait_for_event(uacpi_handle, uacpi_u16)
{
	kfatal("Implement me\n");
}

void
uacpi_kernel_signal_event(uacpi_handle)
{
	kfatal("Implement me\n");
}

void
uacpi_kernel_reset_event(uacpi_handle)
{
	kfatal("Implement me\n");
}

uacpi_status
uacpi_kernel_handle_firmware_request(uacpi_firmware_request *)
{
	kfatal("Implement me\n");
}

uacpi_status
uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler,
    uacpi_handle ctx, uacpi_handle *out_irq_handle)
{
	kdprintf("uacpi_kernel_install_interrupt_handler: irq=%u\n", irq);
	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler,
    uacpi_handle irq_handle)
{
	kfatal("Implement me\n");
}

uacpi_handle
uacpi_kernel_create_spinlock(void)
{
	kspinlock_t *lock = kmem_alloc(sizeof(kspinlock_t));
	ke_spinlock_init(lock);
	return lock;
}

void
uacpi_kernel_free_spinlock(uacpi_handle)
{
	kfatal("Implement me\n");
}

uacpi_cpu_flags
uacpi_kernel_lock_spinlock(uacpi_handle psl)
{
	return ke_spinlock_enter(psl);
}

void
uacpi_kernel_unlock_spinlock(uacpi_handle psl, uacpi_cpu_flags ipl)
{
	ke_spinlock_exit(psl, (ipl_t)ipl);
}

uacpi_status
uacpi_kernel_schedule_work(uacpi_work_type, uacpi_work_handler,
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
	uacpi_u64 u64;
	uacpi_status r;
	memcpy(&address, &device, sizeof(address));
	r = uacpi_kernel_pci_read(&address, offset, 1, &u64);
	*value = u64;
	return r;
}

uacpi_status
uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset,
    uacpi_u16 *value)
{
	uacpi_pci_address address;
	uacpi_u64 u64;
	uacpi_status r;
	memcpy(&address, &device, sizeof(address));
	r = uacpi_kernel_pci_read(&address, offset, 2, &u64);
	*value = u64;
	return r;
}

uacpi_status
uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset,
    uacpi_u32 *value)
{
	uacpi_pci_address address;
	uacpi_u64 u64;
	uacpi_status r;
	memcpy(&address, &device, sizeof(address));
	r = uacpi_kernel_pci_read(&address, offset, 4, &u64);
	*value = u64;
	return r;
}

uacpi_status
uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset, uacpi_u8 value)
{
	uacpi_pci_address address;
	memcpy(&address, &device, sizeof(address));
	return uacpi_kernel_pci_write(&address, offset, 1, value);
}

uacpi_status
uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset,
    uacpi_u16 value)
{
	uacpi_pci_address address;
	memcpy(&address, &device, sizeof(address));
	return uacpi_kernel_pci_write(&address, offset, 2, value);
}

uacpi_status
uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset,
    uacpi_u32 value)
{
	uacpi_pci_address address;
	memcpy(&address, &device, sizeof(address));
	return uacpi_kernel_pci_write(&address, offset, 4, value);
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
#endif
}
