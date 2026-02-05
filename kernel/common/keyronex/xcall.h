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

#include <keyronex/cpu.h>

void xcall_broadcast(void (*func)(void *), void *arg);
void xcall_unicast(void (*func)(void *), void *arg, kcpunum_t cpu_num);

#endif /* ECX_KEYRONEX_XCALL_H */
