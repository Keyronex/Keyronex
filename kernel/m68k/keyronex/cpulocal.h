/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file cpulocal.h
 * @brief m68k CPU-local data handling
 */

#ifndef ECX_KEYRONEX_CPULOCAL_H
#define ECX_KEYRONEX_CPULOCAL_H

#include <stdint.h>

struct karch_cpu_data {
};

#define CPU_LOCAL_OFFSET(FIELD) __builtin_offsetof(struct kcpu_data, FIELD)

#define CPU_LOCAL_LOAD(FIELD) (ke_bsp_cpu_data.FIELD)

#define CPU_LOCAL_STORE(FIELD, VALUE) ke_bsp_cpu_data.FIELD = VALUE

#define CPU_LOCAL_GET() (&ke_bsp_cpu_data)

#define CPU_LOCAL_ADDROF(FIELD) (&(ke_bsp_cpu_data.FIELD))

#endif /* ECX_KEYRONEX_CPULOCAL_H */
