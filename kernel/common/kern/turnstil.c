/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file turnstil.c
 * @brief Turnstiles
 */

#include <keyronex/ktask.h>
#include <keyronex/kwait.h>

#include <libkern/queue.h>

struct kturnstile {
	SLIST_HEAD(,kturnstile)	*free_link;
	LIST_ENTRY(kturnstile)	hash_link;
	SLIST_ENTRY(kturnstile)	pi_link;
	TAILQ_HEAD(,kturnstile_waiter) waiters;
	uint32_t 	nwaiters;
	void 		*key;
	uint16_t	prio;
	kthread_t	*inheritor;
};

struct kturnstile_waiter {

};

struct kturnstile_chain {
	LIST_HEAD(, kturnstile)	list;
	kspinlock_t lock;
};
