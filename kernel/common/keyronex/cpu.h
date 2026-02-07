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

struct kcpu_data {
	struct karch_cpu_data arch;
	struct kcpu_data *self;
	kcpunum_t cpu_num;

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
	struct kcpu_dispatcher *disp;
	bool redispatch_requested;
};

#define ke_curcpu() CPU_LOCAL_GET()
#define ke_curthread() CPU_LOCAL_LOAD(curthread)

kabstime_t ke_time();

extern struct kcpu_data ke_bsp_cpu_data;
extern struct kcpu_data **ke_cpu_data;
extern size_t ke_ncpu;

#endif /* ECX_KERN_CPU_H */
