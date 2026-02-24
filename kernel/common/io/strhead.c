/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file strhead.c
 * @brief Stream head operations.
 */

#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/termios.h>

extern kmutex_t proctree_mutex;

static struct streamtab sth_streamtab;

void
str_enter(stdata_t *sh, const char *reason)
{
	ke_mutex_enter(sh->mutex, reason);
}

void
str_exit(stdata_t *sh)
{
	ke_mutex_exit(sh->mutex);
}

void
str_reqlock(stdata_t *sh)
{
	struct req_waiter waiter;

	ke_mutex_enter(sh->mutex, "str_reqlock");

	if (!sh->req_locked) {
		sh->req_locked = true;
		return;
	}

	ke_event_init(&waiter.event, 0);
	TAILQ_INSERT_TAIL(&sh->req_waiters, &waiter, link);
	ke_mutex_exit(sh->mutex);

	ke_wait1(&waiter.event, "str_reqlock", false, ABSTIME_FOREVER);

	return ke_mutex_enter(sh->mutex, "str_reqlock 2");
}

void
str_requnlock(stdata_t *sh)
{
	if (!TAILQ_EMPTY(&sh->req_waiters)) {
		struct req_waiter *waiter;
		waiter = TAILQ_FIRST(&sh->req_waiters);
		TAILQ_REMOVE(&sh->req_waiters, waiter, link);
		ke_event_set_signalled(&waiter->event, true);
	} else {
		sh->req_locked = false;
	}
	ke_mutex_exit(sh->mutex);
}

void
str_requnlock_mutexunheld(stdata_t *sh)
{
	ke_mutex_enter(sh->mutex, "str_requnlock_mutexunheld");
	str_requnlock(sh);
}

static queue_t *
qpair_alloc(stdata_t *s, struct streamtab *tab)
{
	queue_t *rq, *wq;

	rq = kmem_alloc(sizeof(*rq));
	if (rq == NULL)
		return NULL;

	wq = kmem_alloc(sizeof(*wq));
	if (wq == NULL) {
		kmem_free(rq, sizeof(*rq));
		return NULL;
	}

	rq->is_readq = true;
	wq->is_readq = false;

	rq->enabled = wq->enabled = false;
	rq->wantw = wq->wantw = false;
	rq->full = wq->full = false;

	rq->other = wq;
	wq->other = rq;

	rq->stdata = wq->stdata = s;
	TAILQ_INIT(&rq->msgq);
	TAILQ_INIT(&wq->msgq);
	rq->count = wq->count = 0;
	rq->ptr = wq->ptr = NULL;
	rq->next = wq->next = NULL;
	rq->back = wq->back = NULL;

	rq->qinfo = tab->rinit;
	wq->qinfo = tab->winit;

	return rq;
}

static void
qpair_free(queue_t *rq)
{
	queue_t *wq = rq->other;
	kmem_free(rq, sizeof(*rq));
	kmem_free(wq, sizeof(*wq));
}

static int
qpair_open(queue_t *rq, void *dev)
{
	queue_t *wq = rq->other;

	if (rq->qinfo->qopen != NULL) {
		if (rq->qinfo->qopen(rq, dev) != 0)
			return -1;
	}

	if (wq->qinfo->qopen != NULL) {
		if (wq->qinfo->qopen(wq, dev) != 0)
			return -1;
	}

	return 0;
}

