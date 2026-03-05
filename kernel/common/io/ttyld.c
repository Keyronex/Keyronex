/*
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 * Created on Sat Dec 20 2025.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file ttyld.c
 * @brief TTY line discipline STREAMS module.
 *
 * FIXME: terribly nonoptimal and no OOM handling at all.
 */

#define _BSD_SOURCE

#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/termios.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/ttydefaults.h>

#define TABSIZE	  8
#define MAX_CANON 255

struct ldterm_state {
	struct termios termios; /* terminal settings */

	/* canonicalisation */
	char *linebuf;
	size_t linepos;
	size_t linesize;

	/* general state flags */
	bool stopped; /* output stopped (XOFF received) */
	int col;      /* current column (for tab/BS handling) */
};

static int ldterm_open(queue_t *rq, void *);
static void ldterm_rput(queue_t *rq, mblk_t *mp);
static void ldterm_wput(queue_t *rq, mblk_t *mp);

static struct qinit ldterm_rinit = {
	.qopen = ldterm_open,
	.putp = ldterm_rput,
};

static struct qinit ldterm_winit = {
	.putp = ldterm_wput,
};

struct streamtab ldterm_streamtab = {
	.rinit = &ldterm_rinit,
	.winit = &ldterm_winit,
};

static mblk_t *
get_readmode_mp(struct ldterm_state *ld)
{
	mblk_t *mp;
	struct stroptions *sop;

	mp = str_allocb(sizeof(struct stroptions));
	if (mp == NULL)
		return NULL;

	sop = (struct stroptions *)mp->rptr;

	mp->db->type = M_SETOPTS;
	sop->flags = SO_READMODE;
	sop->readopt = ld->termios.c_lflag & ICANON ? STR_RMSGN : STR_RNORM;

	return mp;
}

static int
ldterm_open(queue_t *rq, void *)
{
	struct ldterm_state *ld = kmem_alloc(sizeof(*ld));
	mblk_t *mp;

	ld->termios.c_iflag = TTYDEF_IFLAG;
	ld->termios.c_oflag = TTYDEF_OFLAG;
	ld->termios.c_lflag = TTYDEF_LFLAG;
	ld->termios.c_cflag = TTYDEF_CFLAG;
	ld->termios.c_cc[VINTR] = 0x03;	 /* ^C */
	ld->termios.c_cc[VQUIT] = 0x1C;	 /* ^\ */
	ld->termios.c_cc[VERASE] = '\b';
	ld->termios.c_cc[VKILL] = 0x15;	 /* ^U */
	ld->termios.c_cc[VEOF] = 0x04;	 /* ^D */
	ld->termios.c_cc[VSUSP] = 0x1A;	 /* ^Z */
	ld->termios.c_cc[VEOL] = '\r';

	ld->linebuf = kmem_alloc(MAX_CANON);
	ld->linesize = MAX_CANON;
	ld->linepos = 0;
	ld->stopped = false;
	ld->col = 0;

	rq->ptr = rq->other->ptr = ld;

	mp = get_readmode_mp(ld);
	if (mp == NULL) {
		kmem_free(ld->linebuf, MAX_CANON);
		kmem_free(ld, sizeof(*ld));
		return -ENOMEM;
	}

	str_putnext(rq, mp);

	return 0;
}

static void
ldterm_deliver_line(struct ldterm_state *ld, queue_t *rq, bool is_eof)
{
	mblk_t *mp;
	size_t len = ld->linepos;

	if (is_eof && len == 0) {
		/* zero-length message? */
		mp = str_allocb(0);
	} else {
		mp = str_allocb(len);
		memcpy(mp->wptr, ld->linebuf, len);
		mp->wptr += len;
	}

	ld->linepos = 0;
	str_putnext(rq, mp);
}

static void
ldterm_flush(struct ldterm_state *ld)
{
	ld->linepos = 0;
}


static void
ldterm_echo_outstr(queue_t *wq, const char *s, size_t len)
{
	mblk_t *mp;

	if (len == 0)
		return;

	mp = str_allocb(len);
	if (mp == NULL)
		return;

	memcpy(mp->wptr, s, len);
	mp->wptr += len;

	str_putnext(wq, mp);
}

static void
ldterm_echo_outch(queue_t *wq, unsigned char c)
{
	ldterm_echo_outstr(wq, (const char *)&c, 1);
}

