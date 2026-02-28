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
 *
 * FIXME: Needs a lot of review!
 * - state machine
 * - failure handling
 * - move copyin/copyout into the sys_xx, out of so_xx
 */

#include <sys/errno.h>
#include <sys/k_wait.h>
#include <sys/kmem.h>
#include <sys/krx_file.h>
#include <sys/krx_user.h>
#include <sys/krx_vfs.h>
#include <sys/libkern.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/tihdr.h>
#include <sys/un.h>
#include <sys/vnode.h>

#include <stdbool.h>

enum sockstate {
	SS_ISCONNECTED = 1,
	SS_ISCONNECTING = 2,
	SS_ISDISCONNECTING = 4,
	SS_CANTSENDMORE = 8,
	SS_CANTRCVMORE = 16,
	SS_ISBOUND = 32,
	SS_ISLISTENING = 64,
};

struct socknode {
	int domain;
	int type;
	int protocol;

	struct streamtab *streamtab;

	stdata_t *stream;

	enum sockstate state;

	struct sockaddr_storage bound_addr;
	socklen_t bound_addrlen;
	namecache_handle_t bound_nch; /* for AF_UNIX, bound socket file */

	/* for synchronous waiting on TPI ack/nak */
	kevent_t tpi_ack_ev;
	mblk_t *tpi_ack_mp;

	/* for synchronous connect() */
	kevent_t conn_con_ev;
	mblk_t *conn_con_mp;

	mblk_q_t conn_indq;
	int conn_indq_len;
	kevent_t conn_ind_ev;
};

#define VTOSN(VN) ((struct socknode *)(VN)->fsprivate_1)

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
so_create(file_t **out_fp, struct socknode **out_sn, int domain, int type, int protocol)
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
	sn->stream = sh;
	sn->state = 0;

	ke_event_init(&sn->tpi_ack_ev, false);
	sn->tpi_ack_mp = NULL;

	ke_event_init(&sn->conn_con_ev, false);
	sn->conn_con_mp = NULL;

	TAILQ_INIT(&sn->conn_indq);
	sn->conn_indq_len = 0;
	ke_event_init(&sn->conn_ind_ev, false);

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

	*out_fp = fp;
	if (out_sn != NULL)
		*out_sn = sn;

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
sock_rput(queue_t *rq, mblk_t *mp)
{
	struct socknode *sn = rq->ptr;
	union T_primitives *prim = (union T_primitives *)mp->rptr;

	if (mp->db->type != M_PROTO && mp->db->type != M_PCPROTO) {
		str_putnext(rq, mp);
		return;
	}

	switch (prim->type) {
	case T_BIND_ACK:
	case T_OK_ACK:
	case T_ERROR_ACK:
		kassert(sn->tpi_ack_mp == NULL);
		sn->tpi_ack_mp = mp;
		ke_event_set_signalled(&sn->tpi_ack_ev, true);
		break;

	case T_CONN_CON:
		kassert(sn->state == SS_ISCONNECTING);
		sn->state = SS_ISCONNECTED;
		sn->conn_con_mp = mp;
		ke_event_set_signalled(&sn->conn_con_ev, true);
		break;

	case T_CONN_IND:
		TAILQ_INSERT_TAIL(&sn->conn_indq, mp, link);
		sn->conn_indq_len++;
		ke_event_set_signalled(&sn->conn_ind_ev, true);
		pollhead_deliver_events(&sn->stream->pollhead,
		    EPOLLIN | EPOLLRDNORM);
		break;

	case T_DISCON_IND:
		sn->state &= ~(SS_ISCONNECTING | SS_ISCONNECTED |
		    SS_ISDISCONNECTING);
		sn->state |= SS_CANTRCVMORE | SS_CANTSENDMORE;
		ke_event_set_signalled(&sn->conn_con_ev, true);
		ke_event_set_signalled(&sn->stream->data_readable, true);
		/* todo: set sock error? */
		str_freemsg(mp);
		sn->stream->hanged_up = true;
		pollhead_deliver_events(&sn->stream->pollhead,
		    EPOLLIN | EPOLLHUP | EPOLLERR);
		break;

	case T_ORDREL_IND:
		sn->state |= SS_CANTRCVMORE;
		sn->stream->hanged_up = true;
		ke_event_set_signalled(&sn->stream->data_readable, true);
		pollhead_deliver_events(&sn->stream->pollhead,
		    EPOLLIN | EPOLLRDNORM | EPOLLRDHUP);
		break;

	default:
		kfatal("sockfs: unexpected TPI message of type %d\n",
		    prim->type);
	}
}

