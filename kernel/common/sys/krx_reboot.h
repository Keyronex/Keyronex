/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file krx_reboot.h
 * @brief Rebooting
 */

#ifndef ECX_KEYRONEX_krx_reboot_H
#define ECX_KEYRONEX_krx_reboot_H

#include <sys/krx_atomic.h>
#include <sys/reboot.h>

int sys_reboot(int cmd);

extern atomic_bool os_rebooting;

#endif /* ECX_KEYRONEX_krx_reboot_H */
