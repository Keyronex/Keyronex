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

#include <libkern/lib.h>
#include <libkern/queue.h>
#include <stdatomic.h>

#define UINTPTR_BITS (sizeof(uintptr_t) * 8)

#define RT_PRIO_N 32
#define TS_PRIO_N 64

/*
 * Global priority range:
 * 0-63: timesharing
 * 63-127: kernel
 * 127-159: real-time
 */
#define PRIO_MIN_TS 0
#define PRIO_MAX_TS 63
#define PRIO_MIN_KERNEL 64
#define PRIO_MAX_KERNEL 127
#define PRIO_MIN_RT 128
#define PRIO_MAX_RT 159
#define PRIO_LIMIT 160

typedef TAILQ_HEAD(kthread_tq, kthread) runq_t;

struct kcpu_dispatcher {
	kspinlock_t lock;
	uint32_t bitmap[PRIO_LIMIT / 32];
	runq_t rq[PRIO_LIMIT];
	kthread_t *idle_thread;
	kthread_t *cur_thread;
	atomic_int_fast32_t timeslice;
};

static kspinlock_t rt_lock;
static atomic_int_fast32_t rt_bitmap;
static runq_t global_rt_rq[RT_PRIO_N];
static katomic_cpumask_t idle_cpu_mask;
static struct kcpu_dispatcher bsp_dispatcher;

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
			kcpunum_t cpu = i * UINTPTR_BITS + __builtin_ctz(mask);
			return cpu < ke_ncpu ? cpu : KCPUNUM_NULL;
		}
	}
	return KCPUNUM_NULL;
}

void
disp_global_init(void)
{
	memset(&idle_cpu_mask, 0xff, sizeof(idle_cpu_mask));
	for (size_t i = 0; i < RT_PRIO_N; i++)
		TAILQ_INIT(&global_rt_rq[i]);
}

void
disp_init(kcpunum_t cpunum)
{
	struct kcpu_dispatcher *disp = ke_cpu_data[cpunum]->disp;
	size_t i;

	ke_spinlock_init(&disp->lock);

	for (i = 0; i < TS_PRIO_N; i++)
		TAILQ_INIT(&disp->rq[i]);

	disp->idle_thread = ke_cpu_data[cpunum]->curthread;
	disp->cur_thread = disp->idle_thread;
	atomic_store_explicit(&disp->timeslice, 5, memory_order_relaxed);
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
				rt_bitmap &= ~(1U << grt_idx);
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
