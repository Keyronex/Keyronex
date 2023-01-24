#include <amd64/amd64.h>
#include <kern/kmem.h>
#include <kern/ksrv.h>
#include <vm/vm.h>

#include <limine.h>
#include <nanokern/kernmisc.h>
#include <nanokern/thread.h>
#include <stddef.h>
#include <stdint.h>

void setup_cpu_gdt(kcpu_t *cpu);
void idt_setup();
void idt_load();
void x64_vm_init(paddr_t kphys);

void	 lapic_enable();
uint32_t lapic_timer_calibrate();

#define kprintf nk_dbg

enum { kPortCOM1 = 0x3f8 };

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent.

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

static void
done(void)
{
	kprintf("Done!\n");
	for (;;) {
		__asm__("pause");
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

static void
mem_init()
{
	if (hhdm_request.response->offset != 0xffff800000000000) {
		/* we expect HHDM begins there for now for simplicity */
		kprintf("Unexpected HHDM offset (assumes 0xffff800000000000, "
			"actual %lx",
		    hhdm_request.response->offset);
		done();
	}

	if (kernel_address_request.response->virtual_base !=
	    0xffffffff80000000) {
		kprintf("Unexpected kernel virtual base %lx",
		    kernel_address_request.response->virtual_base);
		done();
	}

	struct limine_memmap_entry **entries = memmap_request.response->entries;

	for (int i = 0; i < memmap_request.response->entry_count; i++) {

		vm_pregion_t *bm = P2V((void *)entries[i]->base);
		size_t	      used; /* n bytes used by bitmap struct */
		int	      b;

		if (entries[i]->type != 0 || entries[i]->base < 0x100000)
			continue;

		/* set up a pregion for this area */
		bm->base = entries[i]->base;
		bm->npages = entries[i]->length / PGSIZE;

		used = ROUNDUP(sizeof(vm_pregion_t) +
			sizeof(vm_page_t) * bm->npages,
		    PGSIZE);

		kprintf("used %lu KiB for resident pagetable\n", used / 1024);

		kprintf("Usable memory area: 0x%lx "
			"(%lu mb long, %lu pages)\n",
		    entries[i]->base, entries[i]->length / (1024 * 1024),
		    entries[i]->length / PGSIZE);

		/* initialise pages */
		for (b = 0; b < bm->npages; b++) {
			bm->pages[b].paddr = bm->base + PGSIZE * b;
			LIST_INIT(&bm->pages[b].pv_table);
			bm->pages[b].obj = NULL;
		}

		/* mark off the pages used */
		for (b = 0; b < used / PGSIZE; b++) {
			bm->pages[b].queue = kVMPagePMap;
			TAILQ_INSERT_TAIL(&vm_pgpmapq.queue, &bm->pages[b],
			    pagequeue);
			vm_pgpmapq.npages++;
		}

		/* now zero the remainder */
		for (; b < bm->npages; b++) {
			bm->pages[b].queue = kVMPageFree;
			TAILQ_INSERT_TAIL(&vm_pgfreeq.queue, &bm->pages[b],
			    pagequeue);
			vm_pgfreeq.npages++;
		}

		TAILQ_INSERT_TAIL(&vm_pregion_queue, bm, queue);
	}

	x64_vm_init((paddr_t)kernel_address_request.response->physical_base);
}

/* put character to limine terminal + COM1 */
void
md_dbg_putc(int ch, void *ctx)
{
	struct limine_terminal *terminal =
	    terminal_request.response->terminals[0];
	terminal_request.response->write(terminal, (char *)&ch, 1);

	while (!(inb(kPortCOM1 + 5) & 0x20))
		;
	outb(kPortCOM1, ch);
}

/* can't rely on mutexes until scheduling is up (and in any case not in idle
 * thread), so this must be used instead */
static kspinlock_t early_lock = KSPINLOCK_INITIALISER;
static int	   cpus_up = 0;

static void
common_init(struct limine_smp_info *smpi)
{
	kcpu_t	  *cpu = (kcpu_t *)smpi->extra_argument;
	kthread_t *thread = cpu->running_thread;

	idt_load();
	cpu->md.lapic_base = rdmsr(kAMD64MSRAPICBase);
	lapic_enable(0xff);

	/* nkx_thread_common_init allocates... */
	nk_spinlock_acquire_nospl(&early_lock);
	nkx_thread_common_init(thread, cpu, &proc0);
	nk_spinlock_release_nospl(&early_lock);
	thread->state = kThreadStateRunning;

	setup_cpu_gdt(cpu);

	/* measure thrice and average it */
	cpu->md.lapic_tps = 0;
	cpu->md.lapic_id = smpi->lapic_id;
	for (int i = 0; i < 3; i++)
		cpu->md.lapic_tps += lapic_timer_calibrate() / 3;

	TAILQ_INIT(&cpu->callout_queue);
	TAILQ_INIT(&cpu->dpc_queue);
	cpu->soft_int_dispatch = false;
	cpu->entering_scheduler = false;
	cpu->idle_thread = thread;

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
	md_timeslicing_start();
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
			smpi->extra_argument = (uint64_t)&cpu0;
			cpu0.num = i;

			cpu0.preempt_dpc.arg = &cpu0;
			cpu0.preempt_dpc.callback = nkx_preempt_dpc;
			all_cpus[i] = &cpu0;
			common_init(smpi);
		} else {
			kcpu_t	  *cpu = kmem_alloc(sizeof *cpu);
			kthread_t *thread = kmem_alloc(sizeof *thread);

			cpu->num = i;
			cpu->running_thread = thread;
			cpu->preempt_dpc.arg = cpu;
			cpu->preempt_dpc.callback = nkx_preempt_dpc;
			all_cpus[i] = cpu;

			smpi->extra_argument = (uint64_t)cpu;
			smpi->goto_address = ap_init;
		}
	}

	while (cpus_up != smpr->cpu_count)
		__asm__("pause");
}