stdata_t *
str_head_alloc(enum str_head_kind kind)
{
	stdata_t *sh;

	sh = kmem_alloc(sizeof(*sh));
	if (sh == NULL)
		return NULL;

	sh->kind = kind;
	ke_mutex_init(&sh->integral_mutex);
	sh->mutex = &sh->integral_mutex;
	sh->rq_bottom = NULL;

	ke_spinlock_init(&sh->ingress_lock);
	TAILQ_INIT(&sh->ingress_head);

	sh->flags = 0;
	sh->home_cpu = CPU_LOCAL_LOAD(cpu_num);

	sh->req_locked = false;
	TAILQ_INIT(&sh->req_waiters);
	sh->hanged_up = false;
	ke_event_init(&sh->data_readable, 0);
	ke_event_init(&sh->ioctl_done_ev, 0);
	sh->read_mode = STR_RNORM;

	pollhead_init(&sh->pollhead);

	if (kind == STR_HEAD_KIND_TTY) {
		sh->tty_pgrp = NULL;
		sh->tty_session = NULL;
	} else if (kind == STR_HEAD_KIND_FIFO) {
		sh->nreaders = 0;
		sh->nwriters = 0;
	}

	sh->rq = qpair_alloc(sh, &sth_streamtab);
	if (sh->rq == NULL) {
		kmem_free(sh, sizeof(*sh));
		return NULL;
	}
	sh->wq = sh->rq->other;
	sh->rq->ptr = sh->wq->ptr = sh;

	return sh;
}

stdata_t *
stropen(struct streamtab *devtab, void *dev, enum str_head_kind kind)
{
	stdata_t *sh;
	queue_t *devrq;

	sh = str_head_alloc(STR_HEAD_KIND_FIFO);
	if (sh == NULL)
		return NULL;

	sh->devtab = devtab;

	devrq = qpair_alloc(sh, devtab);
	if (devrq == NULL) {
		qpair_free(sh->rq);
		kmem_free(sh, sizeof(*sh));
		return NULL;
	}

	devrq->next = sh->rq;
	sh->rq->back = devrq;
	sh->wq->next = devrq->other;
	devrq->other->back = sh->wq;
	sh->rq_bottom = devrq;

	if (qpair_open(sh->rq, dev) != 0 || qpair_open(devrq, dev) != 0) {
		qpair_free(devrq);
		qpair_free(sh->rq);
		kmem_free(sh, sizeof(*sh));
		return NULL;
	}

	return sh;
}

int
strpush(stdata_t *sh, struct streamtab *tab)
{
	queue_t *newrq, *newwq;
	queue_t *first_wq, *first_rq;

	ke_mutex_enter(sh->mutex, "str_head_push");

	newrq = qpair_alloc(sh, tab);
	if (newrq == NULL) {
		ke_mutex_exit(sh->mutex);
		return -ENOMEM;
	}
	newwq = newrq->other;

	first_wq = sh->wq->next;
	first_rq = first_wq->other;

	newwq->next = first_wq;
	first_wq->back = newwq;
	sh->wq->next = newwq;
	newwq->back = sh->wq;
	newrq->next = sh->rq;
	sh->rq->back = newrq;
	first_rq->next = newrq;
	newrq->back = first_rq;

	if (qpair_open(newrq, NULL) != 0) {
		sh->wq->next = first_wq;
		first_wq->back = sh->wq;
		first_rq->next = sh->rq;
		sh->rq->back = first_rq;
		qpair_free(newrq);
		ke_mutex_exit(sh->mutex);
		return -ENOMEM;
	}

	ke_mutex_exit(sh->mutex);
	return 0;
}

