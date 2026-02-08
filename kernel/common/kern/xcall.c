/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file xcall.c
 * @brief Cross-processor call handling.
 */

#include <keyronex/cpulocal.h>
#include <keyronex/cpu.h>

#define UINTPTR_BITS (sizeof(uintptr_t) * 8)

void
ke_xcall_broadcast(void (*func)(void *), void *arg)
{
	ipl_t ipl = splhigh();
	struct kcpu_data *local = CPU_LOCAL_GET();

	local->func = func;
	local->arg = arg;
	atomic_store_explicit(&local->xcalls_completed, 1,
	    memory_order_relaxed);

	atomic_thread_fence(memory_order_release);

	for (uint32_t i = 0; i < ke_ncpu; i++) {
		katomic_cpumask_t *mask = &ke_cpu_data[i]->xcalls_pending;
		uint32_t index = local->cpu_num / UINTPTR_BITS;
		uint32_t bit = local->cpu_num % UINTPTR_BITS;
		atomic_fetch_or_explicit(&mask->mask[index],
		    (uintptr_t)1 << bit, memory_order_relaxed);
	}

	kep_arch_ipi_broadcast();

	splx(IPL_DISP);

	while (atomic_load_explicit(&local->xcalls_completed,
		   memory_order_relaxed) < ke_ncpu)
		ke_arch_pause();

	splx(ipl);
}

void
ke_xcall_unicast(void (*func)(void *), void *arg, kcpunum_t cpu_num)
{
	ipl_t ipl = splhigh();
	struct kcpu_data *local = CPU_LOCAL_GET();
	katomic_cpumask_t *mask = &ke_cpu_data[cpu_num]->xcalls_pending;
	uint32_t index = local->cpu_num / UINTPTR_BITS;
	uint32_t bit = local->cpu_num % UINTPTR_BITS;

	local->func = func;
	local->arg = arg;
	atomic_store_explicit(&local->xcalls_completed, 0,
	    memory_order_relaxed);

	atomic_thread_fence(memory_order_release);

	atomic_fetch_or_explicit(&mask->mask[index], (uintptr_t)1 << bit,
	    memory_order_relaxed);

	splx(IPL_DISP);

	kep_arch_ipi_unicast(cpu_num);

	while (atomic_load_explicit(&local->xcalls_completed,
		   memory_order_relaxed) != 1)
		ke_arch_pause();

	splx(ipl);
}

void
kep_xcall_handler(void *unused)
{
	struct kcpu_data *local = CPU_LOCAL_GET();
	katomic_cpumask_t *mask = &local->xcalls_pending;
	int mask_n = roundup2(ke_ncpu, UINTPTR_BITS) / UINTPTR_BITS;

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
			    memory_order_relaxed);
		}
	}
}
