/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Tue Feb 24 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file fifofs.c
 * @brief FIFO filesystem.
 */

#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kmem.h>
#include <sys/krx_file.h>
#include <sys/krx_user.h>
#include <sys/krx_vfs.h>
#include <sys/libkern.h>
#include <sys/proc.h>
#include <sys/stream.h>
#include <sys/strsubr.h>

struct fifonode {
	stdata_t *st;
};

#define VTOFN(VN) ((struct fifonode *)vn->fsprivate_1)

static void loopback_put(queue_t *, mblk_t *);

static int fifo_inactive(vnode_t *);
static int fifo_close(vnode_t *, int flags);
static int fifo_getattr(vnode_t *, vattr_t *);
static int fifo_read(vnode_t *, void *buf, size_t buflen, off_t, int flags);
static int fifo_write(vnode_t *, const void *buf, size_t buflen, off_t,
    int flags);
static int fifo_ioctl(vnode_t *, unsigned long cmd, void *arg);
static int fifo_chpoll(vnode_t *, struct poll_entry *, enum chpoll_mode);

static struct qinit loopback_qinit = {
	.putp = loopback_put,
};

static struct streamtab loopback_streamtab = {
	.rinit = &loopback_qinit,
	.winit = &loopback_qinit,
};

static struct vnode_ops fifo_vnops = {
	.inactive = fifo_inactive,
	.close = fifo_close,
	.getattr = fifo_getattr,
	.read = fifo_read,
	.write = fifo_write,
	.ioctl = fifo_ioctl,
	.chpoll = fifo_chpoll,
};

static void
loopback_put(queue_t *q, mblk_t *mp)
{
	str_put(q->other->next, mp);
}

static int
fifo_inactive(vnode_t *)
{
	return 0;
}

static int
fifo_close(vnode_t *vn, int flags)
{
	struct fifonode *fn = VTOFN(vn);
	stdata_t *sh = fn->st;
	bool last_reader = false;
	bool last_writer = false;

	str_reqlock(sh);

	if (flags == O_RDONLY) {
		kassert(sh->nreaders > 0);
		sh->nreaders--;
		last_reader = (sh->nreaders == 0);

	} else if (flags == O_WRONLY) {
		kassert(sh->nwriters > 0);
		sh->nwriters--;
		last_writer = (sh->nwriters == 0);

	} else {
		kassert(flags == O_RDWR);
		kassert(sh->nwriters > 0);
		kassert(sh->nreaders > 0);

		if (sh->nreaders > 0) {
			sh->nreaders--;
			last_reader = (sh->nreaders == 0);
		}
		if (sh->nwriters > 0) {
			sh->nwriters--;
			last_writer = (sh->nwriters == 0);
		}
	}

	if (last_writer) {
		sh->hanged_up = true;
		ke_event_set_signalled(&sh->data_readable, true);
		pollhead_deliver_events(&sh->pollhead,
		    EPOLLHUP | EPOLLIN | EPOLLRDNORM);
	}

	if (last_reader)
		pollhead_deliver_events(&sh->pollhead, EPOLLOUT | EPOLLERR);

	str_requnlock(sh);
	return 0;
}

static int
fifo_getattr(vnode_t *vn, vattr_t *attr)
{
	memset(attr, 0, sizeof(*attr));
	attr->type = VFIFO;
	return 0;
}

static int
fifo_read(vnode_t *vn, void *buf, size_t buflen, off_t, int flags)
{
	struct fifonode *fn = VTOFN(vn);
	return strread(fn->st, buf, buflen, flags);
}

static int
fifo_write(vnode_t *vn, const void *buf, size_t buflen, off_t, int flags)
{
	struct fifonode *fn = VTOFN(vn);
	return strwrite(fn->st, buf, buflen, flags);
}

static int
fifo_ioctl(vnode_t *vn, unsigned long cmd, void *arg)
{
	ktodo();
}

static int
fifo_chpoll(vnode_t *vn, struct poll_entry *pe, enum chpoll_mode mode)
{
	struct fifonode *fn = VTOFN(vn);
	return strchpoll(fn->st, pe, mode);
}

int
sys_pipe(int upipefd[2], int flags)
{
	struct fifonode *fn;
	stdata_t *st;
	vnode_t *vn;
	struct file *rf, *wf;
	int rfd, wfd;

	if ((flags & ~(O_NONBLOCK | O_CLOEXEC)) != 0)
		return -EINVAL;

	st = stropen(&loopback_streamtab, NULL, STR_HEAD_KIND_FIFO);
	st->nreaders = 1;
	st->nwriters = 1;

	st->rq_bottom->other->back = st->wq;

	fn = kmem_alloc(sizeof(struct fifonode));
	fn->st = st;

	vn = vn_alloc(NULL, VFIFO, &fifo_vnops, (uintptr_t)fn, 0);

	rf = file_new(NCH_NULL, vn, O_RDONLY | (flags & O_NONBLOCK));

	wf = file_new(NCH_NULL, vn, O_WRONLY | (flags & O_NONBLOCK));

	rfd = uf_reserve_fd(curproc()->finfo, 0,
	    flags & O_CLOEXEC ? FD_CLOEXEC : 0);

	wfd = uf_reserve_fd(curproc()->finfo, 0,
	    flags & O_CLOEXEC ? FD_CLOEXEC : 0);

	uf_install_reserved(curproc()->finfo, rfd, rf);

	uf_install_reserved(curproc()->finfo, wfd, wf);

	upipefd[0] = rfd;
	upipefd[1] = wfd;

	return 0;
}
