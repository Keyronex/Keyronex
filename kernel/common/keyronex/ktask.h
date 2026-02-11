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

#include <libkern/queue.h>

#include <keyronex/cpu.h>

struct limine_mp_info;

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

typedef struct kturnstile kturnstile_t;

/*
 * A kernel thread.
 *
 * l: kthread::lock
 */
typedef struct kthread {
	LIST_ENTRY(kthread)	proc_link;
	TAILQ_ENTRY(kthread)	tqlink;

	bool user;		/* is this a user thread? (FPU save/restore) */

	struct ktask	*task;	/* task this thread belongs to */
	struct ktask	*attached_task; /* task temporarily attached to */

	kspinlock_t lock;
	enum kthread_state {
		TS_CREATED,
		TS_READY,
		TS_RUNNING,
		TS_SLEEPING,
		TS_TERMINATED
	} state;
	kcpunum_t	last_cpu_num;	/* CPU running on/last ran on */
	uint8_t		sched_class;	/* scheduling class (SCHED_*) */
	uint8_t		nice;		/* nice value (currently unused) */
	uint16_t	prio;		/* prio determined by scheduler */
	uint16_t 	inherited_prio;	/* prio inherited from turnstile */
	uint16_t	effective_prio;	/* max of prio and inherited_prio */
	SLIST_HEAD(, kturnstile) pi_head; /* l: turnstiles donating priority */

	kwait_internal_status_t	wait_status;	/* wait status */
	struct kwaitblock	integral_waitblocks[4]; /* default waitblocks */
	const char 		*wait_reason;	/* reason for waiting */
} kthread_t;

typedef struct ktask {
	LIST_HEAD(, kthread) threads;
	paddr_t		pmap;
} ktask_t;

void ke_dispatch(void);
void ke_thread_resume(kthread_t *t, bool io_completion);

void ke_disp_global_init(void);
void ke_disp_init(kcpunum_t cpunum);
void ke_cpu_init(kcpunum_t cpunum, struct kcpu_data *data, struct limine_mp_info *info, kthread_t *idle);

extern ktask_t *ke_task0;

#endif /* ECX_KERN_KTASK_H */
