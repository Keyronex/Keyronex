/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file copyinout.c
 * @brief Safe copying to/from userland.
 */

#include <sys/errno.h>
#include <sys/kmem.h>
#include <libkern/lib.h>

typedef struct ktrap_recover_frame {
	/* todo */
} ktrap_recovery_frame_t;

ktrap_recovery_frame_t *ke_trap_recovery_begin(void)
{
	/* todo */
	return NULL;
}

void ke_trap_recovery_end(void)
{
	/* todo */
}

int ke_trap_recovery_safe(ktrap_recovery_frame_t *frame)
{
	/* todo */
	return 0;
}

int
memcpy_from_user(void *dst, const void *src, size_t len)
{
	ktrap_recovery_frame_t *frame = ke_trap_recovery_begin();

	if (ke_trap_recovery_safe(frame) != 0)
		return -EFAULT;

	memcpy(dst, src, len);
	ke_trap_recovery_end();

	return 0;
}

int
memcpy_to_user(void *dst, const void *src, size_t len)
{
	ktrap_recovery_frame_t *frame = ke_trap_recovery_begin();

	if (ke_trap_recovery_safe(frame) != 0)
		return -EFAULT;

	memcpy(dst, src, len);
	ke_trap_recovery_end();

	return 0;
}

size_t
strllen_user(const char *str, size_t strsz)
{
	size_t r;
	ktrap_recovery_frame_t *frame = ke_trap_recovery_begin();

	if (ke_trap_recovery_safe(frame) != 0)
		return -EFAULT;

	r = strlen(str);
	ke_trap_recovery_end();

	return r;
}

size_t
strldup_user(char **dst, const char *src, size_t strsz)
{
	size_t r;
	size_t len;
	char *copy;

	len = strllen_user(src, 4095);
	if (len < 0)
		return len;

	copy = kmem_alloc(len + 1);
	if (copy == NULL)
		return -ENOMEM;

	r = memcpy_from_user(copy, src, len);
	if (r != 0) {
		kmem_free(copy, len + 1);
		return r;
	}

	copy[len] = '\0';
	*dst = copy;

	return len;
}
