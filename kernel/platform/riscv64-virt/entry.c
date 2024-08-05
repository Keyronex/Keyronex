/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Aug 05 2024.
 */

#include <limine.h>
#include <stdint.h>

#include "kdk/kern.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "vm/vmp.h"

void
pac_putc(int c, void *ctx)
{
	for (;;)
		;
}

void
md_cpu_init(kcpu_t *cpu)
{
	cpu->cpucb.ipl = 0;
}

void
plat_first_init(void)
{
}

void
plat_ap_early_init(kcpu_t *cpu, struct limine_smp_info *smpi)
{
}

void
plat_common_core_early_init(kcpu_t *cpu, kthread_t *idle_thread,
    struct limine_smp_info *smpi)
{
}

void
plat_common_core_late_init(kcpu_t *cpu, kthread_t *idle_thread,
    struct limine_smp_info *smpi)
{
}
