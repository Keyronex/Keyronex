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
#include <libkern/lib.h>

#include <stdatomic.h>
#include <stdint.h>

#define MAX_CPUS 256
#define CPUMASK_WIDTH (MAX_CPUS / (sizeof(uintptr_t) * 8))

typedef uint32_t kcpunum_t;

typedef struct kcpumask {
	uintptr_t mask[CPUMASK_WIDTH];
} kcpumask_t;

typedef struct katomic_cpumask {
	atomic_uintptr_t mask[CPUMASK_WIDTH];
} katomic_cpumask_t;

struct kcpu_data {
	struct karch_cpu_data arch;
	struct kcpu_data *self;
	kcpunum_t cpu_num;

	_Atomic(uint32_t) pending_soft_ints;
	ipl_t ipl;

	kspinlock_t dpc_lock;
	TAILQ_HEAD(, kdpc) dpc_queue;

	struct kthread *curthread;
	struct kcpu_dispatcher *disp;
	bool redispatch_requested;
};

#define KCPUNUM_NULL ((kcpunum_t)-1)

#define ke_current_cpu() CPU_LOCAL_GET()

extern struct kcpu_data ke_bsp_cpu_data;
extern struct kcpu_data **ke_cpu_data;
extern size_t ke_ncpu;

#endif /* ECX_KERN_CPU_H */
