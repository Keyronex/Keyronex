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
#include <sys/krx_file.h>
#include <sys/krx_user.h>
#include <sys/libkern.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/termios.h>

#include <net/if.h>

#include <fs/devfs/devfs.h>

static int do_unlink(stdata_t *upper_sh, int index);

extern kmutex_t proctree_mutex;

static struct qinit sth_rinit;
static struct qinit sth_winit;
static struct streamtab sth_streamtab;

static kmutex_t mux_links_lock = KMUTEX_INITIALISER;
static TAILQ_HEAD(, linkblk) mux_links = TAILQ_HEAD_INITIALIZER(mux_links);
static atomic_uint next_link_index = 0;

#define SIOCSIFADDR 0x8916
#define SIOCSIFNETMASK 0x891C
#define SIOCSIFNAMEBYMUXID 0x89A0

void
str_enter(stdata_t *sh, const char *reason)
{
	ke_mutex_enter(sh->mutex, reason);
}

void
str_req_begin(stdata_t *sh)
{
	struct req_waiter waiter;

	ke_mutex_enter(sh->mutex, "str_req_begin");

	if (!sh->req_locked) {
		sh->req_locked = true;
		return;
	}

	ke_event_init(&waiter.event, 0);
	TAILQ_INSERT_TAIL(&sh->req_waiters, &waiter, link);
	str_exit(sh);

	ke_wait1(&waiter.event, "str_req_begin", false, ABSTIME_FOREVER);

	str_enter(sh, "str_req_begin 2");
	return;
}

bool
str_req_trybegin(stdata_t *sh)
{
	if (!ke_mutex_tryenter(sh->mutex))
		return false;

	if (!sh->req_locked) {
		sh->req_locked = true;
		return true;
	} else {
		str_exit(sh);
		return false;
	}
}

void
str_req_end(stdata_t *sh)
{
	if (!TAILQ_EMPTY(&sh->req_waiters)) {
		struct req_waiter *waiter;
		waiter = TAILQ_FIRST(&sh->req_waiters);
		TAILQ_REMOVE(&sh->req_waiters, waiter, link);
		ke_event_set_signalled(&waiter->event, true);
	} else {
		sh->req_locked = false;
	}
	str_exit(sh);
}

void
str_req_end_unheld(stdata_t *sh)
{
	ke_mutex_enter(sh->mutex, "str_req_end_unheld");
	str_req_end(sh);
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
		if (wq->qinfo->qopen(wq, dev) != 0) {
			if (rq->qinfo->qclose != NULL)
				rq->qinfo->qclose(rq);
			return -1;
		}
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

	TAILQ_INIT(&sh->links);

	return sh;
}

stdata_t *
stropen(struct streamtab *devtab, void *dev, enum str_head_kind kind)
{
	stdata_t *sh;
	queue_t *devrq;

	sh = str_head_alloc(kind);
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

	if (qpair_open(sh->rq, dev) != 0) {
		qpair_free(devrq);
		qpair_free(sh->rq);
		kmem_free(sh, sizeof(*sh));
		return NULL;
	}
	if (qpair_open(devrq, dev) != 0) {
		if (sh->wq->qinfo->qclose)
			sh->wq->qinfo->qclose(sh->wq);
		if (sh->rq->qinfo->qclose)
			sh->rq->qinfo->qclose(sh->rq);
		qpair_free(devrq);
		qpair_free(sh->rq);
		kmem_free(sh, sizeof(*sh));
		return NULL;
	}
	return sh;
}

