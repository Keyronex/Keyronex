/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file xcall.c
 * @brief Cross-processor call handling.
 */

#include <sys/cpulocal.h>
#include <sys/k_cpu.h>
#include <sys/k_cpumask.h>
#include <sys/k_xcall.h>
#include <sys/k_log.h>

/* returns how many CPUs were armed */
static unsigned
xcall_arm(struct kcpu_data *local, void (*func)(void *), void *arg,
    const kcpumask_t *targets)
{
	kcpunum_t cpu;
	unsigned n = 0;

	local->func = func;
	local->arg = arg;
	atomic_store_explicit(&local->xcalls_completed, 0,
	    memory_order_relaxed);

	atomic_thread_fence(memory_order_release);

	KE_CPUMASK_FOREACH(cpu, targets) {
		kassert(cpu != local->cpu_num, "xcall to self");
		ke_atomic_cpumask_set(&ke_cpu_data[cpu]->xcalls_pending,
		    local->cpu_num, memory_order_relaxed);
		n++;
	}

	return n;
}

/* wait for xcall completion at IPL_DISP (lets other xcalls in) on n CPUs */
static void
xcall_wait(struct kcpu_data *local, unsigned n)
{
	splx(IPL_DISP);

	while (atomic_load_explicit(&local->xcalls_completed,
		   memory_order_acquire) < n)
		ke_arch_pause();
}

void
ke_xcall_broadcast(void (*func)(void *), void *arg)
{
	ipl_t ipl = splhigh();
	struct kcpu_data *local = CPU_LOCAL_GET();
	kcpumask_t targets;
	unsigned n;

	ke_cpumask_zero(&targets);
	for (kcpunum_t i = 0; i < ke_ncpu; i++)
		if (i != local->cpu_num)
			ke_cpumask_set(&targets, i);

	n = xcall_arm(local, func, arg, &targets);
	if (n != 0) {
		kep_arch_ipi_broadcast();
		xcall_wait(local, n);
	}

	splx(ipl);
}

void
ke_xcall_multicast(void (*func)(void *), void *arg, const kcpumask_t *cpus)
{
	ipl_t ipl = splhigh();
	struct kcpu_data *local = CPU_LOCAL_GET();
	kcpumask_t targets = *cpus;
	unsigned n;

	ke_cpumask_clear(&targets, local->cpu_num);

	n = xcall_arm(local, func, arg, &targets);
	if (n != 0) {
		kep_arch_ipi_multicast(&targets);
		xcall_wait(local, n);
	}

	splx(ipl);
}

void
ke_xcall_unicast(void (*func)(void *), void *arg, kcpunum_t cpu_num)
{
	kcpumask_t targets;

	kassert(cpu_num != CPU_LOCAL_LOAD(cpu_num), "xcall unicast to self");

	ke_cpumask_zero(&targets);
	ke_cpumask_set(&targets, cpu_num);
	ke_xcall_multicast(func, arg, &targets);
}

void
ke_xcall_reschedule(kcpunum_t cpu_num)
{
	ipl_t ipl = splhigh();

	kassert(cpu_num != CPU_LOCAL_LOAD(cpu_num), "resched xcall to self");

	atomic_store_explicit(&ke_cpu_data[cpu_num]->resched_ipi_pending, true,
	    memory_order_release);
	kep_arch_ipi_unicast(cpu_num);

	splx(ipl);
}

void
ke_xcall_nop_multicast(const kcpumask_t *cpus)
{
	ipl_t ipl = splhigh();
	kcpumask_t targets = *cpus;

	ke_cpumask_clear(&targets, CPU_LOCAL_LOAD(cpu_num));
	if (!ke_cpumask_empty(&targets))
		kep_arch_ipi_multicast(&targets);

	splx(ipl);
}

void
kep_xcall_handler(void *unused)
{
	struct kcpu_data *local = CPU_LOCAL_GET();
	katomic_cpumask_t *mask = &local->xcalls_pending;
	int mask_n = roundup2(ke_ncpu, UINTPTR_BITS) / UINTPTR_BITS;

	(void)unused;

	if (atomic_exchange_explicit(&local->resched_ipi_pending, false,
		memory_order_acq_rel))
		kep_xcall_reschedule_handler();

	for (int index = 0; index < mask_n; index++) {
		uintptr_t pending = atomic_exchange_explicit(&mask->mask[index],
		    0, memory_order_relaxed);

		atomic_thread_fence(memory_order_acquire);

		while (pending != 0) {
			int bit = __builtin_ctzl(pending);
			uintptr_t bitmask = (uintptr_t)1 << bit;
			uint32_t cpu_num;
			atomic_uint *remote_completed;
			void (*func)(void *);
			void *arg;

			pending &= ~bitmask;

			cpu_num = index * UINTPTR_BITS + bit;

			func = ke_cpu_data[cpu_num]->func;
			arg = ke_cpu_data[cpu_num]->arg;
			remote_completed =
			    &ke_cpu_data[cpu_num]->xcalls_completed;

			func(arg);

			atomic_fetch_add_explicit(remote_completed, 1,
			    memory_order_release);
		}
	}
}
