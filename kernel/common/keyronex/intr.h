/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file intr.h
 * @brief Interrupt handling related definitions.
 */

#ifndef ECX_KERN_INTR_H
#define ECX_KERN_INTR_H

#include <keyronex/atomic.h>
#include <keyronex/ipl_arch.h>
#include <keyronex/ktypes.h>

#include <libkern/queue.h>

#include <stdbool.h>
#include <stdint.h>

struct kcpu_data;

typedef struct kspinlock {
	uint32_t lock;
} kspinlock_t;

typedef struct kdpc {
	TAILQ_ENTRY(kdpc) qlink;
	_Atomic(struct kcpu_data *) cpu;
	void (*handler)(void *, void *);
	void *arg1, *arg2;
} kdpc_t;

typedef struct kirq_source {
	uint32_t source;
	bool low_polarity;
	bool edge;
} kirq_source_t;

typedef struct kirq {
	kirq_source_t source;
	LIST_ENTRY(kirq) list_entry;
	kcpunum_t cpu;
	uint32_t vector;
} kirq_t;

ipl_t ke_ipl(void);
ipl_t splraise(ipl_t);
void splx(ipl_t);
/* ipl_t spldisp(void); */
#define spldisp() splraise(IPL_DISP)
/* ipl_t splhigh(void) */
#define splhigh() splraise(IPL_HIGH)

bool ke_arch_disable(void);
void ke_arch_enable(bool);

void ke_dpc_init(kdpc_t *, void (*)(void *, void *), void *arg1, void *arg2);
void ke_dpc_schedule(kdpc_t *);

void ke_raise_disp_int(void);

#define KSPINLOCK_INITIALISER { 0 }

static inline bool
ke_spinlock_held(kspinlock_t *lock)
{
	return lock->lock != 0;
}

static inline void
ke_spinlock_init(kspinlock_t *lock)
{
	lock->lock = 0;
}

static inline void
ke_spinlock_enter_nospl(kspinlock_t *lock)
{
	while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE))
		;
}

static inline bool
ke_spinlock_tryenter_nospl(kspinlock_t *lock)
{
	return !__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE);
}

static inline void
ke_spinlock_exit_nospl(kspinlock_t *lock)
{
	__atomic_clear(&lock->lock, __ATOMIC_RELEASE);
}

static inline ipl_t
ke_spinlock_enter(kspinlock_t *lock)
{
	ipl_t ipl = spldisp();
	ke_spinlock_enter_nospl(lock);
	return ipl;
}

static inline void
ke_spinlock_exit(kspinlock_t *lock, ipl_t ipl)
{
	ke_spinlock_exit_nospl(lock);
	splx(ipl);
}

#endif /* ECX_KERN_INTR_H */
