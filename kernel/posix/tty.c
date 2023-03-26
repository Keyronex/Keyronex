/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Mar 25 2023.
 */

#include "kdk/kernel.h"
#include "kdk/vfs.h"
#include "posix/tty.h"

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
#if 0
	knote_t *knote;
#endif

	if (tty->buflen == sizeof(tty->buf))
		return -1;

	if (c == '\n')
		tty->nlines++;

	tty->buf[tty->writehead++] = c;
	if (tty->writehead == sizeof(tty->buf))
		tty->writehead = 0;
	tty->buflen++;

#if 0
	SLIST_FOREACH(knote, &tty->knotes, list)
	{
		knote->status = 1;
		knote_notify(knote);
	}
#endif

	if (!tty_iscanon(tty) || ('c' == '\n'))
		ke_event_signal(&tty->read_evobj);

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

	if (c == '\n' || c == tty->termios.c_cc[VEOL])
		tty->nlines--;

	tty->buflen--;
	return c;
}

void
tty_input(struct tty *tty, int c)
{
	/* signals */
	if (tty_isisig(tty)) {
		if (c == tty->termios.c_cc[VINTR]) {
			kdprintf("VINTR on tty %p\n", tty);
			return;
		} else if (c == tty->termios.c_cc[VQUIT]) {
			kdprintf("VQUIT on tty %p\n", tty);
			return;
		} else if (c == tty->termios.c_cc[VSUSP]) {
			kdprintf("VSUSP on tty %p\n", tty);
			return;
		}
	}

	/* newline */
	if (c == '\r') {
		if (ISSET(tty->termios.c_iflag, IGNCR))
			return;
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
			return;
		}

		/* erase word ^W */
		if (c == tty->termios.c_cc[VWERASE] &&
		    ISSET(tty->termios.c_lflag, IEXTEN)) {
			kdprintf("VWERASE on tty %p\n", tty);
			return;
		}
	}

	if (tty->termios.c_lflag & ECHO /* and is the code printable? */) {
		hl_dputc(c, NULL);
		// tty->putch(tty->data, c);
	}

	enqueue(tty, c);
}

int
tty_read(vnode_t *vn, void *buf, size_t nbyte, io_off_t off)
{
	struct tty *tty = (struct tty *)vn->rdevice;
	ipl_t ipl;
	size_t nread = 0;

	(void)off;

in:
	ipl = ke_spinlock_acquire(&tty->lock);

	if ((tty_iscanon(tty) && tty->nlines == 0) || (tty->buflen == 0)) {
		ke_spinlock_release(&tty->lock, ipl);
		ke_wait(&tty->read_evobj, "tty_read:read_event", false, false,
		    -1);
		goto in;
	}

	while (nread < nbyte) {
		int c = dequeue(tty);
		((char *)buf)[nread++] = c;
		if (c == '\n' || c == tty->termios.c_cc[VEOL])
			break;
	}

	return nread;
}

int
tty_write(struct vnode *vn, void *buf, size_t nbyte, io_off_t off)
{
	ipl_t ipl;

	(void)off;

	ipl = ke_spinlock_acquire(&dprintf_lock);
	for (unsigned i = 0; i < nbyte; i++) {
		int c = ((char *)buf)[i];
		hl_dputc(c, NULL);
	}
	ke_spinlock_release(&dprintf_lock, ipl);

	return nbyte;
}

struct vnops tty_vnops = {
	.read = tty_read,
	.write = tty_write,
};