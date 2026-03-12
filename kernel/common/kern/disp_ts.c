/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file sched_ts.c
 * @brief Timesharing thread scheduling class.
 */

#include <sys/k_cpu.h>
#include <sys/k_thread.h>
#include <sys/k_log.h>

struct ts_prio_info {
	uint8_t quantum;
	uint8_t quantum_expiry_prio;
	uint8_t prio_after_io;
};

static struct ts_prio_info ts_prio_info[TS_PRIO_N] = {
	/* q,	q_e_p,	q_a_i,	 */
	{ 12,	0,	55 },
	{ 12,	0,	55 },
	{ 12,	0,	55 },
	{ 12,	0,	55 },
	{ 12,	0,	55 },
	{ 12,	0,	55 },
	{ 12,	0,	55 },
	{ 12,	0,	55 },

	{ 10,	0,	56 },
	{ 10,	1,	56 },
	{ 10,	2,	56 },
	{ 10,	3,	56 },
	{ 10,	4,	56 },
	{ 10,	5,	56 },
	{ 10,	6,	56 },
	{ 10,	7,	56 },

	{ 8,	8,	57 },
	{ 8,	9,	57 },
	{ 8,	10,	57 },
	{ 8,	11,	57 },
	{ 8,	12,	57 },
	{ 8,	13,	57 },
	{ 8,	14,	57 },
	{ 8,	15,	57 },

	{ 6,	16,	58 },
	{ 6,	17,	58 },
	{ 6,	18,	58 },
	{ 6,	19,	58 },
	{ 6,	20,	58 },
	{ 6,	21,	58 },
	{ 6,	22,	58 },
	{ 6,	23,	58 },

	{ 5,	24,	59 },
	{ 5,	25,	59 },
	{ 5,	26,	59 },
	{ 5,	27,	59 },
	{ 5,	28,	59 },
	{ 5,	29,	59 },
	{ 5,	30,	59 },
	{ 5,	31,	59 },

	{ 4,	32,	60 },
	{ 4,	33,	60 },
	{ 4,	34,	60 },
	{ 4,	35,	60 },
	{ 4,	36,	60 },
	{ 4,	37,	60 },
	{ 4,	38,	60 },
	{ 4,	39,	60 },

	{ 3,	40,	61 },
	{ 3,	41,	61 },
	{ 3,	42,	61 },
	{ 3,	43,	61 },
	{ 3,	44,	61 },
	{ 3,	45,	61 },
	{ 3,	46,	61 },
	{ 3,	47,	61 },

	{ 3,	48,	62 },
	{ 3,	49,	62 },
	{ 3,	50,	62 },
	{ 3,	51,	62 },
	{ 3,	52,	62 },
	{ 3,	53,	62 },
	{ 3,	54,	62 },
	{ 2,	55,	63 },
};

static void
ts_did_preempt_thread(kthread_t *t, bool quantum_expired)
{
	kassert(t->base_prio <= PRIO_MAX_TS, "bad TS base prio");
	if (quantum_expired)
		t->base_prio = ts_prio_info[t->base_prio].quantum_expiry_prio;
	t->effective_prio = t->base_prio > t->inherited_prio ?
	    t->base_prio : t->inherited_prio;
}

static void
ts_io_completed(kthread_t *t)
{
	kassert(t->base_prio <= PRIO_MAX_TS, "bad TS base prio");
	t->base_prio = ts_prio_info[t->base_prio].prio_after_io;
	t->effective_prio = t->base_prio > t->inherited_prio ?
	    t->base_prio : t->inherited_prio;
}

static uint16_t
ts_quantum(kthread_t *t)
{
	kassert(t->base_prio <= PRIO_MAX_TS, "bad TS base prio");
	return ts_prio_info[t->base_prio].quantum;
}

struct ksched_class kep_ts_class = {
	.did_preempt_thread = ts_did_preempt_thread,
	.io_completed = ts_io_completed,
	.quantum = ts_quantum
};
