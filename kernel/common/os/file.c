/*
 * Copyright (c) 2024-2026 Cloudarox Solutions.
 * Created on Sat Jun 29 2024.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file file.c
 * @brief File object
 */

#include <sys/krx_file.h>
#include <sys/krx_vfs.h>
#include <sys/vnode.h>

#include <stdbool.h>

file_t *
file_tryretain_rcu(file_t *f)
{
	while (true) {
		uint32_t count = __atomic_load_n(&f->refcnt, __ATOMIC_RELAXED);
		if (count == 0)
			return f;
		if (__atomic_compare_exchange_n(&f->refcnt, &count, count + 1,
			false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			return f;
		}
	}
}

void
file_release(file_t *file)
{
	uint32_t old = __atomic_fetch_sub(&file->refcnt, 1, __ATOMIC_ACQ_REL);

	if (old == 1) {
		if (file->nch.nc != NULL)
			nchandle_release(file->nch);
		else if (file->vnode != NULL)
			vn_release(file->vnode);
	}
}
