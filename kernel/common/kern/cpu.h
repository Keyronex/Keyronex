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

#include <kern/cpulocal.h>
#include <kern/intr.h>

#include <stdatomic.h>
#include <stdint.h>

struct kcpu_data {
	struct karch_cpu_data arch;
	struct kcpu_data *self;

	_Atomic(uint32_t) pending_soft_ints;
	ipl_t ipl;

	kspinlock_t dpc_lock;
	TAILQ_HEAD(, kdpc) dpc_queue;

	bool redispatch_requested;
};

#define ke_current_cpu() CPU_LOCAL_GET()

#endif /* ECX_KERN_CPU_H */
