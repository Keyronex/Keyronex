#include <limine.h>
#include <stddef.h>
#include <stdint.h>

#include "intr.h"
#include "kdk/amd64.h"
#include "kdk/amd64/portio.h"
#include "kdk/amd64/regs.h"
#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/nanokern.h"
#include "kdk/object.h"
#include "nanokern/ki.h"
#include "vm/vmp.h"
#include "ntcompat/ntcompat.h"
#include "executive/exp.h"

enum { kPortCOM1 = 0x3f8 };

/* autoconf.m */
void ddk_init(void);
void ddk_autoconf(void);
/* intr.c */
void intr_init(void);
/* misc.c */
void setup_cpu_gdt(kcpu_t *cpu);

struct kcpu bootstrap_cpu;
struct kthread thread0;
kspinlock_t pac_console_lock = KSPINLOCK_INITIALISER;

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent.

volatile struct limine_framebuffer_request framebuffer_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
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

static volatile struct limine_kernel_file_request kernel_file_request = {
	.id = LIMINE_KERNEL_FILE_REQUEST,
	.revision = 0
};

static volatile struct limine_memmap_request memmap_request = {
	.id = LIMINE_MEMMAP_REQUEST,
	.revision = 0
};

volatile struct limine_module_request module_request = {
	.id = LIMINE_MODULE_REQUEST,
	.revision = 0
};

volatile struct limine_rsdp_request rsdp_request = { .id = LIMINE_RSDP_REQUEST,
	.revision = 0 };

static volatile struct limine_smp_request smp_request = {
	.id = LIMINE_SMP_REQUEST,
	.revision = 0
};

struct ex_boot_config boot_config = {
	.root = "9p:trans=virtio,server=sysroot,aname=/"
};

static void
serial_init()
{
	outb(kPortCOM1 + 1, 0x00);
	outb(kPortCOM1 + 3, 0x80);
	outb(kPortCOM1 + 0, 0x03);
	outb(kPortCOM1 + 1, 0x00);
	outb(kPortCOM1 + 3, 0x03);
	outb(kPortCOM1 + 2, 0xC7);
	outb(kPortCOM1 + 4, 0x0B);
}

void
pac_putc(int ch, void *ctx)
{
	if (ch == '\n')
		pac_putc('\r', NULL);
	/* put to com1 */
	while (!(inb(kPortCOM1 + 5) & 0x20))
		;
	outb(kPortCOM1, ch);
}

/* can't rely on mutexes until scheduling is up (and in any case not in idle
 * thread), so this must be used instead */
static kspinlock_t early_lock = KSPINLOCK_INITIALISER;
static int cpus_up = 0;

static void
common_init(struct limine_smp_info *smpi)
{
	kcpu_t *cpu = (kcpu_t *)smpi->extra_argument;
	kthread_t *thread = cpu->curthread;
	char *name;

	idt_load();
	cpu->cpucb.lapic_base = rdmsr(kAMD64MSRAPICBase);
	lapic_enable(0xff);

	/* guard allocations */
	ke_spinlock_acquire_nospl(&early_lock);
	kmem_asprintf(&name, "idle thread *cpu %d)", cpu->num);
	ki_thread_common_init(thread, cpu, &kernel_process->kprocess, name);
	ke_spinlock_release_nospl(&early_lock);
	thread->state = kThreadStateRunning;

	setup_cpu_gdt(cpu);

	/* measure thrice and average it */
	cpu->cpucb.lapic_tps = 0;
	cpu->cpucb.lapic_id = smpi->lapic_id;
	for (int i = 0; i < 3; i++)
		cpu->cpucb.lapic_tps += lapic_timer_calibrate() / 3;

	ki_cpu_init(cpu, thread);

	/* enable SSE and SSE2 */
	uint64_t cr0 = read_cr0();
	cr0 &= ~((uint64_t)1 << 2);
	cr0 |= (uint64_t)1 << 1;
	write_cr0(cr0);

	uint64_t cr4 = read_cr4();
	cr4 |= (uint64_t)3 << 9;
	write_cr4(cr4);

	lapic_timer_start();
	__atomic_add_fetch(&cpus_up, 1, __ATOMIC_RELAXED);

	asm("sti");
}

static void
ap_init(struct limine_smp_info *smpi)
{
	kcpu_t *cpu = (kcpu_t *)smpi->extra_argument;

	write_cr3(kernel_process->vm->md.table);

	/* set it up immediately to avoid problems */
	wrmsr(kAMD64MSRGSBase, (uintptr_t)&cpus[cpu->num]);

	common_init(smpi);
	/* this is now that CPU's idle thread loop */
	hcf();
}

static void
smp_init()
{
	struct limine_smp_response *smpr = smp_request.response;

	cpus = kmem_alloc(sizeof(kcpu_t *) * smpr->cpu_count);

	kprintf("%lu cpus\n", smpr->cpu_count);
	ncpus = smpr->cpu_count;

	for (size_t i = 0; i < smpr->cpu_count; i++) {
		struct limine_smp_info *smpi = smpr->cpus[i];

		if (smpi->lapic_id == smpr->bsp_lapic_id) {
			smpi->extra_argument = (uint64_t)&bootstrap_cpu;
			bootstrap_cpu.num = i;
			cpus[i] = &bootstrap_cpu;
			common_init(smpi);
		} else {
			kcpu_t *cpu = kmem_alloc(sizeof *cpu);
			kthread_t *thread = kmem_alloc(sizeof *thread);

			cpu->num = i;
			cpu->curthread = thread;
			cpus[i] = cpu;

			smpi->extra_argument = (uint64_t)cpu;
			smpi->goto_address = ap_init;
		}
	}

	while (cpus_up != smpr->cpu_count)
		__asm__("pause");
}

// The following will be our kernel's entry point.
// If renaming _start() to something else, make sure to change the
// linker script accordingly.
void
_start(void)
{
	void *pcpu0 = &bootstrap_cpu;

	serial_init();

	npf_pprintf(pac_putc, NULL,
	    "Keyronex-lite/amd64 (" __DATE__ " " __TIME__ ")\r\n");

	/* set up initial threading structures */
	wrmsr(kAMD64MSRGSBase, (uint64_t)&pcpu0);
	ki_cpu_init(curcpu(), &thread0);
	thread0.last_cpu = &bootstrap_cpu;
	thread0.state = kThreadStateRunning;
	thread0.timeslice = 5;
	ki_thread_common_init(&thread0, &bootstrap_cpu, &kernel_process->kprocess, "idle0");

	intr_init();

	curcpu()->cpucb.lapic_base = rdmsr(kAMD64MSRAPICBase);
	lapic_enable(0xff);

	struct limine_memmap_entry **entries = memmap_request.response->entries;

	for (int i = 0; i < memmap_request.response->entry_count; i++) {
		if (entries[i]->type != LIMINE_MEMMAP_USABLE ||
		    entries[i]->base < 0x100000)
			continue;

		vm_region_add(entries[i]->base, entries[i]->length);
	}

	vmp_kernel_init();
	kmem_init();
	obj_init();
	smp_init();
	ntcompat_init();

	ps_create_kernel_thread(&ex_init_thread, "ex_init", ex_init, NULL);
	ke_thread_resume(ex_init_thread);

	/* idle loop */
	for (;;) {
		asm("hlt");
	}
}
