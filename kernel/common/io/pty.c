/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Apr 11 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file pty.c
 * @brief Unix98 pseudoterminals
 *
 * TODO:
 * -----
 *
 * - flow control?
 * - ptem module?
 * - share a mutex between the master and slave so we can twist them?
 */

#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/k_thread.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/termios.h>

#include <fs/devfs/devfs.h>
#include <stdbool.h>

#define PTY_MAX 256

extern struct streamtab ldterm_streamtab;

struct pty {
	kmutex_t lock;
	int	index;	/* /dev/ptsN */
	bool	locked;
	bool	master_open;
	bool	slave_open;
	queue_t	*master_rq;
	queue_t	*slave_rq;

	struct winsize winsize;	/* could move into a ptem module  */
};

static kmutex_t pty_lock = KMUTEX_INITIALISER;
static struct pty *pty_table[PTY_MAX];

static int ptm_ropen(queue_t *rq, void *dev);
static void ptm_rclose(queue_t *rq);
static void ptm_wput(queue_t *wq, mblk_t *mp);

static int pts_ropen(queue_t *rq, void *dev);
static void pts_rclose(queue_t *rq);
static void pts_wput(queue_t *wq, mblk_t *mp);

static struct qinit ptm_rinit = {
	.qopen = ptm_ropen,
	.qclose = ptm_rclose,
	.putp = str_putnext,
};

static struct qinit ptm_winit = {
	.putp = ptm_wput,
};

static struct streamtab ptm_streamtab = {
	.rinit = &ptm_rinit,
	.winit = &ptm_winit,
};

static struct qinit pts_rinit = {
	.qopen = pts_ropen,
	.qclose = pts_rclose,
	.putp = str_putnext,
};

static struct qinit pts_winit = {
	.putp = pts_wput,
};

static struct streamtab pts_streamtab = {
	.rinit = &pts_rinit,
	.winit = &pts_winit,
};

static dev_ops_t ptm_devops = {
	.streamtab = &ptm_streamtab,
};

static dev_ops_t pts_devops = {
	.streamtab = &pts_streamtab,
	.autopush = &ldterm_streamtab,
};

/*
 * If both master and slave are now closed, get rid of the PTY.
 */
static void
pty_closed(struct pty *pty)
{
	if (pty->master_open || pty->slave_open) {
		ke_mutex_exit(&pty->lock);
		return;
	}

	ke_mutex_exit(&pty->lock);

	ke_mutex_enter(&pty_lock, "pty_destroy");
	kassert(pty_table[pty->index] == pty);
	pty_table[pty->index] = NULL;
	ke_mutex_exit(&pty_lock);

	kdprintf("TODO pty close doesn't get rid of devfs node\n");

	kmem_free(pty, sizeof(*pty));
}

/*
 * master routines
 */

static int
ptm_ropen(queue_t *rq, void *dev)
{
	struct pty *pty;
	int index = -1;

	pty = kmem_alloc(sizeof(*pty));
	if (pty == NULL)
		return -ENOMEM;

	ke_mutex_init(&pty->lock);
	pty->locked = false; /* TODO: default locked - need mlibc sysdep */
	pty->master_open = true;
	pty->slave_open = false;
	pty->master_rq = rq;
	pty->slave_rq = NULL;
	pty->winsize.ws_row = 24;
	pty->winsize.ws_col = 80;
	pty->winsize.ws_xpixel = 640;
	pty->winsize.ws_ypixel = 480;

	ke_mutex_enter(&pty_lock, "ptm_ropen");
	for (int i = 0; i < PTY_MAX; i++) {
		if (pty_table[index] == NULL) {
			index = i;
			break;
		}
	}

	if (index == -1) {
		ke_mutex_exit(&pty_lock);
		kmem_free(pty, sizeof(*pty));
		return -ENOSPC;
	}

	pty->index = index;
	pty_table[index] = pty;
	ke_mutex_exit(&pty_lock);

	devfs_create_node(DEV_KIND_STREAM, &pts_devops, pty,
	    "pts%d", index);

	rq->ptr = rq->other->ptr = pty;

	return 0;
}

static void
ptm_rclose(queue_t *rq)
{
	struct pty *pty = rq->ptr;

	ke_mutex_enter(&pty->lock, "ptm_rclose");
	pty->master_open = false;
	pty->master_rq = NULL;

	if (pty->slave_open) {
		mblk_t *mp = str_allocb(0);
		if (mp != NULL) {
			mp->db->type = M_HANGUP;
			str_ingress_putq(pty->slave_rq->stdata, mp);
		}
	}

	pty_closed(pty); /* releases pty->lock */
}

