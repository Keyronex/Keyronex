/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Mar 25 2023.
 */

#include <sys/errno.h>

#include <asm/ioctls.h>

#include <termios.h>

#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/vfs.h"
#include "posix/tty.h"
#include "pxp.h"

#define ISSET(FIELD, VAL) ((FIELD)&VAL)

static bool
tty_iscanon(struct tty *tty)
{
	return tty->termios.c_lflag & ICANON;
}

static bool
tty_isisig(struct tty *tty)
{
	return tty->termios.c_lflag & ISIG;
}

static int
enqueue(struct tty *tty, int c)
{
	if (tty->buflen == sizeof(tty->buf))
		return -1;

	if (c == '\n')
		tty->nlines++;

	tty->buf[tty->writehead++] = c;
	if (tty->writehead == sizeof(tty->buf))
		tty->writehead = 0;
	tty->buflen++;

	struct pollhead *ph;
	LIST_FOREACH (ph, &tty->polllist.pollhead_list, polllist_entry) {
		pollhead_raise(ph, EPOLLIN);
	}

	if (!tty_iscanon(tty) || (c == '\n')) {
		ke_event_signal(&tty->read_evobj);
	}

	return 0;
}

static int
unenqueue(struct tty *tty)
{
	io_off_t prevwritehead;
	int prevc;

	if (tty->buflen == 0)
		return -1;

	prevwritehead = tty->writehead == 0 ? elementsof(tty->buf) - 1 :
					      tty->writehead - 1;

	prevc = tty->buf[prevwritehead];

	/* no erasure after newline */
	if (tty->buf[prevwritehead] == tty->termios.c_cc[VEOL] ||
	    tty->buf[prevwritehead == '\n'])
		return '\0';

	tty->writehead = prevwritehead;
	tty->buflen--;
	return prevc;
}

static int
dequeue(struct tty *tty)
{
	int c;

	if (tty->buflen == 0)
		return '\0';

	c = tty->buf[tty->readhead++];
	if (tty->readhead == sizeof(tty->buf))
		tty->readhead = 0;

	if (c == '\n' || c == tty->termios.c_cc[VEOL]) {
		tty->nlines--;
		if (tty->nlines == 0 && tty_iscanon(tty))
			ke_event_clear(&tty->read_evobj);
	}

	tty->buflen--;
	if (tty->buflen == 0)
		ke_event_clear(&tty->read_evobj);

	return c;
}

void
tty_input(struct tty *tty, int c)
{
	ipl_t ipl = ke_spinlock_acquire(&tty->lock);
	int sig = -1;

	/* signals */
	if (tty_isisig(tty)) {
		if (c == tty->termios.c_cc[VINTR]) {
			sig = SIGINT;
			goto out;
		} else if (c == tty->termios.c_cc[VQUIT]) {
			kdprintf("VQUIT on tty %p\n", tty);
			goto out;
		} else if (c == tty->termios.c_cc[VSUSP]) {
			kdprintf("VSUSP on tty %p\n", tty);
			goto out;
		}
	}

	/* newline translation */
	if (c == '\r') {
		if (ISSET(tty->termios.c_iflag, IGNCR))
			goto out;
		else if (ISSET(tty->termios.c_iflag, ICRNL))
			c = '\n';
	} else if (c == '\n' && ISSET(tty->termios.c_iflag, INLCR))
		c = '\r';

	if (tty_iscanon(tty)) {
		/* erase ^h/^? */
		if (c == tty->termios.c_cc[VERASE]) {
			unenqueue(tty);
			hl_dputc('\b', NULL);
			hl_dputc(' ', NULL);
			hl_dputc('\b', NULL);
			goto out;
		}

		/* erase word ^W */
		if (c == tty->termios.c_cc[VWERASE] &&
		    ISSET(tty->termios.c_lflag, IEXTEN)) {
			kdprintf("VWERASE on tty %p\n", tty);
			goto out;
		}
	}

	if (tty->termios.c_lflag & ECHO /* and is the code printable? */) {
		hl_dputc(c, NULL);
		// tty->putch(tty->data, c);
	}

	enqueue(tty, c);

out:
	ke_spinlock_release(&tty->lock, ipl);

	if (sig != -1) {
		struct posix_pgroup *pg;
		posix_proc_t *proc;

		ipl = px_acquire_proctree_mutex();
		pg = tty->pg;
		psx_signal_pgroup(pg, sig);
		px_release_proctree_mutex(ipl);
	}
}

