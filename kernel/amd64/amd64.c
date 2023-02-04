#include <sys/param.h>

#include <amd64/amd64.h>
#include <amd64/asmintr.h>
#include <kern/ksrv.h>
#include <md/intr.h>
#include <md/vm.h>
#include <nanokern/kerndefs.h>
#include <nanokern/kernmisc.h>
#include <nanokern/thread.h>
#include <vm/vm.h>

enum {
	kIntNumSyscall = 128,
	/*! < CR8=15*/
	kIntNumLAPICTimer = 224,
	kIntNumRescheduleIPI = 225,
	/*! unfilterable */
	kIntNumSwitch = 240, /* Manually invoked with INT */
	kIntNumIPIInvlPG = 241,
};

enum {
	kLAPICRegEOI = 0xb0,
	kLAPICRegSpurious = 0xf0,
	kLAPICRegICR0 = 0x300,
	kLAPICRegICR1 = 0x310,
	kLAPICRegTimer = 0x320,
	kLAPICRegTimerInitial = 0x380,
	kLAPICRegTimerCurrentCount = 0x390,
	kLAPICRegTimerDivider = 0x3e0,
};

enum {
	kLAPICTimerPeriodic = 0x20000,
};

typedef struct {
	uint16_t length;
	uint16_t base_low;
	uint8_t	 base_mid;
	uint8_t	 access;
	uint8_t	 flags;
	uint8_t	 base_high;
	uint32_t base_upper;
	uint32_t reserved;
} __attribute__((packed)) tss_gdt_entry_t;

static struct gdt {
	uint64_t	null;
	uint64_t	code16;
	uint64_t	data16;
	uint64_t	code32;
	uint64_t	data32;
	uint64_t	code64;
	uint64_t	data64;
	uint64_t	code64_user;
	uint64_t	data64_user;
	tss_gdt_entry_t tss;
} __attribute__((packed)) gdt = {
	.null = 0x0,
	.code16 = 0x8f9a000000ffff,
	.data16 = 0x8f92000000ffff,
	.code32 = 0xcf9a000000ffff,
	.data32 = 0xcf92000000ffff,
	.code64 = 0xaf9a000000ffff,
	.data64 = 0x8f92000000ffff,
	.code64_user = 0xaffa000000ffff,
	.data64_user = 0x8ff2000000ffff,
	.tss = { .length = 0x68, .access = 0x89 },
};

typedef struct {
	uint16_t isr_low;
	uint16_t selector;
	uint8_t	 ist;
	uint8_t	 type;
	uint16_t isr_mid;
	uint32_t isr_high;
	uint32_t zero;
} __attribute__((packed)) idt_entry_t;

struct md_intr_entry {
	const char	 *name;
	ipl_t		  prio;
	intr_handler_fn_t handler;
	void		 *arg;
};

static bool intr_syscall(md_intr_frame_t *frame, void *arg);
static bool pagefault(md_intr_frame_t *frame, void *arg);
void	    idt_load(void);
void	    lapic_eoi();

static idt_entry_t	    idt[256] = { 0 };
static struct md_intr_entry md_intrs[256] = { 0 };

/*
 * TODO: "Avoid placing a page boundary in the part of the TSS that the
 * processor reads during a task switch (the first 104 bytes)" saith the
 * Intel x86 and 64 Manual. So for now it's a statically-allocated thing.
 */
static tss_t tss[64] __attribute__((aligned(4096)));

void
load_gdt()
{
	struct {
		uint16_t limit;
		vaddr_t	 addr;
	} __attribute__((packed)) gdtr = { sizeof(gdt) - 1, (vaddr_t)&gdt };

	asm volatile("lgdt %0" : : "m"(gdtr));
}