void strclose(stdata_t *sh)
{
	struct str_per_cpu_scheduler *sc =
	    ke_cpu_data[sh->home_cpu]->str_scheduler;
	ipl_t ipl;

	for (;;) {
		int index;
		str_enter(sh, "strclose unlink");
		if (TAILQ_EMPTY(&sh->links)) {
			str_exit(sh);
			break;
		}
		index = TAILQ_FIRST(&sh->links)->index;
		str_exit(sh);
		/* I_UNLINK can't be vetoed */
		do_unlink(sh, index);
	}

	ke_mutex_enter(sh->mutex, "strclose");

	atomic_fetch_or(&sh->flags, ST_FROZEN | ST_DEAD);

	kassert(!sh->req_locked);
	kassert(TAILQ_EMPTY(&sh->req_waiters));

	for (queue_t *nextwq, *wq = sh->wq->next; wq != NULL; wq = nextwq) {
		queue_t *rq = wq->other;

		nextwq = wq->next;

		if (wq->qinfo->qclose != NULL)
			wq->qinfo->qclose(wq);
		if (rq->qinfo->qclose != NULL)
			rq->qinfo->qclose(rq);

		str_flushq(rq, FLUSHALL);
		str_flushq(wq, FLUSHALL);

		qpair_free(rq);
	}

	str_flushq(sh->rq, FLUSHALL);
	str_flushq(sh->wq, FLUSHALL);
	qpair_free(sh->rq);

	if (sh->kind == STR_HEAD_KIND_TTY) {
		ke_mutex_enter(&proctree_mutex, "strclose tty");
		if (sh->tty_session != NULL) {
			sh->tty_session->ctty_str = NULL;
			sh->tty_session->ctty_vn = NULL;
			session_unref(sh->tty_session);
		}
		if (sh->tty_pgrp != NULL)
			pgrp_unref(sh->tty_pgrp);
		ke_mutex_exit(&proctree_mutex);
	}

	kassert(LIST_EMPTY(&sh->pollhead.pollers));

	ipl = ke_spinlock_enter(&sh->ingress_lock);
	str_mblk_q_free(&sh->ingress_head);
	ke_spinlock_exit(&sh->ingress_lock, ipl);

	str_exit(sh);

	/* by this point, no one is left able to kick the stream */

	ipl = ke_spinlock_enter(&sc->lock);
	if (sh->flags & ST_QUEUED)
		TAILQ_REMOVE(&sc->runq, sh, sched_link);
	TAILQ_INSERT_TAIL(&sc->freeq, sh, sched_link);
	ke_spinlock_exit(&sc->lock, ipl);
}

int
strpush(stdata_t *sh, struct streamtab *tab)
{
	queue_t *newrq, *newwq;
	queue_t *first_wq, *first_rq;

	newrq = qpair_alloc(sh, tab);
	if (newrq == NULL)
		return -ENOMEM;

	newwq = newrq->other;

	str_req_begin(sh);

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
		str_req_end(sh);
		return -ENOMEM;
	}

		str_req_end(sh);
	return 0;
}

