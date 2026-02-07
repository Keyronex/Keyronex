/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file init.c
 * @brief Kernel initialisation.
 */

#include <keyronex/cpu.h>

struct kcpu_data ke_bsp_cpu_data;
struct kcpu_data **ke_cpu_data;
size_t ke_ncpu;
