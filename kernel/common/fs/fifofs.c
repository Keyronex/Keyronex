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
fifo_inactive(vnode_t *vn)
{
	struct fifonode *fn = VTOFN(vn);
	strclose(fn->st);
	kmem_free(fn, sizeof(*fn));
	return 0;
}

static int
fifo_close(vnode_t *vn, int flags)
{
	struct fifonode *fn = VTOFN(vn);
	stdata_t *sh = fn->st;
	bool last_reader = false;
	bool last_writer = false;

	str_req_begin(sh);

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

	str_req_end(sh);
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
	return -ENOTTY;
}

static int
fifo_chpoll(vnode_t *vn, struct poll_entry *pe, enum chpoll_mode mode)
{
	struct fifonode *fn = VTOFN(vn);
	return strchpoll(fn->st, pe, mode);
}

int
sys_pipe(int ufd[2], int flags)
{
	struct fifonode *fn;
	stdata_t *st;
	vnode_t *vn;
	struct file *rf, *wf;
	int fd[2];
	int r;

	if ((flags & ~(O_NONBLOCK | O_CLOEXEC)) != 0)
		return -EINVAL;

	st = stropen(&loopback_streamtab, NULL, STR_HEAD_KIND_FIFO);
	if (st == NULL)
		return -ENOMEM;

	st->nreaders = 1;
	st->nwriters = 1;

	st->rq_bottom->other->back = st->wq;

	fn = kmem_alloc(sizeof(struct fifonode));
	if (fn == NULL) {
		strclose(st);
		return -ENOMEM;
	}

	fn->st = st;

	vn = vn_alloc(NULL, VFIFO, &fifo_vnops, (uintptr_t)fn, 0);
	if (vn == NULL) {
		kmem_free(fn, sizeof(*fn));
		strclose(st);
		return -ENOMEM;
	}

	rf = file_new(NCH_NULL, vn, O_RDONLY | (flags & O_NONBLOCK));
	if (rf == NULL) {
		vn_release(vn);
		return -ENOMEM;
	}

	vn_retain(vn); /* file_new() steals a ref */

	wf = file_new(NCH_NULL, vn, O_WRONLY | (flags & O_NONBLOCK));
	if (wf == NULL) {
		vn_release(vn);
		file_release(rf);
	}

	fd[0] = uf_reserve_fd(curproc()->finfo, 0,
	    flags & O_CLOEXEC ? FD_CLOEXEC : 0);
	if (fd[0] < 0) {
		file_release(rf);
		file_release(wf);
		return fd[0];
	}

	fd[1] = uf_reserve_fd(curproc()->finfo, 0,
	    flags & O_CLOEXEC ? FD_CLOEXEC : 0);
	if (fd[1] < 0) {
		uf_unreserve_fd(curproc()->finfo, fd[0]);
		file_release(rf);
		file_release(wf);
		return fd[1];
	}

	r = memcpy_to_user(ufd, fd, sizeof(fd));
	if (r < 0) {
		uf_unreserve_fd(curproc()->finfo, fd[0]);
		uf_unreserve_fd(curproc()->finfo, fd[1]);
		file_release(rf);
		file_release(wf);
		return r;
	}

	uf_install_reserved(curproc()->finfo, fd[0], rf);
	uf_install_reserved(curproc()->finfo, fd[1], wf);

	return 0;
}
