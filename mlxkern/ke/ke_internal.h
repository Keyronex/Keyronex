#ifndef MLX_KE_KE_INTERNAL_H
#define MLX_KE_KE_INTERNAL_H

#include "ke/ke.h"

/*!
 * @brief Wake up a thread waiting on a waitblock if appropriate.
 * @pre Dispatcher lock held.
 */
bool ki_waiter_maybe_wakeup(kthread_t *thread, kdispatchheader_t *hdr);

/*!
 * @brief Low-level enqueue a timer.
 */
void ki_timer_enqueue(ktimer_t *callout);

/*!
 * @brief Low-level dequeue a timer.
 */
void ki_timer_dequeue(ktimer_t *callout);

#endif /* MLX_KE_KE_INTERNAL_H */
