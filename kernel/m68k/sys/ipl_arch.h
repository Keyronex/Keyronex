/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file cpulocal.h
 * @brief m68k IPL
 */

#ifndef ECX_KEYRONEX_IPL_ARCH_H
#define ECX_KEYRONEX_IPL_ARCH_H

typedef enum ipl {
	IPL_0,

	IPL_DISP = 2,

	IPL_M68K_0 = IPL_DISP, /* this & below are 0*/
	IPL_M68K_1,
	IPL_M68K_2,
	IPL_M68K_3,
	IPL_M68K_4,
	IPL_M68K_5,
	IPL_M68K_6,
	IPL_M68K_7,

	IPL_HIGH = IPL_M68K_7
} ipl_t;

#endif /* ECX_KEYRONEX_IPL_ARCH_H */