int
strread(stdata_t *sh, void *buf, size_t len, int options)
{
	size_t ncopied = 0;

	if (len == 0)
		return 0;

	str_reqlock(sh);

	while (ncopied < len) {
		mblk_t *mp, *bp;
		int r;

		while (TAILQ_EMPTY(&sh->rq->msgq)) {
			if (ncopied > 0) {
				str_requnlock(sh);
				return ncopied;
			}

			if (sh->hanged_up) {
				str_requnlock(sh);
				return 0;
			}

			if (options & O_NONBLOCK) {
				str_requnlock(sh);
				return -EWOULDBLOCK;
			}

			ke_event_set_signalled(&sh->data_readable, false);
			str_requnlock(sh);

			ke_wait1(&sh->data_readable, "sth_read", true,
			    ABSTIME_FOREVER);

			str_reqlock(sh);
		}

		mp = TAILQ_FIRST(&sh->rq->msgq);

		for (bp = mp; bp != NULL && ncopied < len; bp = bp->cont) {
			size_t avail, tocopy;

			avail = bp->wptr - bp->rptr;
			if (avail == 0)
				continue;

			tocopy = (avail < len - ncopied) ? avail :
							   (len - ncopied);

			/* todo: release mutex while doing memcpy_to_user */
			r = memcpy_to_user((char *)buf + ncopied, bp->rptr,
			    tocopy);
			if (r < 0) {
				str_requnlock(sh);
				return ncopied > 0 ? ncopied : r;
			}

			ncopied += tocopy;

			if ((options & MSG_PEEK) == 0) {
				bp->rptr += tocopy;
				sh->rq->count -= tocopy;
				if (sh->rq->full &&
				    sh->rq->count <= sh->rq->lowat) {
					sh->rq->full = false;
					if (sh->rq->wantw) {
#if 0 /* implement me */
						str_backenable(sh->rq);
#endif
					}
				}
			}
		}

		if ((options & MSG_PEEK) == 0) {
			if (str_msgsize(mp) == 0) {
				TAILQ_REMOVE(&sh->rq->msgq, mp, link);
				str_freemsg(mp);

				if (sh->read_mode != STR_RNORM)
					break;
			} else {
				if (sh->read_mode == STR_RMSGD) {
					sh->rq->count -= str_msgsize(mp);
					TAILQ_REMOVE(&sh->rq->msgq, mp, link);
					str_freemsg(mp);
				}
				/* RNORM/RMSGN leave the partial message in
				 * queue */
				break;
			}
		} else {
			/* not correct for STR_RNORM! */
			break;
		}
	}

	str_requnlock(sh);

	return ncopied;
}

int
strwrite(stdata_t *sh, const void *buf, size_t len, int)
{
	int r;
	mblk_t *mp;

	mp = str_allocb(len);
	if (mp == NULL) {
		return -ENOMEM;
	}

	str_reqlock(sh);
	ke_mutex_exit(sh->mutex);

	r = memcpy_from_user(mp->wptr, buf, len);
	if (r < 0) {
		str_requnlock_mutexunheld(sh);
		str_freeb(mp);
		return r;
	}

	mp->wptr += len;

	ke_mutex_enter(sh->mutex, "str_write");

	if (sh->kind == STR_HEAD_KIND_FIFO && sh->nreaders == 0) {
		str_freeb(mp);
		str_requnlock(sh);
		/* TODO: send SIGPIPE to process */
		return -EPIPE;
	}

	str_putnext(sh->wq, mp);
	str_requnlock(sh);

	return len;
}

static int
do_setctty(struct vnode *vn, stdata_t *sh)
{
	ipl_t ipl;
	struct session *sess;
	proc_t *proc = curproc();

	ke_mutex_enter(&proctree_mutex, "do_setctty");

	if (proc->pgrp == NULL) {
		ke_mutex_exit(&proctree_mutex);
		return -ESRCH;
	}

	sess = proc->pgrp->session;

	if (sess->leader != proc) {
		/* process is not the session leader */
		ke_mutex_exit(&proctree_mutex);
		return -EPERM;
	}

	if (sess->ctty_str != NULL) {
		/* session has a controlling terminal already */
		ke_mutex_exit(&proctree_mutex);
		return -EPERM;
	}

	session_ref(sess);
	pgrp_ref(proc->pgrp);

	sh->tty_session = sess;
	sh->tty_pgrp = proc->pgrp;

	sess->ctty_str = sh;
	sess->ctty_vn = vn;

	ke_mutex_exit(&proctree_mutex);

	return 0;
}

