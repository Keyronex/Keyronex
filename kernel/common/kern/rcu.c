/*
 * Copyright (c) 2024-26 Cloudarox Solutions.
 * Created on Fri April 4 2024.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file rcu.c
 * @brief Read-copy-update.
 */

#include <sys/cpulocal.h>
#include <sys/k_cpu.h>
#include <sys/k_cpumask.h>
#include <sys/k_intr.h>
#include <sys/k_rcu.h>

#include <sys/k_log.h>

struct rcu_state {
	kspinlock_t lock;
	katomic_cpumask_t quiesced;
	uintptr_t generation, highest_generation;
};

static struct rcu_state rcu = {
	.lock = KSPINLOCK_INITIALISER,
	.generation = 0,
	.highest_generation = 0,
};

static void
start_gp(void)
{
	kassert(ke_ncpu > 0, "RCU generation before smp_init");

	for (kcpunum_t i = 0; i < ke_ncpu; i++)
		ke_atomic_cpumask_set(&rcu.quiesced, i, memory_order_relaxed);
}

static bool
gp_pending(void)
{
	return ke_atomic_cpumask_first(&rcu.quiesced, memory_order_relaxed) !=
	    KCPUNUM_NULL;
}

static void
complete_gp(void)
{
	/*
	 * Pair with kep_rcu_quiet()'s acquire
	 * so existing reader accesses finish before callbacks run
	 */
	__atomic_store_n(&rcu.generation, rcu.generation + 1, __ATOMIC_RELEASE);

	if (rcu.generation <= rcu.highest_generation)
		start_gp();
}

void
kep_rcu_quiet(void)
{
	struct kep_rcu_per_cpu_data *rcpu = CPU_LOCAL_ADDROF(rcu_cpustate);
	kcpunum_t cpunum = CPU_LOCAL_LOAD(cpu_num);

	kassert(ke_ipl() >= IPL_DISP);

	/*
	 * only we clear our own bit, so this is fine. If we miss a concurrent
	 * GP start then we still report at the next quiescent state.
	 */
	if (ke_atomic_cpumask_isset(&rcu.quiesced, cpunum,
	    memory_order_relaxed)) {
		ke_spinlock_enter_nospl(&rcu.lock);
		ke_atomic_cpumask_clear(&rcu.quiesced, cpunum,
		    memory_order_relaxed);
		if (!gp_pending())
			complete_gp();
		ke_spinlock_exit_nospl(&rcu.lock);
	}

	if (!TAILQ_EMPTY(&rcpu->current_callbacks) &&
	    __atomic_load_n(&rcu.generation, __ATOMIC_ACQUIRE) >
	    rcpu->generation) {
		TAILQ_CONCAT(&rcpu->past_callbacks, &rcpu->current_callbacks,
		    queue_entry);
		ke_dpc_schedule(&rcpu->past_callbacks_dpc);
	}

	if (TAILQ_EMPTY(&rcpu->current_callbacks) &&
	    !TAILQ_EMPTY(&rcpu->next_callbacks)) {
		bool pending;

		TAILQ_CONCAT(&rcpu->current_callbacks, &rcpu->next_callbacks,
		    queue_entry);

		ke_spinlock_enter_nospl(&rcu.lock);

		pending = gp_pending();
		rcpu->generation = pending ? rcu.generation + 1 :
		    rcu.generation;

		kassert(rcu.highest_generation <= rcpu->generation,
		    "rcu generation target regressed");
		rcu.highest_generation = rcpu->generation;

		if (!pending)
			start_gp();

		ke_spinlock_exit_nospl(&rcu.lock);
	}
}

static void
quiet_dpc(void *, void *)
{
	kep_rcu_quiet();
}

void
kep_rcu_hardclock(void)
{
	struct kep_rcu_per_cpu_data *rcpu = CPU_LOCAL_ADDROF(rcu_cpustate);

	if (ke_atomic_cpumask_isset(&rcu.quiesced, CPU_LOCAL_LOAD(cpu_num),
	    memory_order_relaxed) ||
	    !TAILQ_EMPTY(&rcpu->current_callbacks) ||
	    !TAILQ_EMPTY(&rcpu->next_callbacks))
		ke_dpc_schedule(&rcpu->quiet_dpc);
}

static void
process_past_callbacks(void *arg, void *)
{
	struct kep_rcu_per_cpu_data *rcpu = arg;

	while (!TAILQ_EMPTY(&rcpu->past_callbacks)) {
		krcu_entry_t *head = TAILQ_FIRST(&rcpu->past_callbacks);
		TAILQ_REMOVE(&rcpu->past_callbacks, head, queue_entry);
		head->callback(head->arg);
	}
}

void
kep_rcu_per_cpu_init(struct kep_rcu_per_cpu_data *data)
{
	data->generation = 0;
	TAILQ_INIT(&data->past_callbacks);
	TAILQ_INIT(&data->current_callbacks);
	TAILQ_INIT(&data->next_callbacks);
	ke_dpc_init(&data->past_callbacks_dpc, process_past_callbacks, data,
	    NULL);
	ke_dpc_init(&data->quiet_dpc, quiet_dpc, NULL, NULL);
}

void
ke_rcu_call(krcu_entry_t *head, krcu_callback_t callback, void *arg)
{
	ipl_t ipl = spldisp();
	head->callback = callback;
	head->arg = arg;
	TAILQ_INSERT_TAIL(&CPU_LOCAL_ADDROF(rcu_cpustate)->next_callbacks, head,
	    queue_entry);
	splx(ipl);
}

static void
set_event(void *arg)
{
	kevent_t *ev = arg;
	ke_event_set_signalled(ev, true);
}

void
ke_rcu_synchronise(void)
{
	kevent_t ev;
	krcu_entry_t head;

	kassert(ke_ipl() < IPL_DISP);

	ke_event_init(&ev, false);
	ke_rcu_call(&head, set_event, &ev);
	ke_wait1(&ev, "ke_rcu_synchronise", false, ABSTIME_FOREVER);
}
