#include <limine.h>
#include <stddef.h>
#include <stdint.h>

#include "amd64.h"
#include "executive/ex_private.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "kernel/ke_internal.h"
#include "process/psp.h"
#include "vm/vm_internal.h"

struct winsize;

enum { kPortCOM1 = 0x3f8 };

/* apic.c */
void lapic_enable(uint8_t spurvec);
uint32_t lapic_timer_calibrate(void);

/* cpu.c */
void setup_cpu_gdt(kcpu_t *cpu);

/* intr.c */
void idt_load(void);
void idt_setup(void);

/* rtc.c */
void read_rtc();

/* vmamd64.c */
void pmap_kernel_init(void);

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

volatile struct limine_terminal_request terminal_request = {
	.id = LIMINE_TERMINAL_REQUEST,
	.revision = 0
};

const unsigned logow = 74, logoh = 22;
const unsigned logosize = logow * logoh * 4;
extern const char logosmall[6512];
void (*syscon_puts)(const char *buf, size_t len) = NULL;
void (*syscon_printstats)(void) = NULL;
void (*syscon_getsize)(struct winsize *winsize) = NULL;
void (*syscon_getfbinfo)(struct fb_var_screeninfo *var,
    struct fb_fix_screeninfo *fix) = NULL;
void (*syscon_inhibit)(void) = NULL;

static void
done(void)
{
	kdprintf("Done.\n");
	for (;;) {
		__asm__("hlt");
	}
}

void
draw_logo(void)
{
	struct limine_framebuffer *fbs =
	    terminal_request.response->terminals[0]->framebuffer;
	size_t fbw = fbs->width;
	uint8_t *fb = fbs->address;

#define fb_at(x, y) &fb[y * 4 * fbw + x * 4];
	size_t startx = 0, starty = 0;
	size_t x = startx, y = starty;
	for (unsigned i = 0; i < sizeof(logosmall); i += 4) {
		if (i % (logow * 4) == 0) {
			y++;
			x = startx;
		}

		uint8_t *fba = (uint8_t *)fb_at(x, y);
		uint8_t *pica = (uint8_t *)&logosmall[i];

		*fba++ = *(pica + 2);
		*fba++ = *(pica + 0);
		*fba++ = *(pica + 1);
		*fba++ = *(pica + 3);

		x++;
	}
}

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

/*! doesn't strictly belong here */
struct kmsgbuf {
	char buf[4096];
	size_t read, write;
} kmsgbuf;

/* put character to limine terminal + COM1 */
void
hl_dputc(int ch, void *ctx)
{
	/* put to com1 */
	while (!(inb(kPortCOM1 + 5) & 0x20))
		;
	outb(kPortCOM1, ch);
}

void
hl_computc(int ch, void *ctx)
{
	/* put on kmsgbuf */
	kmsgbuf.buf[kmsgbuf.write++] = ch;
	if (kmsgbuf.write >= 4096)
		kmsgbuf.write = 0;
	if (kmsgbuf.read == kmsgbuf.write && ++kmsgbuf.read == kmsgbuf.write)
		kmsgbuf.read = 0;

	hl_dputc(ch, ctx);

	hl_scputc(ch, ctx);
}

void
hl_scputc(int ch, void *ctx)
{
	/* put to syscon/limine terminal */
	if (!syscon_puts) {
		struct limine_terminal *terminal =
		    terminal_request.response->terminals[0];
		terminal_request.response->write(terminal, (char *)&ch, 1);
	} else {
		syscon_puts((char *)&ch, 1);
	}
}

void
hl_replaykmsgbuf(void)
{
	for (size_t i = kmsgbuf.read; i != kmsgbuf.write; i++) {
		hl_scputc(kmsgbuf.buf[i % sizeof(kmsgbuf.buf)], NULL);
	}
}

static void
mem_init()
{
	if (hhdm_request.response->offset != 0xffff800000000000) {
		/* we expect HHDM begins there for now for simplicity */
		kdprintf("Unexpected HHDM offset (assumes 0xffff800000000000, "
			 "actual %lx",
		    hhdm_request.response->offset);
		done();
	}

	if (kernel_address_request.response->virtual_base !=
	    0xffffffff80000000) {
		kdprintf("Unexpected kernel virtual base %lx",
		    kernel_address_request.response->virtual_base);
		done();
	}

	vm_pdaemon_init();

	struct limine_memmap_entry **entries = memmap_request.response->entries;

	for (int i = 0; i < memmap_request.response->entry_count; i++) {
		if (entries[i]->type != 0 || entries[i]->base < 0x100000)
			continue;

		vmp_region_add(entries[i]->base, entries[i]->length);
	}

	kprintf("Available memory: %luMiB\n",
	    vmstat.ntotal * PGSIZE / 1024 / 1024);
}

