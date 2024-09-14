/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Tue Jul 09 2024.
 */
/*!
 * @file kconsole.c
 * @brief Kernel console.
 */

#include <sys/errno.h>
#include <sys/termios.h>

#include <stddef.h>

#include "kdk/executive.h"
#include "kdk/file.h"
#include "kdk/kern.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/poll.h"
#include "kdk/vfs.h"

static vnode_t *kconsole_vnode;
static struct vnode_ops console_vnops;
static pollhead_t console_pollhead;

static kspinlock_t console_lock;
static char in_buf[4096];
size_t in_buf_len = 0;
size_t in_read_head = 0;
size_t in_write_head = 0;

static io_result_t
io_result(int result, size_t count)
{
	return (io_result_t) { .result = result, .count = count };
}

int
copyin(vaddr_t udata, size_t len, char **out)
{
	char *kdata;
	kdata = kmem_alloc(len);
	if (kdata == NULL)
		return -ENOMEM;
	memcpy(kdata, (void *)udata, len);
	*out = kdata;
	return 0;
}

void
ex_console_input(int c)
{
	ipl_t ipl = ke_spinlock_acquire(&console_lock);

	if (in_buf_len == sizeof(in_buf)) {
		ke_spinlock_release(&console_lock, ipl);
		return;
	}

	in_buf[in_write_head++] = c;
	if (in_write_head == sizeof(in_buf))
		in_write_head = 0;
	in_buf_len++;
	ke_spinlock_release(&console_lock, ipl);

	pollhead_deliver_events(&console_pollhead, EPOLLIN);
}

void
ex_console_init(void)
{
	ke_spinlock_init(&console_lock);
	pollhead_init(&console_pollhead);
	kconsole_vnode = vnode_new(NULL, VCHR, &console_vnops, NULL, NULL, 0);
}

ex_desc_ret_t
ex_console_open(eprocess_t *proc)
{
	descnum_t num;
	file_t *file;

	file = ex_file_new();
	file->vnode = kconsole_vnode;
	file->nch = (namecache_handle_t) { NULL, NULL };

	num = ex_object_space_reserve(proc->objspace, false);
	if (num == DESCNUM_NULL) {
		ex_file_free((obj_t *)file);
		return -EMFILE;
	}

	ex_object_space_reserved_insert(proc->objspace, num, file);

	return num;
}

static io_result_t
console_read(vnode_t *vnode, vaddr_t user_addr, io_off_t off, size_t size)
{
	ipl_t ipl;
	size_t nread = 0;

	ipl = ke_spinlock_acquire(&console_lock);

	if (in_buf_len == 0) {
		ke_spinlock_release(&console_lock, ipl);
		return io_result(-EAGAIN, 0);
	}

	while (nread < size) {
		char c;

		if (in_buf_len == 0)
			break;

		c = in_buf[in_read_head++];
		if (in_read_head == sizeof(in_buf))
			in_read_head = 0;

		in_buf_len--;

		/* must copyout without holding spinlocks in case of fault */
		ke_spinlock_release(&console_lock, ipl);
		memcpy((void *)(user_addr + nread), &c, sizeof(char));
		ke_spinlock_acquire(&console_lock);

		nread++;
	}

	ke_spinlock_release(&console_lock, ipl);

	return io_result(0, nread);
}

static io_result_t
console_write(vnode_t *vnode, vaddr_t user_addr, io_off_t off, size_t size)
{
	char *buf;
	int r;
	ipl_t ipl;

	(void)vnode;
	(void)off;

	r = copyin(user_addr, size, &buf);
	if (r != 0) {
		kmem_free(buf, size);
		return io_result(-r, 0);
	}

	ipl = ke_spinlock_acquire_at(&pac_console_lock, kIPLHigh);
	for (int i = 0; i < size; i++)
		kputc(buf[i], NULL);
	ke_spinlock_release(&pac_console_lock, ipl);

	kmem_free(buf, size);

	return io_result(0, size);
}

static int
console_seek(vnode_t *vnode, io_off_t old_offset, io_off_t *new_offset)
{
	return -ESPIPE;
}

static int
console_getattr(vnode_t *vn, vattr_t *attr)
{
	(void)vn;

	memset(attr, 0, sizeof(*attr));
	attr->type = VCHR;

	return 0;
}

static int
console_ioctl(vnode_t *vnode, unsigned long cmd, void *data)
{
	switch (cmd) {
	case TIOCGWINSZ:
		return 0;

	default:
		return -EINVAL;
	}
}

static int
console_chpoll(vnode_t *vnode, struct poll_entry *poll, enum chpoll_mode mode)
{
	if (poll != NULL && mode == kChpollPoll)
		pollhead_register(&console_pollhead, poll);
	else if (mode == kChpollRemove) {
		kassert(poll != NULL);
		pollhead_unregister(&console_pollhead, poll);
		return 0;
	}
	return EPOLLOUT | (in_buf_len ? EPOLLIN : 0);
}

static struct vnode_ops console_vnops = {
	.cached_read = console_read,
	.cached_write = console_write,
	.seek = console_seek,
	.getattr = console_getattr,
	.ioctl = console_ioctl,
	.chpoll = console_chpoll,
};
