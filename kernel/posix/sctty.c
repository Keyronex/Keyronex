/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Mar 26 2023.
 */

#include <sys/termios.h>

#include "kdk/vfs.h"
#include "tty.h"

int devfs_create(struct device *dev, const char *name, struct vnops *devvnops);

extern struct vnops tty_vnops;
static struct tty sctty;

int
pxp_make_syscon_tty(void)
{
	ke_spinlock_init(&sctty.polllist.lock);
	LIST_INIT(&sctty.polllist.pollhead_list);

	sctty.termios.c_cc[VINTR] = 0x03;
	sctty.termios.c_cc[VEOL] = '\n';
	sctty.termios.c_cc[VEOF] = '\0';
	sctty.termios.c_cflag = TTYDEF_CFLAG;
	sctty.termios.c_iflag = TTYDEF_IFLAG;
	sctty.termios.c_lflag = TTYDEF_LFLAG;
	sctty.termios.c_oflag = TTYDEF_OFLAG;
	sctty.termios.ibaud = sctty.termios.obaud = TTYDEF_SPEED;

	ke_spinlock_init(&sctty.lock);
	ke_event_init(&sctty.read_evobj, false);

	sctty.buflen = 0;
	sctty.readhead = 0;
	sctty.writehead = 0;
	sctty.nlines = 0;

	return devfs_create(&sctty.dev, "console", &tty_vnops);
}

void
syscon_instr(const char *str)
{
	while (*str != '\0')
		tty_input(&sctty, *str++);
}

void
syscon_inchar(char c)
{
	tty_input(&sctty, c);
}