/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file cpulocal.h
 * @brief AMD64 IPL
 */

#ifndef ECX_KEYRONEX_IPL_ARCH_H
#define ECX_KEYRONEX_IPL_ARCH_H

typedef enum ipl {
	IPL_0,

	IPL_DISP = 2,

	IPL_HIGH = 15
} ipl_t;

#endif /* ECX_KEYRONEX_IPL_ARCH_H */
