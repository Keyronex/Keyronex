/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file ktask.h
 * @brief Kernel task & thread structures and related functionality.
 */

#ifndef ECX_KERN_KTASK_H
#define ECX_KERN_KTASK_H

#include <keyronex/cpu.h>

typedef struct kthread {
	TAILQ_ENTRY(kthread) tqlink;

	kspinlock_t lock;
	enum kthread_state {
		TS_CREATED,
		TS_READY,
		TS_RUNNING,
		TS_SLEEPING,
		TS_TERMINATED
	} state;
	uint8_t sched_class;
	// uint8_t nice;
	uint16_t prio;
	kcpunum_t last_cpu_num;

} kthread_t;

typedef struct ktask {

} ktask_t;

#endif /* ECX_KERN_KTASK_H */
