/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 15 2023.
 */

#ifndef MLX_KE_KE_INTERNAL_H
#define MLX_KE_KE_INTERNAL_H

#include "kernel/ke.h"

/*!
 * @brief Set up a kernel thread to run a function with argument.
 */
void kmd_thread_init(struct kthread *thread, void (*start_fun)(void *),
    void *start_arg);

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
