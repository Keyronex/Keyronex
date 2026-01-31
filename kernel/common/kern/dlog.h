/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dlog.h
 * @brief Kernel debug log APIs.
 */

#ifndef ECX_KERN_DLOG_H
#define ECX_KERN_DLOG_H

void ke_md_early_putc(int c, void *);

int kdprintf(const char *fmt, ...);

#endif /* ECX_KERN_DLOG_H */
