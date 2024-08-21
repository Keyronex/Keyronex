/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Wed Aug 21 2024.
 */

#include <kdk/kern.h>
#include <limine.h>

extern kthread_t thread0;

void
plat_first_init(void)
{
	thread0.last_cpu = &bootstrap_cpu;
	bootstrap_cpu.curthread = &thread0;
	void intr_init(void);
	intr_init();
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
	(void)cpu;
	(void)idle_thread;
	(void)smpi;
}
