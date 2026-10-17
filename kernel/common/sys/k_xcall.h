/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2024 Cloudarox Solutions.
 */
/*!
 * @file xcall.h
 * @brief Cross-processor call functionality.
 */

#ifndef ECX_KEYRONEX_XCALL_H
#define ECX_KEYRONEX_XCALL_H

#include <sys/k_types.h>


void ke_xcall_broadcast(void (*func)(void *), void *arg);
void ke_xcall_unicast(void (*func)(void *), void *arg, kcpunum_t cpu_num);
void ke_xcall_reschedule(kcpunum_t cpu_num);

void kep_xcall_handler(void *unused);
void kep_xcall_reschedule_handler(void);

#endif /* ECX_KEYRONEX_XCALL_H */
