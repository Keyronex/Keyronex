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

#include <sys/k_cpu.h>
#include <sys/pcb.h>

struct limine_mp_info;

typedef struct ksyncops ksyncops_t;
typedef struct kturnstile kturnstile_t;

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

typedef uint16_t kpri_t;

struct ksched_class {
	void (*did_preempt_thread)(struct kthread *, bool quantum_expired);
	void (*io_completed)(struct kthread *);

	uint16_t (*quantum)(struct kthread *);
};

typedef struct kturnstile {
	union {
		SLIST_HEAD(, kturnstile) freelist;
		SLIST_ENTRY(kturnstile) free_link;
	};
	LIST_ENTRY(kturnstile)	hash_link;
	SLIST_ENTRY(kturnstile)	pi_link;
	TAILQ_HEAD(, kturnstile_waiter) waiters[2];
	uint32_t 	nwaiters[2];
	void 		*obj;

	struct kthread	*owner;
	struct kthread	*inheritor;
	kpri_t		pri;
} kturnstile_t;

typedef struct krwlock {
	uintptr_t val;
} krwlock_t;

typedef struct kmutex {
	uintptr_t val;
} kmutex_t;

/*
 * A kernel thread.
 *
 * l: kthread::lock
 */
typedef struct kthread {
	LIST_ENTRY(kthread)	proc_link;
	TAILQ_ENTRY(kthread)	tqlink;

	karch_pcb_t	pcb;		/* saved state */
	void 		*kstack_base;	/* kernel stack base */
	bool 		user;		/* user thread? (do FPU save/restore) */

	struct ktask	*task;	/* task this thread belongs to */

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
	bool			wait_signallable; /* is wait interruptilble? */

	kturnstile_t	*turnstile;	/* my turnstile */
	void 		*waiting_on;	/* object waited on */
} kthread_t;

typedef struct ktask {
	LIST_HEAD(, kthread) threads;
} ktask_t;

void ke_dispatch(void);
void ke_thread_resume(kthread_t *, bool io_completion);

void ke_thread_init(kthread_t *, ktask_t *, kturnstile_t *ts, void *stack_base,
    struct karch_trapframe *forkframe, void (*func)(void *), void *arg);

kpri_t ke_thread_epri_locked(kthread_t *);
void ke_thread_set_ipri_locked(kthread_t *, kpri_t);

void ke_disp_global_init(void);
void ke_disp_init(kcpunum_t cpunum);
void ke_cpu_init(kcpunum_t cpunum, struct kcpu_data *data,
    struct limine_mp_info *info, kthread_t *idle);

void kep_arch_switch(struct kthread *old, struct kthread *new);
void kep_arch_thread_init(kthread_t *thread, void *stack_base,
    struct karch_trapframe *forkframe, void (*func)(void *), void *arg);

void kep_turnstile_init(void);
ipl_t ke_turnstile_lookup(void *obj, kturnstile_t **out);
kthread_t *ke_turnstile_waiter(kturnstile_t *, bool writer);
void ke_turnstile_block(kturnstile_t *, bool writer, void *obj,
    kthread_t *owner, ipl_t);
void ke_turnstile_wakeup(kturnstile_t *, bool writer, int count,
    kthread_t *newowner, ipl_t);
void ke_turnstile_exit(void *obj, ipl_t);

void ke_rwlock_init(krwlock_t *);
void ke_rwlock_enter_read(krwlock_t *, const char *reason);
void ke_rwlock_enter_write(krwlock_t *, const char *reason);
void ke_rwlock_exit_read(krwlock_t *);
void ke_rwlock_exit_write(krwlock_t *);
void ke_rwlock_downgrade(krwlock_t *);
bool ke_rwlock_tryupgrade(krwlock_t *);

void ke_mutex_init(kmutex_t *);
void ke_mutex_enter(kmutex_t *, const char *reason);
void ke_mutex_exit(kmutex_t *);
bool ke_mutex_tryenter(kmutex_t *);

extern ktask_t *ke_task0;

#endif /* ECX_KERN_KTASK_H */
