/*
 * Copyright (c) 2025-26 Cloudarox Solutions.
 * Created on Tue Dec 23 2025.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file sockfs.c
 * @brief Socket filesystem.
 */

#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/krx_file.h>
#include <sys/krx_user.h>
#include <sys/krx_vfs.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/vnode.h>

enum sockstate {
	SS_ISCONNECTED = 1,
	SS_ISCONNECTING = 2,
	SS_ISDISCONNECTING = 4,
	SS_CANTSENDMORE = 8,
	SS_CANTRCVMORE = 16,
	SS_ISBOUND = 32
};

struct socknode {
	int domain;
	int type;
	int protocol;
	struct streamtab *streamtab;

	stdata_t *stream;

	enum sockstate state;
};

#define VTOSN(VN) ((struct socknode *)vn->fsprivate_1)

static void sock_rput(queue_t *, mblk_t *);

static int sock_inactive(vnode_t *);
static int sock_close(vnode_t *, int flags);
static int sock_getattr(vnode_t *, vattr_t *);
static int sock_read(vnode_t *, void *buf, size_t buflen, off_t, int flags);
static int sock_write(vnode_t *, const void *buf, size_t buflen, off_t,
    int flags);
static int sock_ioctl(vnode_t *, unsigned long cmd, void *arg);
static int sock_chpoll(vnode_t *, struct poll_entry *, enum chpoll_mode);

extern struct streamtab ux_cotsord_streamtab, ux_clts_streamtab;

static struct qinit sock_rinit = {
	.putp = sock_rput,
};
static struct qinit sock_winit = {
	.putp = str_putnext,
};
static struct streamtab sock_streamtab = {
	.rinit = &sock_rinit,
	.winit = &sock_winit,
};

static struct vnode_ops sock_vnops = {
	.inactive = sock_inactive,
	.close = sock_close,
	.getattr = sock_getattr,
	.read = sock_read,
	.write = sock_write,
	.ioctl = sock_ioctl,
	.chpoll = sock_chpoll,
};

static int
so_create(file_t **out, int domain, int type, int protocol)
{
	bool nonblock = type & SOCK_NONBLOCK;
	struct streamtab *streamtab;
	stdata_t *sh = NULL;
	struct socknode *sn = NULL;
	vnode_t *vn = NULL;
	file_t *fp = NULL;
	int r;

	type &= ~SOCK_NONBLOCK;

	switch (domain) {
	case AF_UNIX:
		switch (type) {
		case SOCK_STREAM:
			streamtab = &ux_cotsord_streamtab;
			break;
		case SOCK_DGRAM:
			ktodo();
			break;
		}
		break;

	default:
		ktodo();
	}

	sh = stropen(streamtab, NULL, STR_HEAD_KIND_NONE);
	if (sh == NULL) {
		r = -ENOMEM;
		goto err;
	}

	r = strpush(sh, &sock_streamtab);
	if (r != 0) {
		goto err;
	}

	sn = kmem_alloc(sizeof(*sn));
	if (sn == NULL) {
		r = -ENOMEM;
		goto err;
	}

	sn->domain = domain;
	sn->type = type;
	sn->protocol = protocol;
	sn->streamtab = streamtab;

	sn->state = 0;
	sn->stream = sh;

	sh->wq->next->ptr = sh->wq->next->other->ptr = sn; /* sockmod's ptr */

	vn = vn_alloc(NULL, VSOCK, &sock_vnops, (uintptr_t)sn, 0);
	if (vn == NULL) {
		r = -ENOMEM;
		goto err;
	}

	fp = file_new(NCH_NULL, vn, nonblock ? O_NONBLOCK : 0);
	if (fp == NULL) {
		r = -ENOMEM;
		goto err;
	}

	*out = fp;
	return 0;

err:
	if (fp != NULL)
		file_release(fp);
	else if (vn != NULL)
		vn_release(vn);
	else if (sh != NULL)
		strclose(sh);

	return r;
}

static void
sock_rput(queue_t *, mblk_t *)
{
	ktodo();
}

/*
 * vnode ops
 */

static int
sock_inactive(vnode_t *)
{
	ktodo();
}

static int
sock_close(vnode_t *, int flags)
{
	ktodo();
}

static int
sock_getattr(vnode_t *, vattr_t *)
{
	ktodo();
}

static int
sock_read(vnode_t *, void *buf, size_t buflen, off_t, int flags)
{
	ktodo();
}

static int
sock_write(vnode_t *, const void *buf, size_t buflen, off_t, int flags)
{
	ktodo();
}

static int
sock_ioctl(vnode_t *, unsigned long cmd, void *arg)
{
	ktodo();
}

static int
sock_chpoll(vnode_t *, struct poll_entry *, enum chpoll_mode)
{
	ktodo();
}

/*
 * system calls
 */

int
sys_socket(int domain, int type, int protocol)
{
	file_t *fp;
	int fd, r;

	fd = uf_reserve_fd(curproc()->finfo, 0,
	    (type & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
	if (fd < 0)
		return fd;

	r = so_create(&fp, domain, type, protocol);
	if (r < 0) {
		uf_unreserve_fd(curproc()->finfo, fd);
		return -ENOMEM;
	}

	uf_install_reserved(curproc()->finfo, fd, fp);
	return fd;
}

int
sys_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
	ktodo();
}

int
sys_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	ktodo();
}

int
sys_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	ktodo();
}

int
sys_listen(int sockfd, int backlog)
{
	ktodo();
}

int
sys_getsockopt(int sockfd, int level, int optname, void *optval,
    socklen_t *optlen)
{
	ktodo();
}

int
sys_setsockopt(int sockfd, int level, int optname, const void *optval,
    socklen_t optlen)
{
	ktodo();
}

int
sys_recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	ktodo();
}

int
sys_sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
	ktodo();
}
