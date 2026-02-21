/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dlog.c
 * @brief Kernel debug log.
 */

#include <sys/k_log.h>
#include <sys/k_intr.h>

#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS	1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS	1
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS		0
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS 		1
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS		1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS		1
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS	0

#define NANOPRINTF_IMPLEMENTATION
#include <libkern/nanoprintf.h>

kspinlock_t dlog_lock = KSPINLOCK_INITIALISER;

int
kdvprintf_unlocked(const char *fmt, va_list ap)
{
	int r;
	r = npf_vpprintf(ke_md_early_putc, NULL, fmt, ap);
	return r;
}

int
kdvprintf(const char *fmt, va_list ap)
{
	int r;
	ipl_t ipl = splhigh();
	ke_spinlock_enter_nospl(&dlog_lock);
	r = kdvprintf_unlocked(fmt, ap);
	ke_spinlock_exit(&dlog_lock, ipl);
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

int
kdprintf_unlocked(const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = kdvprintf_unlocked(fmt, ap);
	va_end(ap);

	return ret;
}

int
kvsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
	return npf_vsnprintf(str, size, fmt, ap);
}

int
ksnprintf(char *str, size_t size, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = kvsnprintf(str, size, fmt, ap);
	va_end(ap);

	return ret;
}

void
kdputn(const char *str, size_t len)
{
	ipl_t ipl = splhigh();
	ke_spinlock_enter_nospl(&dlog_lock);
	for (size_t i = 0; i < len; i++)
		ke_md_early_putc(str[i], 0);
	ke_spinlock_exit(&dlog_lock, ipl);
}

void
kfatal_internal(const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	// spldisp();
	splhigh();

	ke_spinlock_enter_nospl(&dlog_lock);
	kdprintf_unlocked("Fatal: %s:%d: ", file, line);
	va_start(ap, fmt);
	kdvprintf_unlocked(fmt, ap);
	ke_md_early_putc('\n', 0);
	va_end(ap);
	ke_spinlock_exit_nospl(&dlog_lock);

	for (;;)
		;
}