int
tty_read(vnode_t *vn, void *buf, size_t nbyte, io_off_t off)
{
	struct tty *tty = (struct tty *)vn->rdevice;
	ipl_t ipl;
	size_t nread = 0;
	kwaitstatus_t w;

	(void)off;

in:
	ipl = ke_spinlock_acquire(&tty->lock);

	if ((tty_iscanon(tty) && tty->nlines == 0) || (tty->buflen == 0)) {
		ke_spinlock_release(&tty->lock, ipl);
		w = ke_wait(&tty->read_evobj, "tty_read:read_event", false,
		    false, -1);
		if (w == kKernWaitStatusSignalled)
			return -EINTR;
		kassert(w == kKernWaitStatusOK);
		goto in;
	}

	while (nread < nbyte) {
		int c = dequeue(tty);
		/* todo: this is illegal at this IPL */
		((char *)buf)[nread++] = c;
		if (tty_iscanon(tty) && c == tty->termios.c_cc[VEOL]) {
			break;
		}
	}

	ke_spinlock_release(&tty->lock, ipl);

	return nread;
}

int
tty_write(struct vnode *vn, void *buf, size_t nbyte, io_off_t off)
{
	ipl_t ipl;

	(void)off;

	char *mybuf = kmem_alloc(nbyte);
	memcpy(mybuf, buf, nbyte);

	ipl = ke_spinlock_acquire(&dprintf_lock);
	for (unsigned i = 0; i < nbyte; i++) {
		int c = ((char *)mybuf)[i];
		hl_scputc(c, NULL);
	}
	ke_spinlock_release(&dprintf_lock, ipl);

	kmem_free(mybuf, nbyte);

	return nbyte;
}

static int
tty_chpoll(vnode_t *vn, struct pollhead *ph, enum chpoll_kind kind)
{
	struct tty *tty = (struct tty *)vn->rdevice;
	ipl_t ipl = ke_spinlock_acquire(&tty->lock);
	int r = 0;

	if (kind == kChPollAdd) {
		if (tty->buflen > 0) {
			ph->revents = EPOLLIN;
			r = 1;
		} else
			LIST_INSERT_HEAD(&tty->polllist.pollhead_list, ph,
			    polllist_entry);
	} else if (kind == kChPollChange) {
		kfatal("Unimplemented");
	} else if (kind == kChPollRemove) {
		LIST_REMOVE(ph, polllist_entry);
	}

	ke_spinlock_release(&tty->lock, ipl);

	return r;
}

static int
tty_ioctl(vnode_t *vn, unsigned long command, void *data)
{
	struct tty *tty = (struct tty *)vn->rdevice;
	ipl_t ipl;
	int r = 0;

	switch (command) {
	case TCGETS: {
		struct termios tmp;

		ipl = ke_spinlock_acquire(&tty->lock);
		tmp = tty->termios;
		ke_spinlock_release(&tty->lock, ipl);

		*(struct termios *)data = tmp;
		break;
	}

	case TCSETS:
	case TCSETSF:
	case TCSETSW: {
		struct termios tmp = *(struct termios *)data;

		ipl = ke_spinlock_acquire(&tty->lock);
		tty->termios = tmp;
		ke_spinlock_release(&tty->lock, ipl);

		/* todo: adjust event if canonicity changed */
		break;

	case TIOCGWINSZ: {
		struct winsize *ws = data;

		/* todo don't hardcode syscon */
		syscon_getsize(ws);

		break;
	}

	case TIOCSWINSZ: {
		/* todo, do we allow it?*/

		/* epsilon*/
		break;
	}

	case TIOCGPGRP:
		/* todo lock*/
		// if (!tty_controls_proc(tty, px_curproc()))
		//	return -ENOTTY;
		*(int *)data = tty->pg != NULL ? tty->pg->pgid : -1;
		break;

	case TIOCSPGRP: {
		struct posix_pgroup *pgrp;

		// if (!tty_controls_proc(tty, px_curproc()))
		//	return -ENOTTY;

		pgrp = psx_lookup_pgid(*(int *)data);

		if (pgrp != NULL)
			tty->pg = pgrp;
		else
			r = -ESRCH;

		break;
	}

	default:
		kfatal("Unhandled ioctl.\n");
	}
	}

	return r;
}

struct vnops tty_vnops = {
	.ioctl = tty_ioctl,
	.read = tty_read,
	.write = tty_write,
	.chpoll = tty_chpoll,
};