int
strread(stdata_t *sh, void *buf, size_t len, int options)
{
	size_t ncopied = 0;

	if (len == 0)
		return 0;

	str_req_begin(sh);

	while (ncopied < len) {
		mblk_t *mp, *bp;
		int r;

		while (TAILQ_EMPTY(&sh->rq->msgq)) {
			if (ncopied > 0) {
				str_req_end(sh);
				return ncopied;
			}

			if (sh->hanged_up) {
				str_req_end(sh);
				return 0;
			}

			if (options & O_NONBLOCK) {
				str_req_end(sh);
				return -EWOULDBLOCK;
			}

			ke_event_set_signalled(&sh->data_readable, false);
			str_req_end(sh);

			ke_wait1(&sh->data_readable, "sth_read", true,
			    ABSTIME_FOREVER);

			str_req_begin(sh);
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
				str_req_end(sh);
				return ncopied > 0 ? ncopied : r;
			}

			ncopied += tocopy;

			if ((options & MSG_PEEK) == 0) {
				bp->rptr += tocopy;
				sh->rq->count -= tocopy;
				if (sh->rq->full &&
				    sh->rq->count <= sh->rq->lowat) {
					sh->rq->full = false;
					if (sh->rq->wantw)
						str_backenable(sh->rq);
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

	str_req_end(sh);

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

	str_req_begin(sh);
	str_exit(sh);

	r = memcpy_from_user(mp->wptr, buf, len);
	if (r < 0) {
		str_req_end_unheld(sh);
		str_freeb(mp);
		return r;
	}

	mp->wptr += len;

	str_enter(sh, "str_write");

	if (sh->kind == STR_HEAD_KIND_FIFO && sh->nreaders == 0) {
		str_freeb(mp);
		str_req_end(sh);
		/* TODO: send SIGPIPE to process */
		return -EPIPE;
	}

	str_putnext(sh->wq, mp);
	str_req_end(sh);

	return len;
}

static int
do_setctty(struct vnode *vn, stdata_t *sh)
{
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

/* convert a muxed stream back into one with a normal head */
static void
unmux(stdata_t *lower_sh)
{
	lower_sh->rq->qinfo = &sth_rinit;
	lower_sh->wq->qinfo = &sth_winit;
	lower_sh->rq->ptr = lower_sh;
	lower_sh->wq->ptr = lower_sh;
}

static int
do_link_common(stdata_t *upper_sh, int fd, int cmd)
{
	struct file *lowerfp;
	stdata_t *lower_sh;
	struct linkblk *linkp;
	struct strioctl *ioc;
	mblk_t *iocmp;
	int r;

	kassert(cmd == I_LINK || cmd == I_PLINK);

	lowerfp = uf_lookup(curproc()->finfo, fd);
	if (lowerfp == NULL)
		return -EBADF;

	lower_sh = devfs_spec_get_stream(lowerfp->vnode);
	if (lower_sh == NULL || lower_sh == upper_sh) {
		file_release(lowerfp);
		return -EINVAL;
	}

	linkp = kmem_alloc(sizeof(*linkp));
	if (linkp == NULL) {
		file_release(lowerfp);
		return -ENOMEM;
	}

	iocmp = str_allocb(sizeof(struct strioctl));
	if (iocmp == NULL) {
		kmem_free(linkp, sizeof(*linkp));
		file_release(lowerfp);
		return -ENOMEM;
	}

	iocmp->db->type = M_IOCTL;
	ioc = (typeof(ioc))iocmp->rptr;
	ioc->ic_cmd = cmd;
	ioc->ic_len = sizeof(*linkp);
	ioc->ic_dp = linkp;
	iocmp->wptr += sizeof(*ioc);

	/* lock out concurrent read/write/ioctl on both streams */
	if (lower_sh < upper_sh) {
		str_req_begin(lower_sh);
		str_req_begin(upper_sh);
	} else {
		str_req_begin(upper_sh);
		str_req_begin(lower_sh);
	}

	/* exit upper stream; we don't need its mutex yet */
	str_exit(upper_sh);

	/* freeze the lower stream so we can manipulate it */
	str_freeze(lower_sh);

	/* replace the stream head with the mux routines */
	kassert(lower_sh->rq->qinfo == &sth_rinit &&
	    lower_sh->wq->qinfo == &sth_winit);
	kassert(lower_sh->wq->ptr == lower_sh && lower_sh->rq->ptr == lower_sh);

	lower_sh->rq->qinfo = upper_sh->devtab->muxrinit;
	lower_sh->wq->qinfo = upper_sh->devtab->muxwinit;
	lower_sh->rq->ptr = lower_sh->wq->ptr = NULL;

	linkp->index = atomic_fetch_add(&next_link_index, 1);
	if (cmd == I_LINK)
		linkp->qtop = upper_sh->rq_bottom->other;
	else
		linkp->qtop = NULL;
	linkp->qbot = lower_sh->wq;
	if (cmd == I_PLINK)
		linkp->tabtop = upper_sh->devtab;
	else
		linkp->tabtop = NULL;

	r = qpair_open(lower_sh->rq, NULL);
	if (r != 0) {
		/* restore original qinfo */
		unmux(lower_sh);
		str_thaw(lower_sh);
		str_exit(lower_sh);
		str_req_end_unheld(upper_sh);
		str_req_end_unheld(lower_sh);
		str_freeb(iocmp);
		kmem_free(linkp, sizeof(*linkp));
		file_release(lowerfp);
		return r;
	}

	/* lower stream now free to run again */
	str_thaw(lower_sh);
	str_exit(lower_sh);

	/* now enter the upper stream and send ioctl down */
	str_enter(upper_sh, "do_link_common ioctl");
	ke_event_set_signalled(&upper_sh->ioctl_done_ev, false);
	str_putnext(upper_sh->wq, iocmp);
	str_exit(upper_sh);

	ke_wait1(&upper_sh->ioctl_done_ev, "do_link_common wait ioctl", true,
	    ABSTIME_FOREVER);

	str_enter(upper_sh, "do_link_common post-ioctl");

	if (iocmp->db->type == M_IOCACK) {
		linkp->lowerfp = file_retain(lowerfp);

		if (cmd == I_PLINK) {
			ke_mutex_enter(&mux_links_lock,
			    "do_link_common plink insert");
			TAILQ_INSERT_TAIL(&mux_links, linkp, link);
			ke_mutex_exit(&mux_links_lock);
		} else {
			TAILQ_INSERT_TAIL(&upper_sh->links, linkp, link);
		}

		r = linkp->index;
	} else {
		kassert(iocmp->db->type == M_IOCNAK);

		/* link rejected by driver, undo it */
		str_exit(upper_sh);

		str_enter(lower_sh, "do_link_common nak undo");
		str_freeze(lower_sh);

		if (lower_sh->rq->qinfo->qclose != NULL)
			lower_sh->rq->qinfo->qclose(lower_sh->rq);
		if (lower_sh->wq->qinfo->qclose != NULL)
			lower_sh->wq->qinfo->qclose(lower_sh->wq);

		unmux(lower_sh);

		str_thaw(lower_sh);
		str_exit(lower_sh);

		str_enter(upper_sh, "do_link_common nak cleanup");
		kmem_free(linkp, sizeof(*linkp));

		r = -EINVAL;
	}

	str_freeb(iocmp);
	str_req_end(upper_sh);
	str_req_end_unheld(lower_sh);

	file_release(lowerfp);

	return r;
}

static void
link_teardown(struct linkblk *linkp)
{
	stdata_t *lower_sh;

	lower_sh = devfs_spec_get_stream(linkp->lowerfp->vnode);
	kassert(lower_sh != NULL);
	kassert(lower_sh->req_locked);

	/* freeze lower stream while we manipulate it */
	str_enter(lower_sh, "link_teardown");
	str_freeze(lower_sh);

	/* close the mux side of the queue pair */
	if (lower_sh->wq->qinfo->qclose != NULL)
		lower_sh->wq->qinfo->qclose(lower_sh->wq);
	if (lower_sh->rq->qinfo->qclose != NULL)
		lower_sh->rq->qinfo->qclose(lower_sh->rq);

	/* flush any residual messages */
	str_flushq(lower_sh->rq, FLUSHALL);
	str_flushq(lower_sh->wq, FLUSHALL);

	/* restore the stream head qinit */
	unmux(lower_sh);

	str_thaw(lower_sh);
	str_req_end(lower_sh);

	file_release(linkp->lowerfp);
}

static int
do_unlink_common(stdata_t *upper_sh, stdata_t *lower_sh, struct linkblk *linkp,
    int cmd)
{
	struct strioctl *ioc;
	mblk_t *iocmp;

	/* both req-locks are held, but we don't need the inner mutexes yet */
	str_exit(upper_sh);
	str_exit(lower_sh);

	iocmp = str_allocb(sizeof(struct strioctl));
	if (iocmp == NULL) {
		return -ENOMEM;
	}

	iocmp->db->type = M_IOCTL;
	ioc = (typeof(ioc))iocmp->rptr;
	ioc->ic_cmd = cmd;
	ioc->ic_len = sizeof(*linkp);
	ioc->ic_dp = linkp;
	iocmp->wptr += sizeof(*ioc);

	/* send unlink ioctl do the upper stream */
	str_enter(upper_sh, "do_unlink_common ioctl");
	ke_event_set_signalled(&upper_sh->ioctl_done_ev, false);
	str_putnext(upper_sh->wq, iocmp);
	str_exit(upper_sh);

	ke_wait1(&upper_sh->ioctl_done_ev, "do_unlink_common wait ioctl", true,
	    ABSTIME_FOREVER);

	str_enter(upper_sh, "do_unlink_common post-ioctl");

	if (iocmp->db->type == M_IOCNAK)
		kfatal("driver tried to refuse legitimate I_UNLINK/I_PUNLINK");

	kassert(iocmp->db->type == M_IOCACK);
	str_exit(upper_sh);

	str_freeb(iocmp);

	return 0;
}

/*
 * Because we must search upper_sh->links with the upper reqlock held to find
 * the linkblk (and only then learn which lower stream is involved), we may
 * find we'd have to reqlock a lower stream out-of-order. If we can't then
 * try-enter the reqlock on the lower stream, we must try again.
 */
static int
do_unlink(stdata_t *upper_sh, int index)
{
	struct linkblk *linkp;
	stdata_t *lower_sh;
	int r;

retry:
	str_req_begin(upper_sh);

	TAILQ_FOREACH(linkp, &upper_sh->links, link) {
		if (linkp->index == index)
			break;
	}

	if (linkp == NULL) {
		str_req_end(upper_sh);
		return -EINVAL;
	}

	lower_sh = devfs_spec_get_stream(linkp->lowerfp->vnode);
	kassert(lower_sh != NULL);
	kassert(lower_sh != upper_sh);

	if (lower_sh > upper_sh) {
		str_req_begin(lower_sh);
	} else if (!str_req_trybegin(lower_sh)) {
		file_t *fp = file_retain(linkp->lowerfp);
		int saved_index = linkp->index;
		struct linkblk *var;

		str_req_end(upper_sh);

		str_req_begin(lower_sh);
		str_req_begin(upper_sh);

		TAILQ_FOREACH(var, &upper_sh->links, link) {
			if (var->index == saved_index &&
			    var->lowerfp == fp)
				break;
		}

		if (var == NULL) {
			str_req_end(upper_sh);
			str_req_end(lower_sh);
			file_release(fp);
			goto retry;
		}

		linkp = var;
		file_release(fp);
	}

	r = do_unlink_common(upper_sh, lower_sh, linkp, I_UNLINK);
	if (r == 0) {
		str_enter(upper_sh, "do_unlink finally");
		TAILQ_REMOVE(&upper_sh->links, linkp, link);
		str_req_end(upper_sh);

		link_teardown(linkp);
		kmem_free(linkp, sizeof(*linkp));
	} else {
		str_req_end_unheld(upper_sh);
		str_req_end_unheld(lower_sh);
	}

	return r;
}

static int
do_punlink(stdata_t *upper_sh, int index)
{
	struct linkblk *linkp;
	stdata_t *lower_sh;
	int r;

	ke_mutex_enter(&mux_links_lock, "do_punlink find");

	TAILQ_FOREACH(linkp, &mux_links, link) {
		if (linkp->index == index)
			break;
	}

	if (linkp == NULL || linkp->tabtop != upper_sh->devtab) {
		ke_mutex_exit(&mux_links_lock);
		return -EINVAL;
	}

	/* after removing it, the link is in our custody */
	TAILQ_REMOVE(&mux_links, linkp, link);
	ke_mutex_exit(&mux_links_lock);

	lower_sh = devfs_spec_get_stream(linkp->lowerfp->vnode);
	kassert(lower_sh != NULL);
	kassert(lower_sh != upper_sh);

	if (lower_sh < upper_sh) {
		str_req_begin(lower_sh);
		str_req_begin(upper_sh);
	} else {
		str_req_begin(upper_sh);
		str_req_begin(lower_sh);
	}

	r = do_unlink_common(upper_sh, lower_sh, linkp, I_PUNLINK);
	if (r == 0) {
		link_teardown(linkp);
		kmem_free(linkp, sizeof(*linkp));
		str_req_end_unheld(upper_sh);
	} else {
		/* put it back if unlink failed */
		ke_mutex_enter(&mux_links_lock, "do_punlink enomem");
		TAILQ_INSERT_TAIL(&mux_links, linkp, link);
		ke_mutex_exit(&mux_links_lock);
		str_req_end_unheld(upper_sh);
		str_req_end_unheld(lower_sh);
	}

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

	case I_LINK:
	case I_PLINK:
		return do_link_common(sh, (int)(intptr_t)arg, cmd);

	case I_UNLINK:
		return do_unlink(sh, (int)(intptr_t)arg);

	case I_PUNLINK:
		return do_punlink(sh, (int)(intptr_t)arg);

#if 0
	case SIOCADDRT:
		in_size = sizeof(struct rtentry);
		break;
#endif

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
	ioc->ic_cmd = cmd;
	ioc->ic_len = MAX2(in_size, out_size);
	ioc->ic_dp = data;
	mp->wptr += sizeof(struct strioctl);
	mp->db->type = M_IOCTL;

	str_req_begin(sh);
	ke_event_set_signalled(&sh->ioctl_done_ev, false);
	str_putnext(sh->wq, mp);
	str_exit(sh);

	ke_wait1(&sh->ioctl_done_ev, "str_ioctl", true, ABSTIME_FOREVER);

	ke_mutex_enter(sh->mutex, "strioctl");
	str_req_end(sh);

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

	str_exit(sh);

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
	case M_IOCNAK:
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

static struct qinit sth_rinit = {
	.putp = sth_rput,
	.hiwat = 536,
	.lowat = 128,
};

static struct qinit sth_winit = {
	.putp = sth_wput,
	.srvp = sth_wsrv,
};

static struct streamtab sth_streamtab = {
	.rinit = &sth_rinit,
	.winit = &sth_winit,
};
