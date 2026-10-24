/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file reboot.c
 * @brief Rebooting
 *
 * - unceremoniously terminate every process but the caller;
 * - start reboot thread;
 * - terminates the caller.
 * - reboot thread awaits last process to terminate;
 * - unmounts everything in reverse mount order;
 * - syncs;
 * - checks nothing remains that shouldn't;
 * - powers off.
 */

#include <sys/errno.h>
#include <sys/k_cpu.h>
#include <sys/k_intr.h>
#include <sys/k_log.h>
#include <sys/k_rcu.h>
#include <sys/k_wait.h>
#include <sys/krx_vfs.h>
#include <sys/krx_reboot.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#include <uacpi/sleep.h>

atomic_bool os_rebooting;
static int reboot_cmd;

extern atomic_uint files_allocated, files_freed;
extern atomic_uint vnodes_allocated, vnodes_freed;

void
halt(void)
{
	ke_arch_disable();
	for (;;) {
#if defined(__amd64__)
		asm volatile("hlt");
#endif
	}
}

static void
killall(void *ignore)
{
	kabstime_t deadline = ke_time() + (kabstime_t)30 * NS_PER_S;
	size_t alive;

	while ((alive = proc_terminate_all(ignore)) > 0) {
		kevent_t never;

		if (ke_time() > deadline) {
			debug_proc_dump_all(ignore);
			kfatal("reboot: %zu processes left\n", alive);
		}

		ke_event_init(&never, false);
		ke_wait1(&never, "killall", false, ke_time() + NS_PER_S / 4);
	}
}

static void
reboot_thread(void *)
{
	int r;

	r = vfs_sync_all(VN_FSYNC_DURABLE);
	if (r != 0)
		kdprintf("reboot: sync failed: %d\n", r);

	vfs_unmount_all();

	ke_rcu_synchronise(); /* clear up RCU-freed items */

	kdprintf("reboot: files alloced=%u freed=%u "
	   "vnodes alloced=%u freed=%u\n",
	    atomic_load_explicit(&files_allocated, memory_order_relaxed),
	    atomic_load_explicit(&files_freed, memory_order_relaxed),
	    atomic_load_explicit(&vnodes_allocated, memory_order_relaxed),
	    atomic_load_explicit(&vnodes_freed, memory_order_relaxed));

	kassert(atomic_load_explicit(&files_allocated, memory_order_relaxed) ==
	    atomic_load_explicit(&files_freed, memory_order_relaxed),
	    "reboot: file objects remain");

	switch (reboot_cmd) {
	case RB_HALT_SYSTEM:
		kdprintf("reboot: halting\n");
		halt();

	case RB_AUTOBOOT:
		kdprintf("reboot: rebooting\n");
		ke_arch_disable();
		uacpi_reboot();
		kdprintf("reboot: ACPI reboot failed, halting\n");
		halt();

	case RB_POWER_OFF: {
		uacpi_status st;

		st = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
		if (st == UACPI_STATUS_OK) {
			ke_arch_disable();
			st = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
		}

		kdprintf("reboot: ACPI poweroff failed: %s, halting\n",
		    uacpi_status_to_string(st));
		halt();
	}
	}
}

int
sys_reboot(int cmd)
{
	thread_t *thread;

	switch (cmd) {
	case RB_POWER_OFF:
	case RB_HALT_SYSTEM:
	case RB_AUTOBOOT:
		break;
	default:
		return -EINVAL;
	}

	if (atomic_exchange_explicit(&os_rebooting, true,
		memory_order_acq_rel) != 0)
		return -EBUSY;

	reboot_cmd = cmd;

	kdprintf("reboot: requested by pid %d (%s)\n", curproc()->pid,
	    curproc()->comm);

	killall(curproc());

	thread = proc_new_system_thread(reboot_thread, NULL);
	if (thread == NULL)
		kfatal("reboot: couldn't create the reboot thread\n");
	ke_thread_resume(&thread->kthread, false);

	proc_terminate(curproc(), W_EXITCODE(0, 0));
	sys_thread_exit();
	kunreachable();
}
