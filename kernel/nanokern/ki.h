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
 * @brief Wake up a thread waiting on a waitblock if appropriate.
 * @pre Dispatcher lock held.
 */
bool ki_waiter_maybe_wakeup(kthread_t *thread, kdispatchheader_t *hdr);

void ki_thread_common_init(kthread_t *thread, kcpu_t *last_cpu, void *proc,
    const char *name);

/*!
 * @brief Resume a thread.
 * @pre Dispatcher lock held.
 */
void ki_thread_resume_locked(kthread_t *thread);

void ki_timer_enqueue(ktimer_t *callout);
void ki_timer_dequeue(ktimer_t *callout);

void md_raise_dpc_interrupt(void);
void md_cpu_init(kcpu_t *cpu);

/*!
 * Reschedule this core.
 *
 * \pre Dispatcher DB locked.
 */
void ki_reschedule(void);

#endif /* KRX_NANOKERN_KI_H */
