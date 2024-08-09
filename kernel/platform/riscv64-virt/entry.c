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

struct sbi_return_t {
	uintptr_t err, ret;
};

void
sbi_ecall1(int ext, int func, uintptr_t arg0)
{
	struct sbi_return_t ret;
	register uintptr_t a7 asm("a7") = ext;
	register uintptr_t a6 asm("a6") = func;
	register uintptr_t a0 asm("a0") = arg0;
	register uintptr_t a1 asm("a1");
	asm volatile("ecall" : "+r"(a0), "=r"(a1) : "r"(a7), "r"(a6));
	ret.err = a0;
	ret.ret = a1;
}

void
pac_putc(int ch, void *ctx)
{
	sbi_ecall1(1, 0, ch);
}

void
md_cpu_init(kcpu_t *cpu)
{
	cpu->cpucb.ipl = 0;
}

void
plat_first_init(void)
{
	void trap(void);

	asm volatile("mv tp, %0" : : "r"(&thread0));
	asm volatile("csrw sscratch, zero");
	asm volatile("csrw stvec, %0" : : "r"(trap));
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
	uint64_t sie;
	asm volatile("csrr %0, sie" : "=r"(sie));
	sie |= (1UL << 1) | /* (1UL << 5) |*/ (1UL << 9); /* SIE, SEIE, STIE */
	asm volatile("csrw sie, %0" ::"r"(sie));
}