/*
 * Make a synchronous TPI request.
 * Consumes mp.
 * Caller must hold the stream's reqlock, and mutex.
 * The stream mutex is released while sleeping and reacquired afterwards.
 * The reqlock remains held throughout.
 */
static mblk_t *
sock_tpi_request(struct socknode *sn, mblk_t *mp)
{
	stdata_t *sh = sn->stream;

	kassert(ke_mutex_held(sh->mutex));

	sn->tpi_ack_mp = NULL;
	ke_event_set_signalled(&sn->tpi_ack_ev, false);

	str_putnext(sh->wq, mp);

	ke_mutex_exit(sh->mutex);
	ke_wait1(&sn->tpi_ack_ev, "sock_tpi_request", false, ABSTIME_FOREVER);
	ke_mutex_enter(sh->mutex, "sock_tpi_request done");

	return sn->tpi_ack_mp;
}

/*
 * Extract Unix errno from a TPI ack/nak message.
 */
static int
sock_tpi_errno(mblk_t *mp)
{
	union T_primitives *prim = (typeof(prim))mp->rptr;
	switch (prim->type) {
	case T_BIND_ACK:
	case T_OK_ACK:
		return 0;

	case T_ERROR_ACK:
		return prim->error_ack.UNIX_error;

	default:
		kfatal("sock_tpi_errno: unexpected TPI prim %d", prim->type);
	}
}

static void
sock_requeue_conn_ind_mp(struct socknode *sn, mblk_t *mp)
{
	kassert(ke_mutex_held(sn->stream->mutex));

	TAILQ_INSERT_HEAD(&sn->conn_indq, mp, link);
	sn->conn_indq_len++;
	ke_event_set_signalled(&sn->conn_ind_ev, true);
	pollhead_deliver_events(&sn->stream->pollhead, EPOLLIN | EPOLLRDNORM);
}

