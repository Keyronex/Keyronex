/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dispatch.c
 * @brief Thread dispatcher.
 */

#include <keyronex/cpu.h>
#include <keyronex/dlog.h>
#include <keyronex/intr.h>
#include <keyronex/ktask.h>
#include <keyronex/limine.h>
#include <keyronex/xcall.h>

#include <libkern/lib.h>
#include <libkern/queue.h>

#include <sched.h>
#include <stdatomic.h>

#define UINTPTR_BITS (sizeof(uintptr_t) * 8)

#define elementsof(x) (sizeof(x) / sizeof((x)[0]))

extern struct ksched_class kep_ts_class, kep_rt_class;

static kspinlock_t rt_lock = KSPINLOCK_INITIALISER;
static atomic_int_fast32_t rt_bitmap;
static runq_t global_rt_rq[RT_PRIO_N];
static katomic_cpumask_t idle_cpu_mask;

static struct ksched_class *kep_sched_class[3] = {
	[SCHED_RR] = &kep_rt_class,
	[SCHED_FIFO] = &kep_rt_class,
	[SCHED_OTHER] = &kep_ts_class,
};

static void
atomic_cpumask_set(katomic_cpumask_t *set, kcpunum_t cpunum, memory_order order)
{
	size_t idx = cpunum / UINTPTR_BITS;
	size_t bit = cpunum % UINTPTR_BITS;

	atomic_fetch_or_explicit(&set->mask[idx], 1UL << bit, order);
}

static void
atomic_cpumask_clear(katomic_cpumask_t *set, kcpunum_t cpunum,
    memory_order order)
{
	size_t idx = cpunum / UINTPTR_BITS;
	size_t bit = cpunum % UINTPTR_BITS;

	atomic_fetch_and_explicit(&set->mask[idx], ~(1UL << bit), order);
}

static bool
atomic_cpumask_isset(katomic_cpumask_t *set, kcpunum_t cpunum,
    memory_order order)
{
	size_t idx = cpunum / UINTPTR_BITS;
	size_t bit = cpunum % UINTPTR_BITS;
	uintptr_t mask = atomic_load_explicit(&set->mask[idx], order);
	return (mask & (1UL << bit)) != 0;
}

static kcpunum_t
atomic_cpumask_first_set(katomic_cpumask_t *set, memory_order order)
{
	size_t width = (ke_ncpu + UINTPTR_BITS - 1) / UINTPTR_BITS;
	for (size_t i = 0; i < width; i++) {
		uintptr_t mask = atomic_load_explicit(&set->mask[i], order);
		if (mask != 0) {
			kcpunum_t cpu = i * UINTPTR_BITS + __builtin_ctzl(mask);
			return cpu < ke_ncpu ? cpu : KCPUNUM_NULL;
		}
	}
	return KCPUNUM_NULL;
}

void
ke_disp_global_init(void)
{
	for (size_t i = 0; i < elementsof(idle_cpu_mask.mask); i++)
		atomic_store_explicit(&idle_cpu_mask.mask[i], UINTPTR_MAX,
		    memory_order_relaxed);
	for (size_t i = 0; i < RT_PRIO_N; i++)
		TAILQ_INIT(&global_rt_rq[i]);
}

void
ke_disp_init(kcpunum_t cpunum)
{
	struct kcpu_dispatcher *disp = &ke_cpu_data[cpunum]->disp;
	size_t i;

	ke_spinlock_init(&disp->lock);

	for (i = 0; i < PRIO_LIMIT; i++)
		TAILQ_INIT(&disp->rq[i]);

	memset(disp->bitmap, 0, sizeof(disp->bitmap));
	disp->idle_thread = ke_cpu_data[cpunum]->curthread;
	disp->cur_thread = disp->idle_thread;
	atomic_store_explicit(&disp->timeslice, 5, memory_order_relaxed);
}

static bool
should_preempt(struct kcpu_dispatcher *disp, kthread_t *thread)
{
	kthread_t *cur = disp->cur_thread;

	if (cur == disp->idle_thread) {
		return true;
	} else if (thread->sched_class == SCHED_FIFO ||
	    thread->sched_class == SCHED_RR) {
		if (cur->sched_class == SCHED_OTHER)
			return true;
		else if (thread->prio > cur->prio)
			return true;
		else
			return false;
	} else if (thread->prio >= cur->prio) {
		return true;
	} else {
		return false;
	}
}

uint32_t
pick_cpu(kthread_t *thread)
{
	if (thread->last_cpu_num != KCPUNUM_NULL &&
	    atomic_cpumask_isset(&idle_cpu_mask, thread->last_cpu_num,
	    memory_order_relaxed)) {
		return thread->last_cpu_num;
	} else {
		uint32_t cpu = atomic_cpumask_first_set(&idle_cpu_mask,
		    memory_order_relaxed);
		if (cpu != KCPUNUM_NULL)
			return cpu;
		else
			return CPU_LOCAL_LOAD(cpu_num);
	}
}

