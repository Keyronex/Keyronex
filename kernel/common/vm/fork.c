/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file fork.c
 * @brief Virtual memory fork operation.
 */

#include <vm/map.h>
#include <vm/page.h>

kspinlock_t anon_creation_lock;
kspinlock_t anon_stealing_lock;
