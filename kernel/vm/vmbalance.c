/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sat Apr 27 2024.
 */
/*!
 * @file vmbalance.c
 * @brief Virtual memory balancing thread.
 */

#include "kdk/executive.h"
#include "kdk/nanokern.h"
#include "kdk/queue.h"
#include "vm/vmp_dynamics.h"
#include "vmp.h"

TAILQ_HEAD(vmp_trim_queue, vm_procstate);
struct vmp_trim_queue vmp_trim_queue = TAILQ_HEAD_INITIALIZER(vmp_trim_queue);
kspinlock_t vmp_trim_queue_lock = KSPINLOCK_INITIALISER;
static uint32_t trim_counter = 0;

kevent_t vmp_balance_set_scheduler_event = KEVENT_INITIALISER(
	     vmp_balance_set_scheduler_event),
	 vmp_writer_event = KEVENT_INITIALISER(vmp_writer_event);
kthread_t *vmp_balancer_thread, *vmp_writeback_thread;
void vmp_balancer(void *), vmp_writeback(void *);

void
vmp_paging_init(void)
{
	ps_create_kernel_thread(&vmp_balancer_thread, "vm balance set manager",
	    vmp_balancer, NULL);
	ps_create_kernel_thread(&vmp_writeback_thread,
	    "vm dirty writer daemon", vmp_writeback, NULL);

	ke_thread_resume(vmp_balancer_thread);
	ke_thread_resume(vmp_writeback_thread);
}

static void
trim_working_sets(bool urgent)
{
	trim_counter++;

	while (true) {
		vm_procstate_t *vmps;
		ipl_t ipl;

		/*
		 * NOTE TO SELF:
		 * - General lock order of working set locks -> trim queue lock
		 * - The trim queue lock will protect certain fields of the
		 * vm_procstate
		 * - Deletion of a vm_procstate will have to be held up until
		 * after the VM balance set manager has finished dealing with
		 * it.
		 * - Probably want a fag in the vm_procstate, protected by the
		 * trim queue lock, that is set when the the VM balance set
		 * manager takes a vm_procstate for inspection, causing the
		 * deletion of the vm_procstate to be delayed.
		 * - The deleter of the vm_procstate could perhaps insert a
		 * pointer to a kevent in to the vm_procstate, and wait on that,
		 * with ourselves signalling it at the needful time and not
		 * replacing the vm_procstate on the trim queue?
		 */

		ipl = ke_spinlock_acquire(&vmp_trim_queue_lock);
		vmps = TAILQ_FIRST(&vmp_trim_queue);
		if (vmps->last_trim_counter == trim_counter) {
			/* all working sets visited */
			ke_spinlock_release(&vmp_trim_queue_lock, ipl);
			return;
		}
		vmps->last_trim_counter = trim_counter;
		TAILQ_REMOVE(&vmp_trim_queue, vmps, trim_queue_entry);
		ke_spinlock_release(&vmp_trim_queue_lock, ipl);

		/* do the trimming.... */

		ipl = ke_spinlock_acquire(&vmp_trim_queue_lock);
		TAILQ_INSERT_TAIL(&vmp_trim_queue, vmps, trim_queue_entry);
		ke_spinlock_release(&vmp_trim_queue_lock, ipl);
	}
}

void
vmp_balancer(void *)
{
	while (true) {
		kwaitresult_t w = ke_wait(&vmp_balance_set_scheduler_event,
		    "vmp_balance_set_scheduler_event", false, false, NS_PER_S);
		bool urgent = w == 0;

#if 0
		trim_working_sets(urgent);
#endif
	}
}
