#include <limine.h>
#include <stdint.h>

#include "kdk/kmem.h"
#include "kdk/nanokern.h"
#include "kdk/object.h"
#include "nanokern/aarch64/cpu.h"
#include "vm/vmp.h"

static volatile struct limine_dtb_request dtb_request = {
	.id = LIMINE_DTB_REQUEST,
	.revision = 0
};

static volatile struct limine_hhdm_request hhdm_request = {
	.id = LIMINE_HHDM_REQUEST,
	.revision = 0
};

static volatile struct limine_kernel_address_request kernel_address_request = {
	.id = LIMINE_KERNEL_ADDRESS_REQUEST,
	.revision = 0
};

static volatile struct limine_memmap_request memmap_request = {
	.id = LIMINE_MEMMAP_REQUEST,
	.revision = 0
};

volatile struct limine_rsdp_request rsdp_request = { .id = LIMINE_RSDP_REQUEST,
	.revision = 0 };

static volatile struct limine_smp_request smp_request = {
	.id = LIMINE_SMP_REQUEST,
	.revision = 0
};

volatile uint8_t *uart = (uint8_t *)0x09000000;
kspinlock_t pac_console_lock = KSPINLOCK_INITIALISER;
kcpu_t bootstrap_cpu;
struct kthread thread0;

void
pac_putc(int c, void *ctx)
{
	if (c == '\n')
		*uart = '\r';
	*uart = c;
}

ipl_t
splraise(ipl_t ipl)
{
	return kIPL0;
}

void
splx(ipl_t ipl)
{
	(void)ipl;
}

ipl_t
splget(void)
{
	return kIPL0;
}

void
md_raise_dpc_interrupt(void)
{
	curcpu()->dpc_int = true;
}

void md_cpu_init(kcpu_t *cpu)
{

}

void
md_switch(kthread_t *old_thread)
{
	kfatal("Implement this\n");
}

void ke_thread_init_context(kthread_t *thread, void (*func)(void *), void *arg)
{
	kfatal("Implement me\n");
}

#if 0
void vmp_md_enter_kwired(void)
{
	kfatal("Implement this\n");
}
#endif

void
vmp_md_unenter_kwired(void)
{
	kfatal("Implement this\n");
}

void
_start()
{
	kprintf("Keyronex-lite/aarch64: " __DATE__ " " __TIME__ "\n");

	if (hhdm_request.response->offset != HHDM_BASE) {
		/* we expect HHDM begins there for now for simplicity */
		kprintf("Unexpected HHDM offset (assumes 0xffff800000000000, "
			"actual 0x%zx)",
		    hhdm_request.response->offset);
		hcf();
	}

	if (kernel_address_request.response->virtual_base != KERN_BASE) {
		kprintf("Unexpected kernel virtual base %zx",
		    kernel_address_request.response->virtual_base);
		hcf();
	}

	if (dtb_request.response != NULL)
		kprintf("DTB at %p\n", dtb_request.response->dtb_ptr);
	else if (rsdp_request.response != NULL)
		kprintf("RSDP at %p\n", rsdp_request.response->address);
	else
		kprintf("neither ACPI nor DTB\n");

	curcpu()->cpucb.ipl = kIPL0;
	curcpu()->dpc_int = false;
	curcpu()->dpc_lock = (kspinlock_t)KSPINLOCK_INITIALISER;
	TAILQ_INIT(&curcpu()->dpc_queue);
	curcpu()->nanos = 0;
	curcpu()->reschedule_reason = kRescheduleReasonNone;
	curcpu()->idle_thread = &thread0;
	curcpu()->curthread = &thread0;
	TAILQ_INIT(&curcpu()->timer_queue);

	thread0.last_cpu = &bootstrap_cpu;
	thread0.state = kThreadStateRunning;

	struct limine_memmap_entry **entries = memmap_request.response->entries;

	for (int i = 0; i < memmap_request.response->entry_count; i++) {
		if (entries[i]->type != LIMINE_MEMMAP_USABLE)
			continue;

		vm_region_add(entries[i]->base, entries[i]->length);
	}

	vmp_kernel_init();
	kmem_init();
	obj_init();

	struct id_aa64pfr0_el1  pfr = read_id_aa64pfr0_el1();
	void intr_setup(void);
	intr_setup();
	*(char*)0xdeaddeafbeef = '\b';
	void ddk_init(void), ddk_autoconf(void);
	ddk_init();
	ddk_autoconf();

	for (;;)
		;
}