/* can't rely on mutexes until scheduling is up (and in any case not in idle
 * thread), so this must be used instead */
static kspinlock_t early_lock = KSPINLOCK_INITIALISER;
static int cpus_up = 0;

static void
common_init(struct limine_smp_info *smpi)
{
	kcpu_t *cpu = (kcpu_t *)smpi->extra_argument;
	kthread_t *thread = cpu->current_thread;
	char *name;

	idt_load();
	cpu->hl.lapic_base = rdmsr(kAMD64MSRAPICBase);
	lapic_enable(0xff);

	/* guard allocations */
	ke_spinlock_acquire_nospl(&early_lock);
	kmem_asprintf(&name, "idle thread *cpu %d)", cpu->num);
	ki_thread_common_init(thread, cpu, &kernel_process.kproc, name);
	ke_spinlock_release_nospl(&early_lock);
	thread->state = kThreadStateRunning;

	setup_cpu_gdt(cpu);

	/* measure thrice and average it */
	cpu->hl.lapic_tps = 0;
	cpu->hl.lapic_id = smpi->lapic_id;
	for (int i = 0; i < 3; i++)
		cpu->hl.lapic_tps += lapic_timer_calibrate() / 3;

	TAILQ_INIT(&cpu->runqueue);
	TAILQ_INIT(&cpu->timer_queue);
	TAILQ_INIT(&cpu->dpc_queue);
	TAILQ_INIT(&cpu->thread_free_queue);
	cpu->dpc_int = false;
	cpu->reschedule_reason = kRescheduleReasonNone;
	cpu->idle_thread = thread;

	cpu->thread_free_dpc.arg = cpu;
	cpu->thread_free_dpc.callback = ki_do_thread_free_queue;
	cpu->thread_free_dpc.state = kDPCUnbound;

	/* enable SSE and SSE2 */
	uint64_t cr0 = read_cr0();
	cr0 &= ~((uint64_t)1 << 2);
	cr0 |= (uint64_t)1 << 1;
	write_cr0(cr0);

	uint64_t cr4 = read_cr4();
	cr4 |= (uint64_t)3 << 9;
	write_cr4(cr4);

	asm("sti");
	__atomic_add_fetch(&cpus_up, 1, __ATOMIC_RELAXED);
	hl_clock_start();
}

static void
ap_init(struct limine_smp_info *smpi)
{
	kcpu_t *cpu = (kcpu_t *)smpi->extra_argument;

	/* set it up immediately to avoid problems */
	wrmsr(kAMD64MSRGSBase, (uintptr_t)&all_cpus[cpu->num]);

	common_init(smpi);
	/* this is now that CPU's idle thread loop */
	done();
}

static void
smp_init()
{
	struct limine_smp_response *smpr = smp_request.response;

	all_cpus = kmem_alloc(sizeof(kcpu_t *) * smpr->cpu_count);

	kprintf("%lu cpus\n", smpr->cpu_count);
	ncpus = smpr->cpu_count;

	for (size_t i = 0; i < smpr->cpu_count; i++) {
		struct limine_smp_info *smpi = smpr->cpus[i];

		if (smpi->lapic_id == smpr->bsp_lapic_id) {
			smpi->extra_argument = (uint64_t)&cpu_bsp;
			cpu_bsp.num = i;
			all_cpus[i] = &cpu_bsp;
			common_init(smpi);
		} else {
			kcpu_t *cpu = kmem_alloc(sizeof *cpu);
			kthread_t *thread = kmem_alloc(sizeof *thread);

			cpu->num = i;
			cpu->current_thread = thread;
			all_cpus[i] = cpu;

			smpi->extra_argument = (uint64_t)cpu;
			smpi->goto_address = ap_init;
		}
	}

	while (cpus_up != smpr->cpu_count)
		__asm__("pause");
}

// The following will be our kernel's entry point.
void
_start(void)
{
	void *pcpu0 = &cpu_bsp;

	wrmsr(kAMD64MSRGSBase, (uint64_t)&pcpu0);
	cpu_bsp.current_thread = (kthread_t *)&kernel_bsp_thread;
	kernel_bsp_thread.kthread.cpu = pcpu0;
	kernel_bsp_thread.kthread.process = &kernel_process.kproc;
	serial_init();

	// Ensure we got a terminal
	if (terminal_request.response == NULL ||
	    terminal_request.response->terminal_count < 1) {

		done();
	}

	draw_logo();

	kprintf("Keyronex Version 0.7-alpha: " __TIMESTAMP__ "\n");

	idt_setup();
	mem_init();
	pmap_kernel_init();
	vmp_kernel_init();
	kmem_init();

	smp_init();
	read_rtc();

	psp_init_0();
	ex_init(rsdp_request.response->address);

	// We're done, just hang...
	done();
}
