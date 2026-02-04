/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file intr.h
 * @brief Generic interrupt handling related logic.
 */

#include <keyronex/cpu.h>
#include <keyronex/intr.h>

#include <stdatomic.h>

typedef void (*si_handler)(struct kcpu_data *);

static void none_si_handler(struct kcpu_data *);
static void ast_level_si_handler(struct kcpu_data *);
static void disp_level_si_handler(struct kcpu_data *);

static si_handler si_handlers[4] = {
	none_si_handler,
	ast_level_si_handler,
	&disp_level_si_handler,
	&disp_level_si_handler
};

uint32_t
pending_si_mask_for_ipl(ipl_t ipl)
{
	return (1u << (uint32_t)(ipl - 1));
}

void
ke_raise_disp_int(void)
{
	struct kcpu_data *cpu = ke_current_cpu();
	atomic_fetch_or_explicit(&cpu->pending_soft_ints,
	    pending_si_mask_for_ipl(IPL_DISP), memory_order_relaxed);
}


static void none_si_handler(struct kcpu_data *)
{
	/* epsilon */
}

static void ast_level_si_handler(struct kcpu_data *)
{
	/* in future, will handle ASTs */
}

static void
disp_level_si_handler(struct kcpu_data *cpu)
{
	/* At time of entry, and at time of exit, interrupts are disabled */

	(void)atomic_fetch_and_explicit(&cpu->pending_soft_ints,
	    ~pending_si_mask_for_ipl(IPL_DISP), memory_order_relaxed);

	cpu->ipl = IPL_DISP;

	for (;;) {
		kdpc_t *dpc;

		ke_spinlock_enter_nospl(&cpu->dpc_lock);
		dpc = TAILQ_FIRST(&cpu->dpc_queue);
		if (dpc == NULL) {
			ke_spinlock_exit_nospl(&cpu->dpc_lock);
			break;
		}

		TAILQ_REMOVE(&cpu->dpc_queue, dpc, qlink);
		atomic_store_explicit(&dpc->cpu, cpu, memory_order_release);

		ke_spinlock_exit_nospl(&cpu->dpc_lock);
		ke_arch_enable(true);

		dpc->handler(dpc->arg1, dpc->arg2);

		(void)ke_arch_disable();
	}

	ke_arch_enable(true);

	if (cpu->redispatch_requested) {
		/* call the dispatcher */
	}

	(void)ke_arch_disable();
}

void
kep_dispatch_softints(ipl_t newipl)
{
	bool intx = ke_arch_disable();
	struct kcpu_data *cpu = ke_current_cpu();

	for (;;) {
		uint32_t pending = atomic_load_explicit(&cpu->pending_soft_ints,
		    memory_order_relaxed) & (3u << newipl);

		if (pending == 0)
			break;

		si_handlers[pending](cpu);

		/* Reload CPU, as dispatching may have changed it. */
		cpu = ke_current_cpu();
	}

	cpu->ipl = newipl;
	ke_arch_enable(intx);
}
