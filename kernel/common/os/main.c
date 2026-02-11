/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file main.c
 * @brief Entry point to the operating system.
 */

#include <keyronex/cpu.h>
#include <keyronex/dlog.h>
#include <keyronex/kmem.h>
#include <keyronex/limine.h>
#include <keyronex/proc.h>
#include <keyronex/vm.h>
#include <keyronex/vmem.h>

#if defined(__amd64__)
#define BSP_ARCH_ID bsp_lapic_id
#define ARCH_ID	    lapic_id
#elif defined(__riscv)
#define BSP_ARCH_ID bsp_hartid
#define ARCH_ID	    hartid
#elif defined(__m68k__)
#define BSP_ARCH_ID bsp_id
#define ARCH_ID	    id
#endif

/* kern/init.c */
void ke_bsp_early_init(ktask_t *, kthread_t *);

/* vm/init.c */
void vm_phys_init(void);

__attribute__((used, section(".requests_start_marker")))
static volatile uint64_t start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests")))
static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests")))
static volatile struct limine_mp_request
    smp_request = { .id = LIMINE_MP_REQUEST_ID, .revision = 0 };

__attribute__((used, section(".requests_end_marker")))
static volatile uint64_t end_marker[] = LIMINE_REQUESTS_END_MARKER;

proc_t proc0;
thread_t thread0;

static void
smp_init(void)
{
	struct limine_mp_response *smp_resp = smp_request.response;

	ke_ncpu = smp_resp->cpu_count;
	kdprintf("smp_init: bringing up %d CPUs\n", ke_ncpu);

	ke_cpu_data = kmem_alloc(sizeof(struct cpu_local_data *) * ke_ncpu);

	for (size_t i = 0; i < smp_resp->cpu_count; i++) {
		struct limine_mp_info *info = smp_resp->cpus[i];
		struct kcpu_data *data;
		thread_t *idle;
		bool is_bsp = (info->ARCH_ID == smp_resp->BSP_ARCH_ID);

		if (is_bsp) {
			data = &ke_bsp_cpu_data;
			idle = &thread0;
		} else {
			data = kmem_alloc(sizeof(struct kcpu_data));
			idle = kmem_alloc(sizeof(thread_t));
		}

		ke_cpu_data[i] = data;

		ke_cpu_init(i, data, info, &idle->kthread);

		// rcu_per_cpu_init(&data->rcu_cpustate);
		// str_per_cpu_init(&data->str_scheduler);

	}
}

static void
ap_init(struct limine_mp_info *smpi)
{
	kfatal("ap_init\n");
#if 0
	vm_phys_init_ap();
	arch_set_cpu_local(cpu_locals[smpi->extra_argument]);
	arch_set_vector_base(false);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	arch_cpu_early_init();
	spl0();
	/* begin ticking */
	arch_cpu_late_init();
	arch_set_deadline(arch_time() + NS_PER_S / HZ);
	hcf();
#endif
}

static void
smp_start(void)
{
#if !defined(__m68k__)
	struct limine_mp_response *smp_resp = smp_request.response;

	for (size_t i = 0; i < smp_resp->cpu_count; i++) {
		struct limine_mp_info *info = smp_resp->cpus[i];
		info->extra_argument = i;
		__atomic_store_n(&info->goto_address, ap_init,
		    __ATOMIC_RELEASE);
	}
#endif
}

void
_start(void)
{
	ke_bsp_early_init(&proc0.ktask, &thread0.kthread);

	kdprintf("Keyronex\n");

	vm_phys_init();
	kmem_init();
	vmem_global_init();
	vm_kwired_init();
	smp_init();

	smp_start();

	kdprintf("Initialisation complete...\n");

	for (;;) ;
}