void
setup_cpu_gdt(kcpu_t *cpu)
{
	static kspinlock_t gdt_lock = KSPINLOCK_INITIALISER;

	nk_spinlock_acquire_nospl(&gdt_lock);

	cpu->md.tss = &tss[cpu->num];

	/* for when we do dynamic allocation of tss' again, sanity check: */
	/* assert((uintptr_t)cpu->tss / PGSIZE ==
	    ((uintptr_t)cpu->tss + 104) / PGSIZE); */

	gdt.tss.length = 0x68;
	gdt.tss.base_low = (uintptr_t)cpu->md.tss;
	gdt.tss.base_mid = (uintptr_t)cpu->md.tss >> 16;
	gdt.tss.access = 0x89;
	gdt.tss.flags = 0x0;
	gdt.tss.base_high = (uintptr_t)cpu->md.tss >> 24;
	gdt.tss.base_upper = (uintptr_t)cpu->md.tss >> 32;
	gdt.tss.reserved = 0x0;
	load_gdt();
	asm volatile("ltr %0" ::"rm"((uint16_t)offsetof(struct gdt, tss)));
	nk_spinlock_release_nospl(&gdt_lock);
}

static void
idt_set(uint8_t index, vaddr_t isr, uint8_t type, uint8_t ist)
{
	idt[index].isr_low = (uint64_t)isr & 0xFFFF;
	idt[index].isr_mid = ((uint64_t)isr >> 16) & 0xFFFF;
	idt[index].isr_high = (uint64_t)isr >> 32;
	idt[index].selector = 0x28; /* sixth */
	idt[index].type = type;
	idt[index].ist = ist;
	idt[index].zero = 0x0;
}

/* setup the initial IDT */
void
idt_setup(void)
{
#define IDT_SET(VAL) idt_set(VAL, (vaddr_t)&isr_thunk_##VAL, INT, 0);
	NORMAL_INTS(IDT_SET);
#undef IDT_SET

#define IDT_SET(VAL, GATE) idt_set(VAL, (vaddr_t)&isr_thunk_##VAL, GATE, 0);
	SPECIAL_INTS(IDT_SET);
#undef IDT_SET

	idt_load();
	md_intr_register(14, kSPL0, pagefault, NULL);
	md_intr_register(kIntNumLAPICTimer, kSPLHigh, nkx_cpu_hardclock, NULL);
	md_intr_register(kIntNumRescheduleIPI, kSPLHigh, nkx_reschedule_ipi,
	    NULL);
	md_intr_register(kIntNumSyscall, kSPL0, intr_syscall, NULL);
}

void
idt_load(void)
{
	struct {
		uint16_t limit;
		vaddr_t	 addr;
	} __attribute__((packed)) idtr = { sizeof(idt) - 1, (vaddr_t)idt };

	asm volatile("lidt %0" : : "m"(idtr));
}

static bool
pagefault(md_intr_frame_t *frame, void *arg)
{
	if (curcpu()->running_thread->in_pagefault) {
		nk_dbg("nested pagefault in %s\n",
		    curcpu()->running_thread->name);
		goto fail;
	}

retry:
	curcpu()->running_thread->in_pagefault = true;
	int r = vm_fault(frame, curthread()->process->map, (vaddr_t)read_cr2(), frame->code);
	curcpu()->running_thread->in_pagefault = false;

	switch (r) {
	case kVMFaultRetOK:
		/* epsilon*/
		break;

	case kVMFaultRetPageShortage:
		/* */
		nk_dbg("going to sleep on pagedaemon.free_event\n");
		vm_pd_wait();
		goto retry;

	case kVMFaultRetFailure:
		nk_dbg("unhandled page fault in thread %s:\n",
		    curcpu()->running_thread->name);

	fail:
		md_intr_frame_trace(frame);
		nk_fatal("halting\n");
	}

	return true;
}

static bool
intr_syscall(md_intr_frame_t *frame, void *arg)
{
	int posix_syscall(md_intr_frame_t * frame);
	posix_syscall(frame);
	return true;
}

