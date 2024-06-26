#ifndef KRX_NANOKERN_KI_H
#define KRX_NANOKERN_KI_H

#include "kdk/nanokern.h"

/*!
 * Upcall from MD to Ke - process the DPC queue on a CPU.
 *
 * \pre IPL=dpc
 */
void ki_dispatch_dpcs(kcpu_t *cpu);

/*!
 * Upcall from MD to Ke - tick the local hard clock.
 *
 * \pre IPL=high
 */
bool ki_cpu_hardclock(md_intr_frame_t *frame, void *arg);

/*! @brief Initialise a kcpu structure. */
void ki_cpu_init(kcpu_t *cpu, kthread_t *idle_thread);

/*!
 * @brief Release a thread for processing a port.
 *
 * This will decrement the count of threads currently processing on this port,
 * and try to release a waiting thread to process data on the port.
 *
 * To be called when a thread which was processing data on a port goes away or
 * has to sleep to wait for resources.
 *
 * @param wb_queue Queue of threads which should be passed to ki_wake_waiters.
 * @retval True if at least one thread was woken.
 */
bool ki_port_thread_release(kport_t *kport, kwaitblock_queue_t *wb_queue);

/*!
 * @brief Wake a waiting thread.
 * \pre Scheduler lock held
 */
void ki_wake_waiter(kthread_t *thread);

/*!
 * @brief Wake waiters previously queued up by ki_signal()
 * \pre Scheduler lock held
 */
void ki_wake_waiters(kwaitblock_queue_t *queue);

/*!
 * @brief Signal an object; satisfies waiters and places them on @p wakeQueue.
 * \pre Object lock held
 */
void ki_signal(kdispatchheader_t *hdr, kwaitblock_queue_t *wakeQueue);

enum ki_satisfy_attempt_result {
	kDidSatisfyPreWait,
	kDidSatisfyWait,
	kWasAlreadySatisfied,
};

/*! @brief Try to satisfy a waitblock. */
enum ki_satisfy_attempt_result ki_waitblock_try_to_satisfy(kwaitblock_t *wb);

void ki_thread_common_init(kthread_t *thread, kcpu_t *last_cpu,
    kprocess_t *proc, const char *name);

/*!
 * @brief Resume a thread.
 * @pre Scheduler lock held.
 */
void ki_thread_resume_locked(kthread_t *thread);

void ki_timer_enqueue_locked(ktimer_t *callout);
bool ki_timer_dequeue_locked(ktimer_t *callout);

void md_send_invlpg_ipi(kcpu_t *cpu);
void md_raise_dpc_interrupt(void);
void md_cpu_init(kcpu_t *cpu);

/*!
 * Reschedule this core.
 *
 * \pre Dispatcher DB locked.
 */
void ki_reschedule(void);

void ki_rcu_quiet();
void ki_rcu_per_cpu_init(struct ki_rcu_per_cpu_data *data);

void ki_tlb_flush_vaddr_locally(uintptr_t vaddr);
void ki_tlb_flush_vaddr_globally(uintptr_t vaddr);

void ki_enter_user_mode(uintptr_t ip, uintptr_t sp);

#endif /* KRX_NANOKERN_KI_H */
