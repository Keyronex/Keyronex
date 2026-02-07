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

#include <keyronex/ktypes.h>

void ke_xcall_broadcast(void (*func)(void *), void *arg);
void ke_xcall_unicast(void (*func)(void *), void *arg, kcpunum_t cpu_num);

#endif /* ECX_KEYRONEX_XCALL_H */