static int
so_accept4(struct socknode *sn, bool nonblock, struct sockaddr *addr,
    socklen_t *addrlen, int flags)
{
	stdata_t *sh = sn->stream;
	mblk_t *cimp, *crmp;
	file_t *acceptor_fp;
	struct socknode *acceptor_sn;
	struct T_conn_ind *ci;
	struct T_conn_res *cr;
	int r, fd;

	str_reqlock(sh);
	if ((sn->state & SS_ISLISTENING) == 0) {
		str_requnlock(sh);
		return -EINVAL;
	}

	while (TAILQ_EMPTY(&sn->conn_indq)) {
		if (nonblock) {
			str_requnlock(sh);
			return -EAGAIN;
		}

		ke_mutex_exit(sh->mutex);
		ke_wait1(&sn->conn_ind_ev, "sock_accept4 wait connind", false,
		    ABSTIME_FOREVER);
		ke_mutex_enter(sh->mutex, "sock_accept4");
	}

	cimp = TAILQ_FIRST(&sn->conn_indq);
	TAILQ_REMOVE(&sn->conn_indq, cimp, link);
	sn->conn_indq_len--;
	if (TAILQ_EMPTY(&sn->conn_indq))
		ke_event_set_signalled(&sn->conn_ind_ev, false);
	ke_mutex_exit(sh->mutex);

	ci = (typeof(ci))cimp->rptr;

	r = so_create(&acceptor_fp, &acceptor_sn, sn->domain, sn->type | flags,
	    sn->protocol);
	if (r != 0) {
		ke_mutex_enter(sh->mutex, "sock_accept4 requeue");
		sock_requeue_conn_ind_mp(sn, cimp);
		str_requnlock(sh);
		return r;
	}

	crmp = str_allocb(sizeof(struct T_conn_res));
	if (crmp == NULL) {
		ke_mutex_enter(sh->mutex, "sock_accept4 requeue");
		sock_requeue_conn_ind_mp(sn, cimp);
		str_requnlock(sh);
		file_release(acceptor_fp);
		return -ENOMEM;
	}

	fd = uf_reserve_fd(curproc()->finfo, 0,
	    flags & SOCK_CLOEXEC ? FD_CLOEXEC : 0);
	if (fd < 0) {
		ke_mutex_enter(sh->mutex, "sock_accept4 requeue");
		sock_requeue_conn_ind_mp(sn, cimp);
		str_requnlock(sh);
		file_release(acceptor_fp);
		return -ENOMEM;
	}

	crmp->db->type = M_PROTO;
	cr = (typeof(cr))crmp->wptr;
	cr->PRIM_type = T_CONN_RES;
	cr->ACCEPTOR_id = (size_t)acceptor_sn->stream->rq_bottom;
	cr->SEQ_number = ci->SEQ_number;
	crmp->wptr += sizeof(*cr);

	ke_mutex_enter(sh->mutex, "so_accept4 conn_res");
	crmp = sock_tpi_request(sn, crmp);
	r = sock_tpi_errno(crmp);
	str_freemsg(crmp);
	if (r != 0) {
		/*
		 * TODO: special handling for this case - we requeue the
		 * conn_ind despite having sent down a conn_res. (this is what
		 * the Unix transports expects.) But it's subtle and the Unix
		 * transport also
		 */
		if (r == ENOMEM)
			sock_requeue_conn_ind_mp(sn, cimp);
		else
			str_freemsg(cimp);
		str_requnlock(sh);
		file_release(acceptor_fp);
		uf_unreserve_fd(curproc()->finfo, fd);
		return -r;
	} else {
		str_freemsg(cimp);
	}
	str_requnlock(sh);

	acceptor_sn->state |= SS_ISCONNECTED;

	uf_install_reserved(curproc()->finfo, fd, acceptor_fp);
	return fd;
}