void
handle_int(md_intr_frame_t *frame, uintptr_t num)
{
	ipl_t		      ipl;
	struct md_intr_entry *entry;

	if (num == kIntNumSwitch) {
		/* here the context switch actually happens */
		kthread_t *old = curcpu()->md.old, *next = curcpu()->md.new;

#if DEBUG_SCHED == 1
		nk_dbg("Switch from %p to %p\n", old, next);
#endif

		old->frame = *frame;
		// old->md.fs = rdmsr(kAMD64MSRFSBase);

		*frame = next->frame;
		// wrmsr(kAMD64MSRFSBase, next->md.fs);

		// curcpu()->running_thread = next;
		curcpu()->md.tss->rsp0 = next->kstack;

		nk_spinlock_release_nospl(&curcpu()->sched_lock);
		splx(curcpu()->md.switchipl);
		return;
	}

	entry = &md_intrs[num];

	if (entry->handler == NULL) {
		nk_dbg("Unhandled interrupt %lu. Stack trace:\n", num);
		md_intr_frame_trace(frame);
		nk_fatal("unhandled interrupt %lu\n", num);
	}

	if (splget() > entry->prio) {
		nk_dbg("In trying to handle interrupt %lu:\n"
		       "SPL not less or equal (was at %u, need %u)\n",
		    num, splget(), entry->prio);
		if (num == 14) {
			nk_dbg("(CR2: 0x%lx)\n", read_cr2());
		}
		md_intr_frame_trace(frame);
		nk_fatal("halting\n");
	}
	ipl = splraise(entry->prio);

	entry->handler(frame, entry->arg);

	if (num >= 32) {
		lapic_eoi();
	}

	splx(ipl);
}

static uint32_t
lapic_read(kcpu_t *cpu, uint32_t reg)
{
	return *(uint32_t *)P2V((cpu->md.lapic_base & 0xfffff000) + reg);
}

static void
lapic_write(kcpu_t *cpu, uint32_t reg, uint32_t val)
{
	uint32_t *addr = P2V((cpu->md.lapic_base & 0xfffff000) + reg);
	*addr = val;
}

void
lapic_eoi()
{
	lapic_write(curcpu(), kLAPICRegEOI, 0x0);
}

void
lapic_enable(uint8_t spurvec)
{
	lapic_write(curcpu(), kLAPICRegSpurious,
	    lapic_read(curcpu(), kLAPICRegSpurious) | (1 << 8) | spurvec);
}

/* setup PIC to run oneshot for 1/hz sec */
static void
pit_init_oneshot(uint32_t hz)
{
	int divisor = 1193180 / hz;

	outb(0x43, 0x30 /* lohi */);

	outb(0x40, divisor & 0xFF);
	outb(0x40, divisor >> 8);
}

/* await on completion of a oneshot */
static void
pit_await_oneshot(void)
{
	do {
		/* bits 7, 6 must = 1, 5 = don't latch count, 1 = channel 0 */
		outb(0x43, (1 << 7) | (1 << 6) | (1 << 5) | (1 << 1));
	} while (!(inb(0x40) & (1 << 7))); /* check if set */
}

/* return the number of ticks per second for the lapic timer */
uint32_t
lapic_timer_calibrate()
{
	const uint32_t	   initial = 0xffffffff;
	const uint32_t	   hz = 50;
	uint32_t	   apic_after;
	static kspinlock_t calib = KSPINLOCK_INITIALISER;

	ipl_t ipl = nk_spinlock_acquire(&calib);

	lapic_write(curcpu(), kLAPICRegTimerDivider, 0x2); /* divide by 8*/
	lapic_write(curcpu(), kLAPICRegTimer, kIntNumLAPICTimer);

	pit_init_oneshot(hz);
	lapic_write(curcpu(), kLAPICRegTimerInitial, initial);

	pit_await_oneshot();
	apic_after = lapic_read(curcpu(), kLAPICRegTimerCurrentCount);
	// lapic_write(kLAPICRegTimer, 0x10000); /* disable*/

	nk_spinlock_release(&calib, ipl);

	return (initial - apic_after) * hz;
}

