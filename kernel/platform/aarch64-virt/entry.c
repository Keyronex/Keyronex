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
void vmp_set_ttbr1(void);

volatile uint8_t *uart = (uint8_t *)0x09000000;

void
pac_putc(int c, void *ctx)
{
	if (c == '\n')
		*uart = '\r';
	*uart = c;
}

void
md_raise_dpc_interrupt(void)
{
	curcpu()->cpucb.dpc_int = true;
}

void
md_cpu_init(kcpu_t *cpu)
{
	cpu->cpucb.ipl = 0;
}

void
plat_first_init(void)
{
	asm volatile("mov x0, sp\n"
		     "msr SPSel, #1\n"
		     "mov sp, x0\n"
		     :
		     :
		     : "x0");

	asm volatile ("msr tpidr_el1, %0" : : "r" (&thread0) : "memory");
	intr_init();
}

void
plat_ap_early_init(kcpu_t *cpu, struct limine_smp_info *smpi)
{
	asm volatile("mov x0, sp\n"
		     "msr SPSel, #1\n"
		     "mov sp, x0\n"
		     :
		     :
		     : "x0");
	vmp_set_ttbr1();
	intr_init();
	kprintf("AP early init..\n");
}

void
plat_common_core_early_init(kcpu_t *cpu, kthread_t *idle_thread,
    struct limine_smp_info *smpi)
{
	cpu->cpucb.mpidr = smpi->mpidr;
}

void
plat_common_core_late_init(kcpu_t *cpu, kthread_t *idle_thread,
    struct limine_smp_info *smpi)
{
}