int
strioctl(struct vnode *vn, stdata_t *sh, unsigned long cmd, void *arg)
{
	mblk_t *mp;
	void *data = NULL;
	size_t in_size = 0, out_size = 0;
	struct strioctl *ioc;
	int r;

	switch (cmd) {
	case TIOCSCTTY:
		r = do_setctty(vn, sh);
		return r;

	case TIOCGPGRP: {
		pid_t pgrp_id;

		str_enter(sh, "TIOCGPGRP");
		if (sh->tty_pgrp == NULL) {
			str_exit(sh);
			kdprintf("str_ioctl: TIOCGPGRP with no tty_pgrp\n");
			return -ENOTTY;
		}
		pgrp_id = sh->tty_pgrp->pgid;
		str_exit(sh);

		return memcpy_to_user(arg, &pgrp_id, sizeof(pid_t));
	}

	case TIOCSPGRP: {
		pid_t pgrp_id;
		struct pgrp *pgrp;
		struct proc *proc = curproc();

		if (memcpy_from_user(&pgrp_id, arg, sizeof(pid_t)))
			return -EFAULT;

		ke_mutex_enter(&proctree_mutex, "TIOCSPGRP");

		if (sh->tty_session == NULL || proc->pgrp == NULL ||
		    proc->pgrp->session != sh->tty_session) {
			ke_mutex_exit(&proctree_mutex);
			kdprintf("str_ioctl: TIOCSPGRP with no tty_session\n");
			return -ENOTTY;
		}

		pgrp = NULL;
		LIST_FOREACH(pgrp, &sh->tty_session->pgrps, session_link) {
			if (pgrp->pgid == pgrp_id)
				break;
		}

		if (pgrp == NULL) {
			ke_mutex_exit(&proctree_mutex);
			kdprintf(
			    "str_ioctl: TIOCSPGRP with no matching pgrp\n");
			return -ESRCH;
		}

		pgrp_ref(pgrp);

		str_enter(sh, "TIOCSPGRP");
		if (sh->tty_pgrp != NULL)
			pgrp_unref(sh->tty_pgrp);
		sh->tty_pgrp = pgrp;
		str_exit(sh);

		ke_mutex_exit(&proctree_mutex);
		return 0;
	}

	case TIOCGWINSZ:
		out_size = sizeof(struct winsize);
		break;

	case TIOCSWINSZ:
		in_size = sizeof(struct winsize);
		break;

	case TCGETS:
		out_size = sizeof(struct termios);
		break;

	case TCSETS:
	case TCSETSW:
	case TCSETSF:
		in_size = sizeof(struct termios);
		break;

#if 0
	case I_PLINK:
		return plink(sh, (int)(intptr_t)arg);

	case SIOCADDRT:
		in_size = sizeof(struct rtentry);
		break;

	case SIOCGIFFLAGS:
	case SIOCGIFADDR:
	case SIOCGIFNETMASK:
		in_size = sizeof(struct ifreq);
		out_size = sizeof(struct ifreq);
		break;

	case SIOCSIFFLAGS:
	case SIOCSIFADDR:
	case SIOCSIFNETMASK:
		in_size = sizeof(struct ifreq);
		break;

	case SIOCSIFNAMEBYMUXID:
		in_size = sizeof(struct ifreq);
		break;
#endif

	default:
		kfatal("str_ioctl: unhandled ioctl %lu/0x%x\n", cmd, cmd);
	}

	if (in_size != 0 || out_size != 0) {
		data = kmem_alloc(MAX2(in_size, out_size));
		if (data == NULL)
			return -ENOMEM;
	}

	if (in_size > 0) {
		r = memcpy_from_user(data, arg, in_size);
		if (r < 0) {
			kmem_free(data, MAX2(in_size, out_size));
			return r;
		}
	}

	mp = str_allocb(sizeof(struct strioctl));
	if (mp == NULL) {
		kmem_free(data, MAX2(in_size, out_size));
		return -ENOMEM;
	}

	ioc = (struct strioctl *)mp->rptr;
	ioc->cmd = cmd;
	ioc->len = MAX2(in_size, out_size);
	ioc->data = data;
	mp->wptr += sizeof(struct strioctl);
	mp->db->type = M_IOCTL;

	str_reqlock(sh);
	ke_event_set_signalled(&sh->ioctl_done_ev, false);
	str_putnext(sh->wq, mp);
	ke_mutex_exit(sh->mutex);

	ke_wait1(&sh->ioctl_done_ev, "str_ioctl", true, ABSTIME_FOREVER);

	ke_mutex_enter(sh->mutex, "strioctl");
	str_requnlock(sh);

	kassert(mp->db->type == M_IOCACK || mp->db->type == M_IOCNAK);

	if (out_size > 0 && mp->db->type == M_IOCACK) {
		r = memcpy_to_user(arg, data, out_size);
		if (r < 0) {
			kmem_free(data, MAX2(in_size, out_size));
			return r;
		}
	} else if (mp->db->type == M_IOCNAK) {
		kfatal("handle negative ioctl ack\n");
	} else {
		r = 0;
	}

	kmem_free(data, MAX2(in_size, out_size));
	str_freeb(mp);

	return r;
}

