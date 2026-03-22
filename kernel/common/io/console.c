/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file console.c
 * @brief System console driver.
 */

#include <sys/libkern.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/termios.h>

#include <fs/devfs/devfs.h>

extern struct streamtab ldterm_streamtab;

static stdata_t *console_stdata;
static dev_ops_t console_ops;

void
console_init(void)
{
	devfs_create_node(DEV_KIND_STREAM, &console_ops, NULL, "console");
}

void
console_input(const char *buf, int count)
{
	mblk_t *mp;

	kassert(ke_ipl() == IPL_DISP);

	mp = str_allocb(count);
	if (mp == NULL)
		return;

	memcpy(mp->wptr, buf, count);
	mp->wptr += count;

	ke_spinlock_enter_nospl(&console_stdata->ingress_lock);
	TAILQ_INSERT_TAIL(&console_stdata->ingress_head, mp, link);
	ke_spinlock_exit_nospl(&console_stdata->ingress_lock);
	str_kick(console_stdata);
}


static int
console_ropen(queue_t *rq, void *)
{
	console_stdata = rq->stdata;
	return 0;
}

static void
console_rclose(queue_t *rq)
{
	console_stdata = NULL;
}

static void
console_rput(queue_t *rq, mblk_t *mp)
{
	str_putnext(rq, mp);
}

static void
console_ioctl(queue_t *wq, mblk_t *mp)
{
	struct strioctl *ioc = (struct strioctl *)mp->rptr;

	switch (ioc->ic_cmd) {
	case TIOCGWINSZ: {
		struct winsize ws;

		ws.ws_row = 25;
		ws.ws_col = 80;
		ws.ws_xpixel = 0;
		ws.ws_ypixel = 0;

		memcpy(ioc->ic_dp, &ws, sizeof(struct winsize));
		mp->db->type = M_IOCACK;
		ioc->rval = 0;
		str_putnext(wq->other, mp);
		break;
	}

	case TIOCSWINSZ:
		mp->db->type = M_IOCACK;
		ioc->rval = 0;
		str_putnext(wq->other, mp);
		break;

	default:
		kdprintf("console_wput: unknown ioctl, type=0x%x\n",
		    ioc->ic_cmd);
		str_freemsg(mp);
	}
}

static void
console_wput(queue_t *wq, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_DATA:
		for (mblk_t *bp = mp; bp != NULL; bp = bp->cont)
			kdputn(bp->rptr, bp->wptr - bp->rptr);
		str_freemsg(mp);
		break;

	case M_IOCTL:
		return console_ioctl(wq, mp);

	default:
		kdprintf("console_wput: non-data message received, type=%d\n",
		    mp->db->type);
		str_freemsg(mp);
	}
}

static struct qinit console_rinit = {
	.qopen = console_ropen,
	.qclose = console_rclose,
	.putp = console_rput,
};

static struct qinit console_winit = {
	.putp = console_wput,
};

static struct streamtab console_streamtab = {
	.rinit = &console_rinit,
	.winit = &console_winit,
};

static struct dev_ops console_ops = {
	.streamtab = &console_streamtab,
	.autopush = &ldterm_streamtab,
};