static int
so_bind(vnode_t *vn, struct socknode *sn, const struct sockaddr *addr,
    socklen_t addrlen)
{
	mblk_t *mp;
	struct T_bind_req *br;
	int r;

	str_reqlock(sn->stream);
	if (sn->state != 0) {
		str_requnlock(sn->stream);
		return -EINVAL;
	}
	ke_mutex_exit(sn->stream->mutex);

	mp = str_allocb(sizeof(struct T_bind_req));
	if (mp == NULL) {
		str_requnlock(sn->stream);
		return -ENOMEM;
	}

	mp->db->type = M_PROTO;
	br = (typeof(br))mp->wptr;
	br->PRIM_type = T_BIND_REQ;
	br->CONIND_number = 0;
	mp->wptr += sizeof(struct T_bind_req);

	if (sn->domain == AF_UNIX) {
		sa_family_t fam;
		char path[109];
		vattr_t attr = { 0 };
		struct lookup_info li;
		struct sockaddr_ux *sux;

		/*
		 * We translate the sockaddr_un to a sockaddr_ux.
		 * The latter includes a pointer to the stream's lower rq.
		 */

		if (addrlen < sizeof(sa_family_t) ||
		    addrlen > sizeof(struct sockaddr_un)) {
			str_requnlock_mutexunheld(sn->stream);
			return -EINVAL;
		}

		r = memcpy_from_user(&fam, addr, sizeof(sa_family_t));
		if (r != 0) {
			str_requnlock_mutexunheld(sn->stream);
			return -EFAULT;
		}

		if (fam != AF_UNIX) {
			str_requnlock_mutexunheld(sn->stream);
			return -EINVAL;
		}

		if (addrlen > offsetof(struct sockaddr_un, sun_path)) {
			r = memcpy_from_user(path,
			    (const char *)addr + sizeof(sa_family_t),
			    addrlen - sizeof(sa_family_t));
			if (r != 0) {
				str_requnlock_mutexunheld(sn->stream);
				return -EFAULT;
			}

			path[addrlen - sizeof(sa_family_t)] = '\0';

			vfs_lookup_init(&li, root_nch, path, LOOKUP_CREATE);
			attr.type = VSOCK;
			attr.mode = 0777;
			li.create_attr = &attr;
			r = vfs_lookup(&li);

			if (r != 0) {
				str_requnlock_mutexunheld(sn->stream);
				kdprintf("sobind: vfs_lookup failed: %d\n", r);
				kdprintf("sobind: path was '%s'\n", path);
				return r;
			}

			/*
			 * FIXME: this works around the inode reuse bug of 9p!
			 */
			li.result.nc->vp->sock.sockvn = vn;
			li.result.nc->vp->type = VSOCK;

			/* this takes over the retained lookup */
			sn->bound_nch = li.result;
		}

		br->ADDR_length = addrlen +
		    (offsetof(struct sockaddr_ux, sun_path) -
			offsetof(struct sockaddr_un, sun_path));

		sux = (struct sockaddr_ux *)&br->ADDR;
		sux->sun_family = AF_UNIX;
		memcpy(sux->sun_path, path, addrlen - sizeof(sa_family_t));
		sux->sux_rq = sn->stream->rq_bottom;
		;
	} else {
		br->ADDR_length = addrlen;
		r = memcpy_from_user(&br->ADDR, addr, addrlen);
		if (r != 0) {
			str_requnlock_mutexunheld(sn->stream);
			return -EFAULT;
		}
	}

	ke_mutex_enter(sn->stream->mutex, "so_bind");
	mp = sock_tpi_request(sn, mp);

	r = sock_tpi_errno(mp);
	if (r == 0) {
		struct T_bind_ack *ba = (typeof(ba))mp->rptr;
		kassert(ba->ADDR_length > sizeof(sa_family_t) &&
		    ba->ADDR_length <= 128);
		sn->bound_addrlen = ba->ADDR_length;
		memcpy(&sn->bound_addr, &ba->ADDR, ba->ADDR_length);
		sn->state |= SS_ISBOUND;
	} else {
		kfatal("bind failed\n");
	}

	str_requnlock(sn->stream);

	str_freemsg(mp);

	return r;
}

static int
so_listen(struct socknode *sn, int backlog)
{
	stdata_t *sh = sn->stream;
	mblk_t *mp;
	struct T_bind_req *br;
	int r;

	if (sn->type != SOCK_STREAM)
		return -EOPNOTSUPP;

	if (backlog < 1)
		backlog = 1;

	str_reqlock(sh);
	if (!(sn->state & SS_ISBOUND) || sn->state & SS_ISLISTENING) {
		str_requnlock(sh);
		return -EINVAL;
	}
	ke_mutex_exit(sh->mutex);

	mp = str_allocb(sizeof(struct T_bind_req));
	if (mp == NULL)
		return -ENOMEM;

	mp->db->type = M_PROTO;
	br = (struct T_bind_req *)mp->rptr;
	br->PRIM_type = T_BIND_REQ;
	memcpy(&br->ADDR, &sn->bound_addr, sn->bound_addrlen);
	br->ADDR_length = sn->bound_addrlen;
	br->CONIND_number = backlog;
	mp->wptr = mp->rptr + sizeof(struct T_bind_req);

	ke_mutex_enter(sh->mutex, "so_listen");
	mp = sock_tpi_request(sn, mp);

	r = sock_tpi_errno(mp);
	if (r == 0) {
		sn->state |= SS_ISLISTENING;
	} else {
		kfatal("listen failed\n");
	}
	str_requnlock(sh);

	str_freemsg(mp);

	return r;
}