int
strchpoll(stdata_t *sh, struct poll_entry *pe, enum chpoll_mode mode)
{
	int r = 0;

	if (mode == CHPOLL_UNPOLL) {
		kassert(pe != NULL);
		pollhead_unregister(&sh->pollhead, pe);
		return 0;
	}

	if (pe != NULL)
		pollhead_register(&sh->pollhead, pe);

	ke_mutex_enter(sh->mutex, "strchpoll");

	r = 0;

	if (sh->kind == STR_HEAD_KIND_FIFO) {
		if (sh->nreaders > 0)
			r |= EPOLLOUT;
		else
			r |= EPOLLERR;
	} else {
		r |= EPOLLOUT;
	}

	if (sh->rq->count > 0)
		r |= EPOLLIN | EPOLLRDNORM;
	else if (sh->read_mode == STR_RMSGN && !TAILQ_EMPTY(&sh->rq->msgq))
		r |= EPOLLIN | EPOLLRDNORM; /* canon tty, zero msg for EOF */

	if (sh->hanged_up)
		r |= EPOLLHUP;

	ke_mutex_exit(sh->mutex);

	return r;
}

static void
sth_rput(queue_t *q, mblk_t *mp)
{
	stdata_t *sh = (stdata_t *)q->ptr;

	switch (mp->db->type) {
	case M_DATA:

		q->count += str_msgsize(mp);

		/* flow control (c.f. str_putq) */
		if (q->count > q->hiwat)
			q->full = true;

		ke_event_set_signalled(&sh->data_readable, true);
		TAILQ_INSERT_TAIL(&sh->rq->msgq, mp, link);
		pollhead_deliver_events(&sh->pollhead, EPOLLIN | EPOLLRDNORM);
		break;

	case M_HANGUP:
		str_freeb(mp);
		sh->hanged_up = true;
		pollhead_deliver_events(&sh->pollhead, EPOLLHUP);

#if 0
		if (sh->tty_pgrp != NULL)
			kdprintf(" = TODO str_head: SIGHUP to pgrp %d\n",
			    sh->tty_pgrp->pgid);
#endif

		break;

	case M_SETOPTS: {
		struct stroptions *sop = (struct stroptions *)mp->rptr;

		if (sop->flags & SO_READMODE)
			sh->read_mode = sop->readopt;

		str_freeb(mp);
		break;
	}

	case M_IOCACK:
		ke_event_set_signalled(&sh->ioctl_done_ev, true);
		break;

	default:
		kfatal("sth_rput: unhandled message type %d\n", mp->db->type);
	}
}

static void
sth_wput(queue_t *q, mblk_t *mp)
{
	kassert(mp->db->type == M_DATA || mp->db->type == M_IOCTL);
	str_putnext(q->other, mp);
}

static void
sth_wsrv(queue_t *q)
{
	kfatal("TODO: sth_wsrv: may unblock writers, wake pollers\n");
}

static struct qinit rinit = {
	.putp = sth_rput,
	.hiwat = 536,
	.lowat = 128,
};

static struct qinit winit = {
	.putp = sth_wput,
	.srvp = sth_wsrv,
};

static struct streamtab sth_streamtab = {
	.rinit = &rinit,
	.winit = &winit,
};
