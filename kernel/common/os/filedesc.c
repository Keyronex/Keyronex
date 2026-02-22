/*
 * Copyright (c) 2024-2026 Cloudarox Solutions.
 * Created on Sat Jun 29 2024.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file filedesc.c
 * @brief File descriptor management.
 */

#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/k_log.h>
#include <sys/k_rcu.h>
#include <sys/k_thread.h>
#include <sys/k_wait.h>
#include <sys/kmem.h>
#include <sys/krx_file.h>
#include <sys/libkern.h>
#include <sys/proc.h>
#include <sys/user.h>

#include <libkern/bit32.h>

#define FD_RESERVED ((struct file *)-1L)

typedef struct uf_entry {
	file_t *KRX_RCU file;
	uint32_t flags;
} uf_entry_t;

typedef struct uf_list {
	uint32_t capacity;
	uf_entry_t *entries;
	krcu_entry_t rcu;
} uf_list_t;

typedef struct uf_info {
	kmutex_t lock;
	uf_list_t *KRX_RCU list;
} uf_info_t;

static void
uf_list_free_rcu(void *arg)
{
	uf_list_t *list = (uf_list_t *)arg;
	kmem_free(list->entries, sizeof(uf_entry_t) * list->capacity);
	kmem_free(list, sizeof(uf_list_t));
}

static int
uf_info_expand(uf_info_t *info, unsigned int min)
{
	uf_list_t *old_list = info->list, *new_list;
	uint32_t new_max;

	/* info->lock must be held */

	if (min <= old_list->capacity)
		return 0;

	if (old_list->capacity == 0)
		new_max = 64;
	else
		new_max = old_list->capacity * 2;

	while (new_max < min)
		new_max *= 2;

	new_list = kmem_alloc(sizeof(*new_list));
	if (new_list == NULL)
		return -ENOMEM;

	new_list->capacity = new_max;
	new_list->entries = kmem_alloc(new_max * sizeof(uf_entry_t));
	if (new_list->entries == NULL) {
		kmem_free(new_list, sizeof(uf_list_t));
		return -ENOMEM;
	}

	for (unsigned int i = 0; i < old_list->capacity; i++) {
		new_list->entries[i].file = old_list->entries[i].file;
		new_list->entries[i].flags = old_list->entries[i].flags;
	}

	memset(new_list->entries + old_list->capacity, 0x0,
	    sizeof(uf_entry_t) * (new_max - old_list->capacity));

	ke_rcu_assign_pointer(info->list, new_list);

#if 0
	ke_rcu_call(&old_entries->rcu, uf_list_free_rcu, old_entries);
#endif

	return 0;
}

struct file *
uf_lookup(uf_info_t *info, int fd)
{
	ipl_t ipl;
	file_t *file = NULL;
	uf_list_t *list;

	if (fd < 0)
		return NULL;

	ipl = ke_rcu_read_lock();

	while (true) {
		list = ke_rcu_dereference(info->list);

		if (fd >= list->capacity)
			break;

		file = ke_rcu_dereference(list->entries[fd].file);
		if (file == NULL) {
			/* not open, or raced to close */
			break;
		} else if (file == FD_RESERVED) {
			/* not yet*/
			file = NULL;
			break;
		}

		file = file_tryretain_rcu(file);
		if (file != NULL)
			break; /* successfully retained the file */

		/* raced to close or exchange; retry */
	}

	ke_rcu_read_unlock(ipl);

	return file;
}

int
uf_reserve_fd(uf_info_t *info, unsigned int start_fd, unsigned int oflags)
{
	int error;
	uint32_t fd = start_fd;
	uint32_t flags = 0;
	uf_list_t *list;

	ke_mutex_enter(&info->lock, "reserve_fd");
	list = info->list;

	while (fd < list->capacity && list->entries[fd].file != NULL)
		fd++;

	if (fd >= list->capacity) {
		error = uf_info_expand(info, fd + 1);
		if (error) {
			ke_mutex_exit(&info->lock);
			return error;
		}
		list = info->list;
	}

	if (oflags & O_CLOEXEC)
		flags |= FD_CLOEXEC;

	list->entries[fd].flags = flags;
	ke_rcu_assign_pointer(list->entries[fd].file, FD_RESERVED);
	ke_mutex_exit(&info->lock);

	return fd;
}

void
uf_install_reserved(uf_info_t *info, int fd, struct file *file)
{
	uf_list_t *list;

	ke_mutex_enter(&info->lock, "");
	list = info->list;

	kassert(fd < list->capacity);
	kassert(list->entries[fd].file == FD_RESERVED);

	ke_rcu_assign_pointer(list->entries[fd].file, file);

	ke_mutex_exit(&info->lock);
}

uf_info_t *
uf_new(void)
{
	uf_info_t *info;
	uf_list_t *list;

	info = kmem_alloc(sizeof(*info));
	if (info == NULL)
		return NULL;

	ke_mutex_init(&info->lock);

	list = kmem_alloc(sizeof(*list));
	if (list == NULL) {
		kmem_free(info, sizeof(*info));
		return NULL;
	}

	list->capacity = 0;
	list->entries = NULL;

	info->list = list;

	return info;
}

