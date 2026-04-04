/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Apr 04 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file flock.c
 * @brief Advisory file locking.
 */

#include <sys/krx_file.h>
#include <sys/proc.h>

typedef struct flock_entry {
	enum flock_kind {
		FLOCK_TYPE_FLOCK,	/* BSD-style flock() owned by file */
		FLOCK_TYPE_POSIX	/* POSIX-style fcntl() owned by proc */
	} kind;
	enum flock_type {
		FLOCK_RD,
		FLOCK_RW,
	} type;
	off_t	start;	/* first byte locked */
	off_t	end;	/* last byte locked, inclusive */
	union {
		file_t	*file;	/* FLOCK_TYPE_FLOCK */
		proc_t	*proc;	/* FLOCK_TYPE_POSIX */
	} owner;
	TAILQ_ENTRY(flock_entry) owner_link;	/* proc/file flocks_held link */
} flock_entry_t;