static int
so_connect(struct socknode *sn, const struct sockaddr *addr, socklen_t addrlen)
{
	stdata_t *sh = sn->stream;
	struct T_conn_req *br;
	mblk_t *mp;
	vnode_t *peervn;
	int r;

	if (addrlen < sizeof(sa_family_t) ||
	    addrlen > sizeof(struct sockaddr_storage)) {
		kdprintf("sys_connect: invalid addrlen %u\n", addrlen);
		return -EINVAL;
	}

	str_reqlock(sh);
	if (sn->state != 0) {
		str_requnlock(sh);
		return -EINVAL;
	}
	ke_mutex_exit(sh->mutex);


	mp = str_allocb(sizeof(struct T_conn_req));
	if (mp == NULL) {
		str_requnlock_mutexunheld(sh);
		return -ENOMEM;
	}

	mp->db->type = M_PROTO;
	br = (struct T_conn_req *)mp->wptr;
	br->PRIM_type = T_CONN_REQ;
	mp->wptr += sizeof(struct T_conn_req);

	if (sn->domain == AF_UNIX) {
		sa_family_t fam;
		char path[109];
		vnode_t *boundvn;
		struct lookup_info li;
		struct sockaddr_ux *sux;

		if (addrlen < sizeof(sa_family_t) ||
		    addrlen > sizeof(struct sockaddr_un)) {
			str_requnlock_mutexunheld(sh);
			kdprintf("sys_connect: invalid addrlen %u (AF_UNIX)\n",
			    addrlen);
			return -EINVAL;
		}

		r = memcpy_from_user(&fam, addr, sizeof(sa_family_t));
		if (r != 0) {
			str_requnlock_mutexunheld(sh);
			kdprintf("sys_connect: copyin family failed\n");
			return -EFAULT;
		}

		if (fam != AF_UNIX) {
			str_requnlock_mutexunheld(sh);
			kdprintf("sys_connect: invalid family %u for AF_UNIX\n",
			    fam);
			return -EINVAL;
		}

		r = memcpy_from_user(path,
		    (const char *)addr + sizeof(sa_family_t),
		    addrlen - sizeof(sa_family_t));
		if (r != 0) {
			str_requnlock_mutexunheld(sh);
			kdprintf("sys_connect: copyin path failed\n");
			return -EFAULT;
		}

		path[addrlen - sizeof(sa_family_t)] = '\0';

		vfs_lookup_init(&li, root_nch, path, 0);
		r = vfs_lookup(&li);

		if (r != 0) {
			str_requnlock_mutexunheld(sh);
			kdprintf("sys_connect: vfs_lookup failed: %d\n", r);
			return r;
		}

		boundvn = li.result.nc->vp;
		if (boundvn->type != VSOCK || boundvn->sock.sockvn == NULL) {
			nchandle_release(li.result);
			str_requnlock_mutexunheld(sh);
			kdprintf("sys_connect: target is not a socket "
				"(type=%d, sock=%p)\n",
			    boundvn->type, boundvn->sock.sockvn);
			return -ENOTSOCK;
		}

		br->DEST_length = addrlen +
		    (offsetof(struct sockaddr_ux, sun_path) -
			offsetof(struct sockaddr_un, sun_path));

		sux = (struct sockaddr_ux *)&br->DEST;
		sux->sun_family = AF_UNIX;
		memcpy(sux->sun_path, path, addrlen - sizeof(sa_family_t));
		sux->sux_rq = VTOSN(boundvn->sock.sockvn)->stream->rq_bottom;

		peervn = boundvn->sock.sockvn;
		vn_retain(peervn);
		nchandle_release(li.result);

	} else {
		br->DEST_length = addrlen;
		r = memcpy_from_user(&br->DEST, addr, addrlen);
		if (r != 0) {
			str_requnlock_mutexunheld(sh);
			kdprintf("sys_bind: failed to copy addr from user\n");
			return -EFAULT;
		}
	}

	ke_event_init(&sn->conn_con_ev, false);
	sn->conn_con_mp = NULL;

	ke_mutex_enter(sh->mutex, "so_connect");
	sn->state |= SS_ISCONNECTING;
	mp = sock_tpi_request(sn, mp);
	r = sock_tpi_errno(mp);
	if (r != 0) {
		kfatal("so_connect failed");
	}
	ke_mutex_exit(sh->mutex);

	ke_wait1(&sn->conn_con_ev, "so_connect wait", false, ABSTIME_FOREVER);

	/* conn_con handling in sock_rput will set appropriate flags */

	str_requnlock_mutexunheld(sh);
	return 0;
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
sock_getattr(vnode_t *, vattr_t *attr)
{
	memset(attr, 0, sizeof(*attr));
	attr->type = VSOCK;
	return 0;
}

static int
sock_read(vnode_t *vn, void *buf, size_t buflen, off_t, int flags)
{
	struct socknode *sn = VTOSN(vn);
	return strread(sn->stream, (void *)buf, buflen, flags);
}

static int
sock_write(vnode_t *vn, const void *buf, size_t buflen, off_t, int flags)
{
	struct socknode *sn = VTOSN(vn);
	return strwrite(sn->stream, (void *)buf, buflen, flags);
}

static int
sock_ioctl(vnode_t *, unsigned long cmd, void *arg)
{
	ktodo();
}

static int
sock_chpoll(vnode_t *vn, struct poll_entry *pe, enum chpoll_mode mode)
{
	struct socknode *sn = VTOSN(vn);
	stdata_t *sh = sn->stream;
	int r;

	if (mode == CHPOLL_UNPOLL) {
		kassert(pe != NULL);
		pollhead_unregister(&sh->pollhead, pe);
		return 0;
	}

	if (pe != NULL)
		pollhead_register(&sh->pollhead, pe);

	ke_mutex_enter(sh->mutex, "sock_chpoll");

	r = 0;

	if (sn->state & SS_ISLISTENING) {
		if (sn->conn_indq_len > 0)
			r |= EPOLLIN | EPOLLRDNORM;
		ke_mutex_exit(sh->mutex);
		return r;
	}

	if ((sn->state & SS_ISCONNECTED) && !(sn->state & SS_CANTSENDMORE))
		r |= EPOLLOUT | EPOLLWRNORM;

	if (sh->rq->count > 0)
		r |= EPOLLIN | EPOLLRDNORM;

	if (sh->hanged_up)
		r |= EPOLLHUP;

	ke_mutex_exit(sh->mutex);

	return r;
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

	r = so_create(&fp, NULL, domain, type, protocol);
	if (r < 0) {
		uf_unreserve_fd(curproc()->finfo, fd);
		return r;
	}

	uf_install_reserved(curproc()->finfo, fd, fp);
	return fd;
}

static int
lookup_sockfd(int sockfd, file_t **fpp)
{
	file_t *fp = uf_lookup(curproc()->finfo, sockfd);
	if (fp == NULL)
		return -EBADF;
	if (fp->vnode->ops != &sock_vnops)
		return -ENOTSOCK;
	*fpp = fp;
	return 0;
}

int
sys_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
	file_t *fp;
	int r = lookup_sockfd(sockfd, &fp);
	if (r < 0)
		return r;
	r = so_accept4(VTOSN(fp->vnode), fp->flags & O_NONBLOCK, addr, addrlen,
	    flags);
	file_release(fp);
	return r;
}

int
sys_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	file_t *fp;
	int r = lookup_sockfd(sockfd, &fp);
	if (r < 0)
		return r;
	r = so_bind(fp->vnode, VTOSN(fp->vnode), addr, addrlen);
	file_release(fp);
	return r;
}

int
sys_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	file_t *fp;
	int r = lookup_sockfd(sockfd, &fp);
	if (r < 0)
		return r;
	r = so_connect(VTOSN(fp->vnode), addr, addrlen);
	file_release(fp);
	return r;
}

int
sys_listen(int sockfd, int backlog)
{
	file_t *fp;
	int r = lookup_sockfd(sockfd, &fp);
	if (r < 0)
		return r;
	r = so_listen(VTOSN(fp->vnode), backlog);
	file_release(fp);
	return r;
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