uf_info_t *
uf_fork(uf_info_t *old_info)
{
	uf_info_t *new_info;
	uf_list_t *old_list, *new_list;

	new_info = kmem_alloc(sizeof(*new_info));
	if (new_info == NULL)
		return NULL;

	ke_mutex_init(&new_info->lock);

	ke_mutex_enter(&old_info->lock, "fork_fd_table");
	old_list = old_info->list;

	new_list = kmem_alloc(sizeof(*new_list));
	if (new_list == NULL) {
		ke_mutex_exit(&old_info->lock);
		kmem_free(new_info, sizeof(*new_info));
		return NULL;
	}

	new_list->capacity = old_list->capacity;
	new_list->entries = kmem_alloc(new_list->capacity * sizeof(uf_entry_t));
	if (new_list->entries == NULL) {
		ke_mutex_exit(&old_info->lock);
		kmem_free(new_list, sizeof(*new_list));
		kmem_free(new_info, sizeof(*new_info));
		return NULL;
	}

	for (unsigned int i = 0; i < old_list->capacity; i++) {
		struct file *f = old_list->entries[i].file;

		if (f == NULL || f == FD_RESERVED) {
			new_list->entries[i].file = NULL;
			new_list->entries[i].flags = 0;
		} else {
			kassert(file_tryretain_rcu(f) != NULL); /* infallible */
			new_list->entries[i].file = f;
			new_list->entries[i].flags = old_list->entries[i].flags;
		}
	}

	new_info->list = new_list;

	ke_mutex_exit(&old_info->lock);

	return new_info;
}

void
uf_destroy(uf_info_t *info)
{
	for (unsigned int i = 0; i < info->list->capacity; i++) {
		struct file *f = info->list->entries[i].file;
		kassert(f != FD_RESERVED);
		if (f != NULL)
			file_release(f);
	}

	kmem_free(info->list->entries,
	    sizeof(uf_entry_t) * info->list->capacity);
	kmem_free(info->list, sizeof(uf_list_t));
	kmem_free(info, sizeof(uf_info_t));
}

int
sys_dup3(int oldfd, int newfd, unsigned int flags)
{
	uf_list_t *list;
	file_t *f;
	file_t *to_close;
	uf_info_t *info = curproc()->finfo;

	if (newfd < 0)
		return -EBADF;

	if (oldfd == newfd)
		return -EINVAL;

	f = uf_lookup(info, oldfd);
	if (f == NULL)
		return -EBADF;

	ke_mutex_enter(&info->lock, "do_dup3");
	if (newfd >= info->list->capacity) {
		if (uf_info_expand(info, newfd + 1) != 0) {
			ke_mutex_exit(&info->lock);
			file_release(f);
			return -ENOMEM;
		}
	}

	list = info->list;
	to_close = list->entries[newfd].file;

	if (to_close == FD_RESERVED) {
		ke_mutex_exit(&info->lock);
		file_release(f);
		return -EBADF;
	}

	list->entries[newfd].flags = flags;
	ke_rcu_assign_pointer(list->entries[newfd].file, f);
	ke_mutex_exit(&info->lock);

	if (to_close)
		file_release(to_close);

	return newfd;
}

int
sys_dup2(int oldfd, int newfd)
{
	uf_info_t *info = curproc()->finfo;

	if (oldfd == newfd) {
		file_t *f = uf_lookup(info, oldfd);
		if (f == NULL)
			return -EBADF;
		file_release(f);
		return newfd;
	}
	return sys_dup3(oldfd, newfd, 0);
}

int
sys_dup(int oldfd)
{
	int newfd;
	file_t *f;
	uf_info_t *info = curproc()->finfo;

	f = uf_lookup(info, oldfd);
	if (f == NULL)
		return -EBADF;

	newfd = uf_reserve_fd(info, 0, 0);
	if (newfd < 0) {
		file_release(f);
		return newfd;
	}

	uf_install_reserved(info, newfd, f);
	return newfd;
}

int
sys_fcntl(int fd, int cmd, unsigned long arg)
{
	file_t *f;
	int r = 0;
	uf_info_t *info = curproc()->finfo;

	switch (cmd) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC: {
		uint32_t flags = (cmd == F_DUPFD_CLOEXEC) ? FD_CLOEXEC : 0;
		int newfd;

		f = uf_lookup(info, fd);
		if (f == NULL)
			return -EBADF;

		newfd = uf_reserve_fd(info, arg, flags);
		if (newfd >= 0)
			uf_install_reserved(info, newfd, f);
		else
			file_release(f);

		return newfd;
	}

	case F_GETFD:
		ke_mutex_enter(&info->lock, "F_GETFD");
		if (fd >= 0 && fd < info->list->capacity &&
		    info->list->entries[fd].file != NULL &&
		    info->list->entries[fd].file != FD_RESERVED)
			r = info->list->entries[fd].flags;
		else
			r = -EBADF;

		ke_mutex_exit(&info->lock);
		return r;

	case F_SETFD:
		ke_mutex_enter(&info->lock, "F_SETFD");
		if (fd >= 0 && fd < info->list->capacity &&
		    info->list->entries[fd].file != NULL &&
		    info->list->entries[fd].file != FD_RESERVED)
			info->list->entries[fd].flags = (unsigned int)arg;
		else
			r = -EBADF;

		ke_mutex_exit(&info->lock);
		return r;

	default:
		return -EINVAL;
	}
}

int
sys_close(int fd)
{
	file_t *f;
	uf_list_t *list;
	uf_info_t *info = curproc()->finfo;

	ke_mutex_enter(&info->lock, "close");
	list = info->list;

	if (fd < 0 || fd >= list->capacity) {
		ke_mutex_exit(&info->lock);
		return -EBADF;
	}

	f = list->entries[fd].file;

	if (f == NULL || f == FD_RESERVED) {
		ke_mutex_exit(&info->lock);
		return -EBADF;
	}

	ke_rcu_assign_pointer(list->entries[fd].file, NULL);
	list->entries[fd].flags = 0;

	ke_mutex_exit(&info->lock);

	file_release(f);

	return 0;
}