uint8_t
md_intr_alloc(ipl_t prio, intr_handler_fn_t handler, void *arg, bool canShare)
{
	uint8_t vec = 0;

	/* find a vector appropriate to the priority */
	for (int i = MAX(prio << 4, 32); i < (prio << 4) + 16; i++)
		if (md_intrs[i].handler == NULL) {
			vec = i;
			break;
		}

	if (vec == 0) {
		nk_dbg("md_intr_alloc: out of vectors for priority %u\n", prio);
		return -1;
	}

	md_intr_register(vec, prio, handler, arg);
	return vec;
}

void
md_intr_register(int vec, ipl_t prio, intr_handler_fn_t handler, void *arg)
{
	md_intrs[vec].prio = prio;
	md_intrs[vec].handler = handler;
	md_intrs[vec].arg = arg;
}

void
md_intr_frame_trace(md_intr_frame_t *frame)
{
	struct frame {
		struct frame *rbp;
		uint64_t      rip;
	} *aframe = (struct frame *)frame->rbp;
	const char *name = NULL;
	size_t	    offs = 0;

	ksrv_backtrace((vaddr_t)frame->rip, &name, &offs);
	nk_dbg("Begin stack trace:\n");
	nk_dbg(" - %p %s+%lu\n", (void *)frame->rip, name ? name : "???", offs);

	if (aframe != NULL)
		do {
			name = NULL;
			offs = 0;
			ksrv_backtrace((vaddr_t)aframe->rip, &name, &offs);
			nk_dbg(" - %p %s+%lu\n", (void *)aframe->rip,
			    name ? name : "???", offs);
			//nk_dbg("stack depth: %lu\n",
			//    curcpu()->running_thread->kstack -
			//	(uintptr_t)aframe)
		} while ((aframe = aframe->rbp) && //(uint64_t)aframe >= KERN_BASE &&
		    aframe->rip != 0x0);
}

static void
send_ipi(uint32_t lapic_id, uint8_t intr)
{
	lapic_write(curcpu(), kLAPICRegICR1, lapic_id << 24);
	lapic_write(curcpu(), kLAPICRegICR0, intr);
}

void
md_ipi_invlpg(kcpu_t *cpu)
{
	send_ipi(cpu->md.lapic_id, kIntNumIPIInvlPG);
}

void
md_ipi_reschedule(kcpu_t *cpu)
{
	send_ipi(cpu->md.lapic_id, kIntNumRescheduleIPI);
}

#if 0
void
md_timer_set(struct kcpu *cpu, uint64_t nanos)
{
	uint64_t ticks = cpu->md.lapic_tps * nanos / NS_PER_S;
	nk_assert(ticks < UINT32_MAX);
	lapic_write(cpu, kLAPICRegTimerInitial, ticks);
}

uint64_t
md_timer_get_remaining(struct kcpu *cpu)
{
	return (((uint64_t)lapic_read(cpu, kLAPICRegTimerCurrentCount)) *
		   NS_PER_S) /
	    cpu->md.lapic_tps;
}
#endif

void
md_timeslicing_start()
{
	lapic_write(curcpu(), kLAPICRegTimer,
	    kLAPICTimerPeriodic | kIntNumLAPICTimer);
	lapic_write(curcpu(), kLAPICRegTimerInitial,
	    curcpu()->md.lapic_tps / KERN_HZ);
}

void
md_thread_init(struct kthread *thread, void (*start_fun)(void *),
    void		      *start_arg)
{
	thread->frame.cs = 0x28;
	thread->frame.ss = 0x30;
	thread->frame.rflags = 0x202;
	thread->frame.rip = (uintptr_t)start_fun;
	thread->frame.rdi = (uintptr_t)start_arg;
	thread->frame.rbp = 0;
	/*
	 * subtract 8 since there is no `call` prior to executing the function
	 * so its push %rbp will misalign the stack
	 */
	thread->frame.rsp = (uintptr_t)thread->kstack - 8;
}

void
md_switch(ipl_t switchipl, struct kthread *from, struct kthread *to)
{
	curcpu()->md.switchipl = switchipl;
	curcpu()->md.old = from;
	curcpu()->md.new = to;
	/* the sched lock will be dropped (and IPL too) here */
	asm("int $240");
}