static void
ptm_ioctl(struct pty *pty, queue_t *wq, mblk_t *mp)
{
	struct strioctl *ioc = (struct strioctl *)mp->rptr;

	switch (ioc->ic_cmd) {
	case TIOCGPTN: {
		int ptn = pty->index;
		memcpy(ioc->ic_dp, &ptn, sizeof(int));
		mp->db->type = M_IOCACK;
		ioc->rval = 0;
		str_qreply(wq, mp);
		break;
	}

	case TIOCSPTLCK: {
		int lock_val;
		memcpy(&lock_val, ioc->ic_dp, sizeof(int));

		ke_mutex_enter(&pty->lock, "TIOCSPTLCK");
		pty->locked = (lock_val != 0);
		ke_mutex_exit(&pty->lock);

		mp->db->type = M_IOCACK;
		ioc->rval = 0;
		str_qreply(wq, mp);
		break;
	}

	case TIOCGWINSZ:
		ke_mutex_enter(&pty->lock, "ptm TIOCGWINSZ");
		memcpy(ioc->ic_dp, &pty->winsize, sizeof(struct winsize));
		ke_mutex_exit(&pty->lock);

		mp->db->type = M_IOCACK;
		ioc->rval = 0;
		str_qreply(wq, mp);
		break;

	case TIOCSWINSZ:
		ke_mutex_enter(&pty->lock, "ptm TIOCSWINSZ");
		memcpy(&pty->winsize, ioc->ic_dp, sizeof(struct winsize));
		ke_mutex_exit(&pty->lock);

		mp->db->type = M_IOCACK;
		ioc->rval = 0;
		str_qreply(wq, mp);
		break;

	default:
		mp->db->type = M_IOCNAK;
		ioc->rval = ENOTSUP;
		str_qreply(wq, mp);
		break;
	}
}

static void
ptm_wput(queue_t *wq, mblk_t *mp)
{
	struct pty *pty = wq->ptr;

	switch (mp->db->type) {
	case M_DATA:
		ke_mutex_enter(&pty->lock, "ptm_wput data");
		if (pty->slave_open) {
			str_ingress_putq(pty->slave_rq->stdata, mp);
			ke_mutex_exit(&pty->lock);
		} else {
			ke_mutex_exit(&pty->lock);
			str_freemsg(mp);
		}
		break;

	case M_IOCTL:
		ptm_ioctl(pty, wq, mp);
		break;

	case M_FLUSH:
		/* TODO: handle M_FLUSH */
		str_freemsg(mp);
		break;

	default:
		str_freemsg(mp);
		break;
	}
}

/*
 * slave routines
 */

static int
pts_ropen(queue_t *rq, void *dev)
{
	struct pty *pty = dev;

	ke_mutex_enter(&pty->lock, "pts_ropen");

	if (!pty->master_open) {
		ke_mutex_exit(&pty->lock);
		return -ENXIO;
	}

	if (pty->locked) {
		ke_mutex_exit(&pty->lock);
		return -EACCES;
	}

	pty->slave_open = true;
	pty->slave_rq = rq;

	ke_mutex_exit(&pty->lock);

	rq->ptr = rq->other->ptr = pty;

	return 0;
}


static void
pts_rclose(queue_t *rq)
{
	struct pty *pty = rq->ptr;

	ke_mutex_enter(&pty->lock, "pts_rclose");
	pty->slave_open = false;
	pty->slave_rq = NULL;

	if (pty->master_open) {
		mblk_t *mp = str_allocb(0);
		if (mp != NULL) {
			mp->db->type = M_HANGUP;
			str_ingress_putq(pty->master_rq->stdata, mp);
		}
	}

	pty_closed(pty); /* releases pty->lock */
}

static void
pts_ioctl(struct pty *pty, queue_t *wq, mblk_t *mp)
{
	struct strioctl *ioc = (struct strioctl *)mp->rptr;

	switch (ioc->ic_cmd) {
	case TIOCGWINSZ:
		ke_mutex_enter(&pty->lock, "pts TIOCGWINSZ");
		memcpy(ioc->ic_dp, &pty->winsize, sizeof(struct winsize));
		ke_mutex_exit(&pty->lock);

		mp->db->type = M_IOCACK;
		ioc->rval = 0;
		str_qreply(wq, mp);
		break;

	case TIOCSWINSZ:
		ke_mutex_enter(&pty->lock, "pts TIOCSWINSZ");
		memcpy(&pty->winsize, ioc->ic_dp, sizeof(struct winsize));
		ke_mutex_exit(&pty->lock);

		mp->db->type = M_IOCACK;
		ioc->rval = 0;
		str_qreply(wq, mp);
		break;

	default:
		kdprintf("pts: warning, unhandled ioctl 0x%x\n", ioc->ic_cmd);
		break;
	}
}

static void
pts_wput(queue_t *wq, mblk_t *mp)
{
	struct pty *pty = wq->ptr;

	switch (mp->db->type) {
	case M_DATA:
		ke_mutex_enter(&pty->lock, "pts_wput data");
		if (pty->master_open) {
			str_ingress_putq(pty->master_rq->stdata, mp);
			ke_mutex_exit(&pty->lock);
		} else {
			ke_mutex_exit(&pty->lock);
			str_freemsg(mp);
		}
		break;

	case M_IOCTL:
		pts_ioctl(pty, wq, mp);
		break;

	case M_FLUSH:
		/* TODO: handle M_FLUSH */
		str_freemsg(mp);
		break;

	default:
		str_freemsg(mp);
		break;
	}
}

void
pty_init(void)
{
	memset(pty_table, 0, sizeof(pty_table));
	devfs_create_node(DEV_KIND_STREAM_CLONE, &ptm_devops, NULL, "ptmx");
}
