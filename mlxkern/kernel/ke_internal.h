/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 15 2023.
 */

#ifndef MLX_KE_KE_INTERNAL_H
#define MLX_KE_KE_INTERNAL_H

#include "kdk/kernel.h"

/*!
 * @brief Set up a kernel thread to run a function with argument.
 */
void kmd_thread_init(struct kthread *thread, void (*start_fun)(void *),
    void *start_arg);

/*!
 * @brief Common initialisation for handcrafted and normal threads.
 */
void
ki_thread_common_init(kthread_t *thread, kcpu_t *cpu, kprocess_t *proc,
    const char *name);

/*!
 * @brief Initialise a thread.
 */
int ki_thread_init(kthread_t *thread, kprocess_t *process, const char *name, void (*start)(void*), void *arg);

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
