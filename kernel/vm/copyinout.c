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
#include <kdk/libkern.h>

int ke_trap_recovery_save(ktrap_recovery_frame_t *frame);

size_t
memcpy_from_user(void *dst, const void *src, size_t len)
{
	ktrap_recovery_frame_t *frame = ke_trap_recovery_begin();
	if (ke_trap_recovery_save(frame) != 0) {
		kprintf("FAILURE!\n");
		return -EFAULT;
	}

	memcpy(dst, src, len);
	ke_trap_recovery_end();

	return 0;
}
