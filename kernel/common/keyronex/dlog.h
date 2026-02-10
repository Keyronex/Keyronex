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

#include <stdarg.h>
#include <stddef.h>

void ke_md_early_putc(int c, void *);

__attribute__((noreturn)) void kfatal_internal(const char *file, int line,
    const char *fmt, ...);
int kdprintf(const char *fmt, ...);
int kvsnprintf(char *str, size_t size, const char *fmt, va_list ap);

#define kfatal(...) kfatal_internal(__FILE__, __LINE__, __VA_ARGS__)
#define kunreachable() kfatal("Reached unreachable code")
#define kassert(TEST, MSG) if (!(TEST)) \
	kfatal("Assertion failed: %s", MSG)
#define kassert_dbg(TEST, MSG) kassert(TEST, MSG)

#endif /* ECX_KERN_DLOG_H */
