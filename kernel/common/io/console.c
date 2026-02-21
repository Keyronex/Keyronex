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
#include <sys/termios.h>

#include <fs/devfs/devfs.h>

static struct dev_class console_class;
static queue_t *console_rq;

void
console_init(void)
{
	devfs_create_node(&console_class, NULL, "console");
}

static int
console_ropen(queue_t *rq, void *)
{
	console_rq = rq;
	return 0;
}

static void
console_rclose(queue_t *rq)
{
	console_rq = NULL;
}

static void
console_ioctl(queue_t *wq, mblk_t *mp)
{
	struct strioctl *ioc = (struct strioctl *)mp->rptr;

	switch (ioc->cmd) {
	case TIOCGWINSZ: {
		struct winsize ws;

		ws.ws_row = 25;
		ws.ws_col = 80;
		ws.ws_xpixel = 0;
		ws.ws_ypixel = 0;

		memcpy(ioc->data, &ws, sizeof(struct winsize));
		mp->db->type = M_IOCACK;
		str_putnext(wq->other, mp);
		break;
	}

	default:
		kdprintf("console_wput: unknown ioctl received, type=%d\n",
		    ioc->cmd);
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
};

static struct qinit console_winit = {
	.putp = console_wput,
};

static struct streamtab console_streamtab = {
	.rinit = &console_rinit,
	.winit = &console_winit,
};

static struct dev_class console_class = {
	.kind = DEV_KIND_STREAM,
	.streamtab = &console_streamtab
};
