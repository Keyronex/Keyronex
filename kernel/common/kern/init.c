/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file init.c
 * @brief Kernel initialisation.
 */

#include <keyronex/cpu.h>
#include <keyronex/ktask.h>

void kep_arch_set_tp(void *);
void kep_arch_set_vbase(bool isbsp);
void pmap_set_kpgtable(void);

ktask_t *ke_task0;
struct kcpu_data ke_bsp_cpu_data;
struct kcpu_data **ke_cpu_data;
size_t ke_ncpu;

void ke_bsp_early_init(ktask_t *task0, kthread_t *kthread0)
{
	ke_task0 = task0;
	kep_arch_set_vbase(true);
	kep_arch_set_tp(&ke_bsp_cpu_data);
	kthread0->task = task0;
	kthread0->state = TS_RUNNING;
}

void
ke_ap_init(kcpunum_t cpunum)
{
	kep_arch_set_vbase(false);
	kep_arch_set_tp(ke_cpu_data[cpunum]);
	pmap_set_kpgtable();
}