static void
runq_insert(struct kcpu_dispatcher *dp, kthread_t *thread)
{
	if (thread->sched_class == SCHED_FIFO ||
	    thread->sched_class == SCHED_RR) {
		kassert(thread->prio >= PRIO_MIN_RT &&
		    thread->prio < PRIO_LIMIT, "invalid real-time priority");

		ke_spinlock_enter_nospl(&rt_lock);
		TAILQ_INSERT_TAIL(&global_rt_rq[thread->prio - PRIO_MIN_RT],
		    thread, tqlink);
		atomic_fetch_or_explicit(&rt_bitmap,
		    1U << (thread->prio - PRIO_MIN_RT), memory_order_relaxed);

		ke_spinlock_exit_nospl(&rt_lock);
	} else {
		TAILQ_INSERT_TAIL(&dp->rq[thread->prio], thread, tqlink);
		dp->bitmap[thread->prio / 32] |= 1U << (thread->prio % 32);
	}
}

static void do_reschedule(void*)
{
	CPU_LOCAL_STORE(redispatch_requested, true);
	ke_raise_disp_int();
}

/*
 * Resume a blocked thread.
 *
 * Callers must have exclusive custody of the thread (i.e. they are the only one
 * who will try to call ke_thread_resume() on it.)
 */
void
ke_thread_resume(kthread_t *t, bool io_completion)
{
	struct kcpu_dispatcher *disp;
	uint32_t cpunum;
	ipl_t ipl;

	kassert(t->state == TS_SLEEPING || t->state == TS_CREATED,
	    "invalid thread state in ke_resume");

	if (io_completion)
		kep_sched_class[t->sched_class]->io_completed(t);

	ipl = spldisp();

	cpunum = pick_cpu(t);
	disp = &ke_cpu_data[cpunum]->disp;

	ke_spinlock_enter_nospl(&disp->lock);

	t->state = TS_READY;
	runq_insert(disp, t);

	if (should_preempt(disp, t)) {
		ke_spinlock_exit_nospl(&disp->lock);
		if (cpunum == CPU_LOCAL_LOAD(cpu_num))
			do_reschedule(NULL);
		else
			ke_xcall_unicast(do_reschedule, NULL, cpunum);
	} else {
		ke_spinlock_exit_nospl(&disp->lock);
	}

	splx(ipl);
}

static inline int
msb(uint32_t x)
{
	return 31 - __builtin_clz(x);
}

kthread_t *
next_thread(struct kcpu_dispatcher *dp)
{
	int lprio = -1;

	for (int w = (PRIO_LIMIT / 32) - 1; w >= 0; --w) {
		uint32_t bm = dp->bitmap[w];
		if (bm != 0) {
			lprio = w * 32 + msb(bm);
			break;
		}
	}

	if (atomic_load_explicit(&rt_bitmap, memory_order_relaxed)) {
		uint32_t gbm;
		int grt_idx;

		ke_spinlock_enter_nospl(&rt_lock);

		gbm = atomic_load_explicit(&rt_bitmap, memory_order_relaxed);
		grt_idx = (gbm != 0) ? msb(gbm) : -1;

		if (grt_idx != -1 && (grt_idx + PRIO_MIN_RT) >= lprio) {
			runq_t *rq = &global_rt_rq[grt_idx];
			kthread_t *t = TAILQ_FIRST(rq);
			TAILQ_REMOVE(rq, t, tqlink);
			if (TAILQ_EMPTY(rq))
				atomic_fetch_and_explicit(&rt_bitmap,
				    ~(1U << grt_idx), memory_order_relaxed);
			ke_spinlock_exit_nospl(&rt_lock);
			return t;
		}

		ke_spinlock_exit_nospl(&rt_lock);
	}

	if (lprio >= 0) {
		runq_t *rq = &dp->rq[lprio];
		kthread_t *t = TAILQ_FIRST(rq);
		kassert(t != NULL, "dispatcher bitmap and queue out of sync");
		TAILQ_REMOVE(rq, t, tqlink);
		if (TAILQ_EMPTY(rq))
			dp->bitmap[lprio / 32] &= ~(1U << (lprio % 32));
		return t;
	}

	return dp->idle_thread;
}

void
disp_hardclock(void)
{
	struct kcpu_dispatcher *disp = CPU_LOCAL_ADDROF(disp);

	if (atomic_fetch_sub_explicit(&disp->timeslice, 1,
	    memory_order_relaxed) <= 1)
		do_reschedule(NULL);
}

