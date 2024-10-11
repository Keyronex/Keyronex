/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Fri Oct 11 2024.
 */
/*!
 * @file copyout.c
 * @brief Copyin/copyout userspace data functions.
 */

#include <sys/errno.h>

#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>

int ke_trap_recovery_save(ktrap_recovery_frame_t *frame);

int
memcpy_from_user(void *dst, const void *src, size_t len)
{
	ktrap_recovery_frame_t *frame = ke_trap_recovery_begin();

	if (ke_trap_recovery_save(frame) != 0)
		return -EFAULT;

	memcpy(dst, src, len);
	ke_trap_recovery_end();

	return 0;
}

int
memcpy_to_user(void *dst, const void *src, size_t len)
{
	ktrap_recovery_frame_t *frame = ke_trap_recovery_begin();

	if (ke_trap_recovery_save(frame) != 0)
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

	if (ke_trap_recovery_save(frame) != 0)
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