static void
ldterm_echo(struct ldterm_state *ld, queue_t *wq, unsigned char c)
{
	if (c == '\n') {
		ldterm_echo_outch(wq, '\n');
		ld->col = 0;
		return;
	}

	if (c == '\r') {
		ldterm_echo_outch(wq, '\r');
		ld->col = 0;
		return;
	}

	if (c == '\t') {
		if (ld->termios.c_oflag & XTABS) {
			int nspaces = TABSIZE - (ld->col % TABSIZE);
			for (int i = 0; i < nspaces; i++)
				ldterm_echo_outch(wq, ' ');
			ld->col += nspaces;
		} else {
			ldterm_echo_outch(wq, '\t');
			ld->col = (ld->col + TABSIZE) & ~(TABSIZE - 1);
		}
		return;
	}

	if (c == '\b') {
		if (ld->col > 0) {
			ldterm_echo_outch(wq, '\b');
			ld->col--;
		}
		return;
	}

	if ((c < 0x20 || c == 0x7F) && (ld->termios.c_lflag & ECHOCTL)) {
		ldterm_echo_outch(wq, '^');
		ldterm_echo_outch(wq, (c == 0x7F) ? '?' : (c + '@'));
		ld->col += 2;
		return;
	}

	if (c >= 0x20 && c < 0x7F) {
		ldterm_echo_outch(wq, c);
		ld->col++;
		return;
	}

	ldterm_echo_outch(wq, c);
	ld->col++;
}

static void
ldterm_echo_erase(struct ldterm_state *ld, queue_t *wq,
    unsigned char erased_char)
{
	int width;

	if (erased_char == '\t') {
		int new_col = 0;

		for (size_t i = 0; i < ld->linepos; i++) {
			unsigned char ch = ld->linebuf[i];
			if (ch == '\t')
				new_col = (new_col + TABSIZE) & ~(TABSIZE - 1);
			else if (ch < 0x20 || ch == 0x7F)
				new_col += 2; /* ^X */
			else
				new_col++;
		}

		while (ld->col > new_col) {
			ldterm_echo_outstr(wq, "\b \b", 3);
			ld->col--;
		}

		return;
	}

	if (erased_char < 0x20 || erased_char == 0x7F)
		width = (ld->termios.c_lflag & ECHOCTL) ? 2 : 0;
	else
		width = 1;

	for (int i = 0; i < width; i++) {
		ldterm_echo_outstr(wq, "\b \b", 3);
		ld->col--;
	}
}

static void
ldterm_echo_kill(struct ldterm_state *ld, queue_t *wq)
{
	if (ld->termios.c_lflag & ECHOKE) {
		while (ld->linepos > 0) {
			unsigned char erased = ld->linebuf[--ld->linepos];
			ldterm_echo_erase(ld, wq, erased);
		}
	} else if (ld->termios.c_lflag & ECHOK) {
		if (ld->termios.c_lflag & ECHO) {
			unsigned char killc = ld->termios.c_cc[VKILL];
			ldterm_echo(ld, wq, killc);
		}
		ldterm_echo_outch(wq, '\n');
		ld->col = 0;
		ld->linepos = 0;
	} else {
		if (ld->termios.c_lflag & ECHO) {
			unsigned char killc = ld->termios.c_cc[VKILL];
			ldterm_echo(ld, wq, killc);
		}
		ld->linepos = 0;
	}
}

static void
ldterm_canon_input(struct ldterm_state *ld, queue_t *rq, unsigned char c)
{
	if (c == ld->termios.c_cc[VERASE]) {
		if (ld->linepos > 0) {
			unsigned char erased = ld->linebuf[--ld->linepos];
			if (ld->termios.c_lflag & ECHO)
				ldterm_echo_erase(ld, rq->other, erased);
		}
		return;
	}

	if (c == ld->termios.c_cc[VKILL]) {
		if (ld->termios.c_lflag & ECHO)
			ldterm_echo_kill(ld, rq->other);
		else
			ld->linepos = 0;
		return;
	}

	if (c == ld->termios.c_cc[VEOF]) {
		ldterm_deliver_line(ld, rq, true);
		return;
	}

	if (ld->termios.c_lflag & ECHO)
		ldterm_echo(ld, rq->other, c);

	if (ld->linepos < ld->linesize - 1)
		ld->linebuf[ld->linepos++] = c;

	if (c == '\n' || c == ld->termios.c_cc[VEOL]) {
		ldterm_deliver_line(ld, rq, false);
	}
}

