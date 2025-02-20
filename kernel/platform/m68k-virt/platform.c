/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Wed Aug 21 2024.
 */

#include <kdk/kern.h>
#include <limine.h>
#include "goldfish.h"

extern kthread_t thread0;
kcpu_local_data_t cpu_local_data;

void
plat_first_init(void)
{
	thread0.last_cpu = &bootstrap_cpu;
	bootstrap_cpu.curthread = &thread0;
	cpu_local_data.curthread = &thread0;
	cpu_local_data.cpu = &bootstrap_cpu;
	void intr_init(void);
	intr_init();
}

void
plat_pre_smp_init(void)
{
}

void
plat_common_core_early_init(kcpu_t *cpu, kthread_t *idle_thread,
    struct limine_smp_info *smpi)
{
	(void)cpu;
	(void)idle_thread;
	(void)smpi;
}

void
plat_common_core_late_init(kcpu_t *cpu, kthread_t *idle_thread,
    struct limine_smp_info *smpi)
{
	gfrtc_init();
	gfrtc_oneshot(NS_PER_S / KERN_HZ);
	(void)cpu;
	(void)idle_thread;
	(void)smpi;
}
