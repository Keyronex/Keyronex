/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dlog.c
 * @brief Kernel debug log.
 */

#include <keyronex/dlog.h>

#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS	1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS	1
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS		0
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS 		1
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS		1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS		1
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS	0

#define NANOPRINTF_IMPLEMENTATION
#include <libkern/nanoprintf.h>

int
kdvprintf(const char *fmt, va_list ap)
{
	int r;
	r = npf_vpprintf(ke_md_early_putc, NULL, fmt, ap);
	return r;
}

int
kdprintf(const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = kdvprintf(fmt, ap);
	va_end(ap);

	return ret;
}

void
kfatal_internal(const char *file, int line, const char *fmt, ...)
{
	va_list ap;

#if 0
	splhigh();
#endif

	kdprintf("Fatal: %s:%d: ", file, line);
	va_start(ap, fmt);
	kdvprintf(fmt, ap);
	va_end(ap);

	for (;;)
		;
}
