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

#define RT_PRIO_N 32
#define TS_PRIO_N 64

/*
 * Global priority range:
 * 0-63: timesharing
 * 63-127: not used right now (maybe you get a +64 upgrade when blocked
 *   inkernel)
 * 127-159: real-time
 */
#define PRIO_MIN_TS 0
#define PRIO_MAX_TS 63
#define PRIO_MIN_KERNEL 64
#define PRIO_MAX_KERNEL 127
#define PRIO_MIN_RT 128
#define PRIO_MAX_RT 159
#define PRIO_LIMIT 160

struct ksched_class {
	void (*did_preempt_thread)(struct kthread *, bool quantum_expired);
	void (*io_completed)(struct kthread *);

	uint16_t (*quantum)(struct kthread *);
};

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
	kcpunum_t last_cpu_num;
	uint8_t sched_class;
	// uint8_t nice;
	uint16_t prio;
} kthread_t;

typedef struct ktask {

} ktask_t;

#endif /* ECX_KERN_KTASK_H */