void
ke_dispatch(void)
{
	kthread_t *oldt = CPU_LOCAL_LOAD(curthread), *nextt;
	struct kcpu_dispatcher *disp = CPU_LOCAL_ADDROF(disp);

	kassert(ke_ipl() == IPL_DISP, "ke_dispatch called at invalid IPL");

	kassert(ke_spinlock_held(&oldt->lock),
	    "current thread lock not held in ke_dispatch");
	CPU_LOCAL_STORE(redispatch_requested, false);

	// rcu_quiet();

	ke_spinlock_enter_nospl(&disp->lock);

	if (oldt == disp->idle_thread) {
		kassert(oldt->state == TS_RUNNING,
		    "invalid idle thread state in ke_dispatch");
	}

	if (oldt->state == TS_RUNNING) {
		/* currently running - replace on runqueue */
		oldt->state = TS_READY;
		kep_sched_class[oldt->sched_class]->did_preempt_thread(oldt,
		    atomic_load_explicit(&disp->timeslice,
		    memory_order_relaxed) <= 0);
		runq_insert(disp, oldt);
	} else if (oldt->state == TS_SLEEPING) {
		/* going to sleep. account for sleep time here? */
	} else if (oldt->state == TS_TERMINATED) {
		/* queue it on a done queue, to be invoked from DPC... */
	}

	nextt = next_thread(disp);
	nextt->state = TS_RUNNING;
	atomic_store_explicit(&disp->timeslice,
	    kep_sched_class[nextt->sched_class]->quantum(nextt),
	    memory_order_relaxed);

	if (nextt == oldt) {
		ke_spinlock_exit_nospl(&oldt->lock);
		ke_spinlock_exit_nospl(&disp->lock);
		return;
	}

	nextt->last_cpu_num = CPU_LOCAL_LOAD(cpu_num);
	disp->cur_thread = nextt;
	CPU_LOCAL_STORE(curthread, nextt);
	CPU_LOCAL_STORE(prevthread, oldt);

	if (nextt == disp->idle_thread)
		atomic_cpumask_set(&idle_cpu_mask, CPU_LOCAL_LOAD(cpu_num),
		    memory_order_relaxed);
	else if (oldt == disp->idle_thread)
		atomic_cpumask_clear(&idle_cpu_mask, CPU_LOCAL_LOAD(cpu_num),
		    memory_order_relaxed);

	ke_spinlock_exit_nospl(&disp->lock);

	// if (thread_vm_map(next) != thread_vm_map(old))
	//	pmap_activate(thread_vm_map(next));

	// arch_switch(old, next);

	/* we are now potentially on another CPU, so reload prevthread */
	ke_spinlock_exit_nospl(&CPU_LOCAL_LOAD(prevthread)->lock);
}


void
ke_idle_thread_init(kcpunum_t cpunum, kthread_t *thread)
{
	ipl_t ipl;

	ke_spinlock_init(&thread->lock);

	thread->user = false;

	thread->task = ke_task0;
	// thread->vm_map = NULL;
	// thread->kstack_base = NULL;

	thread->state = TS_RUNNING;
	thread->sched_class = SCHED_OTHER;
	thread->prio = 0;
	thread->last_cpu_num = cpunum;

#if 0
	ipl = ke_spinlock_enter(&proc0.threads_lock);
	TAILQ_INSERT_TAIL(&proc0.threads, thread, threads_qlink);
	proc0.threads_count++;
	ke_spinlock_exit(&proc0.threads_lock, ipl);
#endif

#if 0 // defined (__amd64__) /* TODO: move this out */
	memset(&thread->pcb, 0, sizeof(thread->pcb));
	(*(uint16_t *)(thread->pcb.fpu + 0)) = 0x037f;
	(*(uint32_t *)(thread->pcb.fpu + 24)) = 0x1f80;
#endif
}

void
ke_cpu_init(kcpunum_t cpunum, struct kcpu_data *data,
    struct limine_mp_info *info, kthread_t *idle)
{
	data->self = data;
	data->cpu_num = cpunum;
	data->acpi_id = info->processor_id;
	data->arch.arch_cpu_id = info->arch_cpu_id;

	ke_spinlock_init(&data->dpc_lock);
	TAILQ_INIT(&data->dpc_queue);

	/* todo(low): into callout.c please */
	TAILQ_INIT(&data->callout.callouts);
	data->callout.next_deadline = 0;
	ke_spinlock_init(&data->callout.lock);
	void ki_callout_expiry_dpc(void *, void *);
	ke_dpc_init(&data->callout.expiry_dpc, ki_callout_expiry_dpc,
	    &data->callout, NULL);

	memset((void *)data->xcalls_pending.mask, 0,
	    sizeof(data->xcalls_pending));

	ke_idle_thread_init(cpunum, idle);
	ke_disp_init(cpunum);
}