static ksemaphore_t sem;
static ktimer_t	    timer;

static void
fun2(void *arg)
{
	kprintf("Hello from thread B! My address: %p\n",
	    curcpu()->running_thread);

	nk_timer_init(&timer);

	while (true) {
		nk_timer_set(&timer, NS_PER_S / 2);
		int r = nk_wait(&timer, "b_timer", false, false, NS_PER_S);
		nk_assert(r == kKernWaitStatusOK);
		kprintf("B: Releasing Semaphore\n");
		nk_semaphore_release(&sem, 1);
	}

	done();
}

static void
fun(void *arg)
{
	kprintf("Hello from thread A! My address; %p!\n",
	    curcpu()->running_thread);

	nk_semaphore_init(&sem, 0);
	nk_timer_init(&timer);

	kthread_t thread;
	nk_thread_init(&proc0, &thread, fun2, 0xf008a1);
	nk_thread_resume(&thread);

	kprintf("Hello after thread B began!\n");

	while (true) {
		kprintf("A: Waiting on Semaphore\n");
		int r = nk_wait(&sem, "a_", false, false, NS_PER_S);
		nk_assert(r == kKernWaitStatusOK);
		// nk_mutex_release(&mtx);
	}

#if 0
	char *nul = 0;
	*nul = '\0';
#endif
	done();
}

void start(void*)
{
	int autoconf(void);
	autoconf();
	done();
}

// The following will be our kernel's entry point.
void
_start(void)
{
	void *pcpu0 = &cpu0;

	serial_init();

	// Ensure we got a terminal
	if (terminal_request.response == NULL ||
	    terminal_request.response->terminal_count < 1) {
		done();
	}

	/* setting up state immediately so curcpu()/curthread() work */
	cpu0.running_thread = &thread0;
	wrmsr(kAMD64MSRGSBase, (uint64_t)&pcpu0);

	nk_dbg("Keyronex\n");

	spl0();
	idt_setup();
	idt_load();
	mem_init();
	vm_kernel_init();
	kmem_init();
	ksrv_parsekern(kernel_file_request.response->kernel_file->address);

	/* setup proc0 */
	nk_spinlock_init(&proc0.lock);
	SLIST_INIT(&proc0.threads);

	smp_init();

	kthread_t start_thread;
	nk_thread_init(&proc0, &start_thread, start, 0x0);
	nk_thread_resume(&start_thread);


#if 0
	char *stuff = kmem_alloc(
	    56); // kmem_asprintf("%s, %s!", "Hello", "World");
	memcpy(stuff, "hello", 6);
	char *test = 0x0;
	*test = 'f';
#endif

	// We're done, just hang...
	done();
}
