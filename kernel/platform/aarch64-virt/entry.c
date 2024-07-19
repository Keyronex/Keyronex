#include <limine.h>
#include <stdint.h>

#include "kdk/kmem.h"
#include "kdk/kern.h"
#include "kdk/object.h"
#include "kern/aarch64/cpu.h"
#include "vm/vmp.h"

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

ipl_t
splraise(ipl_t ipl)
{
	return kIPL0;
}

void
splx(ipl_t ipl)
{
	(void)ipl;
}

ipl_t
splget(void)
{
	return kIPL0;
}

void
md_raise_dpc_interrupt(void)
{
	curcpu()->dpc_int = true;
}

void
md_cpu_init(kcpu_t *cpu)
{
}

void
md_switch(kthread_t *old_thread)
{
	kfatal("Implement this\n");
}

void
ke_thread_init_context(kthread_t *thread, void (*func)(void *), void *arg)
{
	kfatal("Implement me\n");
}

void
plat_first_init(void)
{
	intr_init();
}

void
plat_ap_early_init(kcpu_t *cpu, struct limine_smp_info *smpi)
{
	vmp_set_ttbr1();
	intr_init();
	kprintf("AP early init..\n");
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
