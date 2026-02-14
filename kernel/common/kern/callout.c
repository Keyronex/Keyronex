/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file kcallout.c
 * @brief Callouts - waitable or DPC-queuing timers.
 */

#include <keyronex/cpu.h>
#include <keyronex/dlog.h>
#include <keyronex/intr.h>
#include <keyronex/kwait.h>

static void
insert(kcallout_t *co, struct kcpu_callout *cc)
{
	kcallout_t *existing;
	bool first = true;

	TAILQ_FOREACH(existing, &cc->callouts, callout_qlink) {
		if (existing->deadline > co->deadline) {
			TAILQ_INSERT_BEFORE(existing, co, callout_qlink);
			if (first) {
				int x = ke_arch_disable();
				cc->next_deadline = co->deadline;
				ke_arch_enable(x);
			}
			return;
		}
		first = false;
	}
	/* no callouts or it's the longest till elapsing */
	TAILQ_INSERT_TAIL(&cc->callouts, co, callout_qlink);
	if (first) {
		int x = ke_arch_disable();
		cc->next_deadline = co->deadline;
		ke_arch_enable(x);
	}
}

void
ke_callout_init(kcallout_t *co)
{
	kep_dispatcher_obj_init(&co->header, 0, SYNCH_CALLOUT);
	atomic_store_explicit(&co->cpu_num, KCPUNUM_NULL, memory_order_relaxed);
	co->softint = NULL;
}

void
ke_callout_init_dpc(kcallout_t *co, kdpc_t *dpc, void (*func)(void *, void *),
    void *arg1, void *arg2)
{
	kep_dispatcher_obj_init(&co->header, 0, SYNCH_CALLOUT);
	atomic_store_explicit(&co->cpu_num, KCPUNUM_NULL, memory_order_relaxed);
	co->softint = dpc;
	ke_dpc_init(dpc, func, arg1, arg2);
}

int
ke_callout_set(kcallout_t *co, kabstime_t deadline)
{
	ipl_t ipl;
	struct kcpu_callout *cc;

	ipl = spldisp();
	cc = CPU_LOCAL_ADDROF(callout);

	ke_spinlock_enter_nospl(&cc->lock);
	ke_spinlock_enter_nospl(&co->header.lock);

	if (atomic_load_explicit(&co->cpu_num, memory_order_relaxed) !=
	    KCPUNUM_NULL)
		kfatal("trying to set an already-set callout");

	atomic_store_explicit(&co->cpu_num, CPU_LOCAL_LOAD(cpu_num),
	    memory_order_relaxed);
	co->deadline = deadline;
	co->header.signalled = 0;
	insert(co, cc);

	ke_spinlock_exit_nospl(&co->header.lock);
	ke_spinlock_exit_nospl(&cc->lock);

	splx(ipl);

	return 0;
}

int
ke_callout_stop(kcallout_t *co)
{
	uint32_t cpu_num;
	ipl_t ipl;
	struct kcpu_callout *cc;

retry:
	cpu_num = atomic_load_explicit(&co->cpu_num, memory_order_relaxed);

	if (cpu_num == KCPUNUM_NULL)
		return -1;

	cc = &ke_cpu_data[cpu_num]->callout;

	ipl = ke_spinlock_enter(&cc->lock);
	ke_spinlock_enter_nospl(&co->header.lock);

	if (atomic_load_explicit(&co->cpu_num, memory_order_relaxed) !=
	    cpu_num) {
		ke_spinlock_exit_nospl(&co->header.lock);
		ke_spinlock_exit(&cc->lock, ipl);
		goto retry;
	}

	TAILQ_REMOVE(&cc->callouts, co, callout_qlink);
	atomic_store_explicit(&co->cpu_num, KCPUNUM_NULL, memory_order_relaxed);

	ke_spinlock_exit_nospl(&co->header.lock);
	ke_spinlock_exit(&cc->lock, ipl);

	return 0;
}

void
kep_callout_expiry_dpc(void *arg1, void *arg2)
{
	struct kcpu_callout *cc = arg1;
	struct kwaitblock_queue wake_queue = TAILQ_HEAD_INITIALIZER(wake_queue);

	ke_spinlock_enter_nospl(&cc->lock);

	while (true) {
		kabstime_t now = ke_time();
		kcallout_t *co = TAILQ_FIRST(&cc->callouts);

		if (co == NULL) {
			int x = ke_arch_disable();
			cc->next_deadline = ABSTIME_NEVER;
			ke_arch_enable(x);
			break;
		} else if (co->deadline > now) {
			int x = ke_arch_disable();
			cc->next_deadline = co->deadline;
			ke_arch_enable(x);
			break;
		}

		TAILQ_REMOVE(&cc->callouts, co, callout_qlink);

		ke_spinlock_enter_nospl(&co->header.lock);
		co->header.signalled = 1;
		kep_signal(&co->header, &wake_queue);
		atomic_store_explicit(&co->cpu_num, KCPUNUM_NULL,
		    memory_order_relaxed);

		if (co->softint != NULL)
			ke_dpc_schedule(co->softint);

		ke_spinlock_exit_nospl(&co->header.lock);
	}

	ke_spinlock_exit_nospl(&cc->lock);

	kep_waiters_wake(&wake_queue);
}

void
kep_callout_hardclock(void)
{
	struct kcpu_callout *cc = CPU_LOCAL_ADDROF(callout);
	kabstime_t now = ke_time();
	kabstime_t next;

	next = cc->next_deadline;
	if (next != ABSTIME_NEVER && now > next)
		ke_dpc_schedule(&cc->expiry_dpc);

	return;
}