static void
ldterm_raw_input(struct ldterm_state *ld, queue_t *rq, unsigned char c)
{
	mblk_t *mp;

	if (ld->termios.c_lflag & ECHO)
		ldterm_echo(ld, rq->other, c);

	mp = str_allocb(1);
	*mp->wptr++ = c;
	str_putnext(rq, mp);
}

static void
ldterm_signal(struct ldterm_state *ld, int sig)
{
	kdprintf(" ==> ldterm: signal %d generated\n", sig);
}

static void
ldterm_rput(queue_t *rq, mblk_t *mp)
{
	struct ldterm_state *ld = rq->ptr;

	if (mp->db->type != M_DATA) {
		str_putnext(rq, mp);
		return;
	}

	while (mp->rptr < mp->wptr) {
		unsigned char c = *mp->rptr++;

		if (c == '\r') {
			if (ld->termios.c_iflag & IGNCR)
				continue;	/* ignore CR */
			if (ld->termios.c_iflag & ICRNL)
				c = '\n';	/* CR -> NL */
		} else if (c == '\n') {
			if (ld->termios.c_iflag & INLCR)
				c = '\r';	/* NL -> CR */
		}

		if (ld->termios.c_lflag & ISIG) {
			if (c == ld->termios.c_cc[VINTR]) {
				ldterm_signal(ld, SIGINT);
				if (!(ld->termios.c_lflag & NOFLSH))
					ldterm_flush(ld);
				continue;
			}
			if (c == ld->termios.c_cc[VQUIT]) {
				ldterm_signal(ld, SIGQUIT);
				continue;
			}
			if (c == ld->termios.c_cc[VSUSP]) {
				ldterm_signal(ld, SIGTSTP);
				continue;
			}
		}

		if (ld->termios.c_lflag & ICANON)
			ldterm_canon_input(ld, rq, c);
		else
			ldterm_raw_input(ld, rq, c);
	}

	str_freeb(mp);
}

static void
ldterm_output_process(struct ldterm_state *ld, queue_t *wq, mblk_t *mp)
{
	size_t extra = 0;
	mblk_t *newmp;

	if (ld->termios.c_oflag & ONLCR) {
		for (char *p = mp->rptr; p < mp->wptr; p++)
			if (*p == '\n')
				extra++;
	}

	if (extra == 0) {
		str_putnext(wq, mp);
		return;
	}

	newmp = str_allocb((mp->wptr - mp->rptr) + extra);
	for (char *p = mp->rptr; p < mp->wptr; p++) {
		if (*p == '\n' && (ld->termios.c_oflag & ONLCR))
			*newmp->wptr++ = '\r';
		*newmp->wptr++ = *p;
	}

	str_freeb(mp);
	str_putnext(wq, newmp);
}

static void
ldterm_ioctl(struct ldterm_state *ld, queue_t *wq, mblk_t *mp)
{
	struct strioctl *ioc = (struct strioctl *)mp->rptr;
	tcflag_t old_lflag;

	switch (ioc->ic_cmd) {
	case TCGETS:
		memcpy(ioc->ic_dp, &ld->termios, sizeof(struct termios));
		mp->db->type = M_IOCACK;
		str_putnext(wq->other, mp);
		break;

	case TCSETS:
	case TCSETSW:
	case TCSETSF:
		old_lflag = ld->termios.c_lflag;
		memcpy(&ld->termios, ioc->ic_dp, sizeof(struct termios));

		if (ioc->ic_cmd == TCSETSF)
			ldterm_flush(ld);

		if ((old_lflag ^ ld->termios.c_lflag) & ICANON) {
			mblk_t *omp = get_readmode_mp(ld);
			str_putnext(wq->other, omp);
		}

		mp->db->type = M_IOCACK;
		str_putnext(wq->other, mp);
		break;

	default:
		str_putnext(wq, mp);
	}
}

static void
ldterm_wput(queue_t *wq, mblk_t *mp)
{
	struct ldterm_state *ld = wq->ptr;

	switch (mp->db->type) {
	case M_IOCTL:
		ldterm_ioctl(ld, wq, mp);
		return;

	case M_DATA:
		if (ld->termios.c_oflag & OPOST)
			ldterm_output_process(ld, wq, mp);
		else
			str_putnext(wq, mp);
		return;

	default:
		str_putnext(wq, mp);
		return;
	}
}
