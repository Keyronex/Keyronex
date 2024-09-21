#include <limine.h>
#include <stdint.h>

#include "gic.h"
#include "kdk/kern.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kern/aarch64/cpu.h"
#include "vm/vmp.h"

void gicc_for_mpidr(uint32_t mpidr);
void intr_init(void);
void irq_init(void);
void vmp_set_ttbr1(void);

volatile uint8_t *uart = (uint8_t *)P2V(0x09000000);

void
pac_putc(int c, void *ctx)
{
	if (c == '\n')
		*uart = '\r';
	*uart = c;
}

void
md_cpu_init(kcpu_t *cpu)
{
	cpu->cpucb.ipl = 0;
	cpu->cpucb.hard_ipl = 0;
}

void
plat_first_init(void)
{
	asm volatile("msr tpidr_el1, %0\n\t"
		     "mov x18, %0\n\t" ::"r"(&bootstrap_cpu_local_data));
	asm volatile("mov x0, sp\n"
		     "msr SPSel, #1\n"
		     "mov sp, x0\n"
		     :
		     :
		     : "x0");
	intr_init();
	irq_init();
}

void
plat_ap_early_init(kcpu_t *cpu, struct limine_smp_info *smpi)
{
	vmp_set_ttbr1();
	asm volatile("msr tpidr_el1, %0\n\t"
		     "mov x18, %0\n\t" ::"r"(cpu->local_data));
	asm volatile("mov x0, sp\n"
		     "msr SPSel, #1\n"
		     "mov sp, x0\n"
		     :
		     :
		     : "x0");
	intr_init();
}

void
plat_common_core_early_init(kcpu_t *cpu, kthread_t *idle_thread,
    struct limine_smp_info *smpi)
{
	uintptr_t cpacr;
	asm volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
	/* fpen */
	asm volatile("msr cpacr_el1, %0" ::"r"(cpacr | (0b11 << 20)));
	cpu->cpucb.mpidr = smpi->mpidr;
}

void
plat_common_core_late_init(kcpu_t *cpu, kthread_t *idle_thread,
    struct limine_smp_info *smpi)
{
	void enable_timer(void);
	enable_timer();
}
