/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file main.c
 * @brief Entry point to the operating system.
 */

#include <keyronex/dlog.h>
#include <keyronex/proc.h>
#include <keyronex/limine.h>

/* kern/init.c */
void ke_bsp_early_init(ktask_t *, kthread_t *);

/* vm/init.c */
void vm_phys_init(void);

__attribute__((used, section(".requests_start_marker")))
static volatile uint64_t start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests")))
static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests")))
static volatile struct limine_mp_request
    smp_request = { .id = LIMINE_MP_REQUEST_ID, .revision = 0 };

__attribute__((used, section(".requests_end_marker")))
static volatile uint64_t end_marker[] = LIMINE_REQUESTS_END_MARKER;

proc_t proc0;
thread_t thread0;

void
_start(void)
{
	ke_bsp_early_init(&proc0.ktask, &thread0.kthread);

	kdprintf("Keyronex\n");

	vm_phys_init();

	for (;;) ;
}
