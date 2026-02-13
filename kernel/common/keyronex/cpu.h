/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file cpu.h
 * @brief CPU-local data and related definitions.
 */

#ifndef ECX_KERN_CPU_H
#define ECX_KERN_CPU_H

#include <keyronex/cpulocal.h>
#include <keyronex/intr.h>
#include <keyronex/kwait.h>

#include <libkern/lib.h>

#include <stdatomic.h>
#include <stdint.h>

struct kthread;

#define KERN_HZ 64

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
	struct kthread *idle_thread;
	struct kthread *cur_thread;
	atomic_int_fast32_t timeslice;
};

struct kcpu_data {
	struct karch_cpu_data arch;
	struct kcpu_data *self;
	kcpunum_t cpu_num;
	uint32_t acpi_id;

	/* interrupt management */
	ipl_t ipl;
	_Atomic(uint32_t) pending_soft_ints;
	kspinlock_t dpc_lock;
	TAILQ_HEAD(, kdpc) dpc_queue;
	struct kcpu_callout callout;

	/* xcall handling */
	void (*func)(void *);
	void *arg;
	atomic_uint xcalls_completed;
	katomic_cpumask_t xcalls_pending;

	/* dispatcher */
	struct kthread *curthread;
	struct kthread *prevthread;
	struct kcpu_dispatcher disp;
	bool redispatch_requested;
};

#define ke_curcpu() CPU_LOCAL_GET()
#define ke_curthread() CPU_LOCAL_LOAD(curthread)

kabstime_t ke_time();
void kep_arch_ipi_unicast(kcpunum_t cpu_num);
void kep_arch_ipi_broadcast(void);
void ke_arch_pause(void);

extern struct kcpu_data ke_bsp_cpu_data;
extern struct kcpu_data **ke_cpu_data;
extern size_t ke_ncpu;

#endif /* ECX_KERN_CPU_H */
