/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Mar 25 2023.
 */

#ifndef KRX_POSIX_TTY_H
#define KRX_POSIX_TTY_H

#include "abi-bits/termios.h"
#include "kdk/devmgr.h"
#include "executive/epoll.h"
#include "posix/pxp.h"

/*!
 * A teletype.
 *
 * (l) => #lock
 */
struct tty {
	struct device dev;

	kspinlock_t lock;    /*!< (~) tty lock */
	kevent_t read_evobj; /*<- (~)  data available for read */

	struct polllist polllist;

	struct termios termios; /*!< (l) terminal attributes */
	char buf[2048];		/*!< (l) input buffer */
	size_t buflen;		/*!< (l) input buffer current length */
	io_off_t readhead;	/*!< (l) input buffer read head */
	io_off_t writehead;	/*!< (l) input buffer write head */
	size_t nlines; /*!< (l) number lines available to read in buf */

	struct posix_pgroup * pg; /*!< (l) pgroup controlled */
};

#define TTYDEF_IFLAG (BRKINT | ICRNL | IMAXBEL | IXON | IXANY)
#define TTYDEF_OFLAG (OPOST | ONLCR)
#define TTYDEF_LFLAG (ECHO | ICANON | ISIG | IEXTEN | ECHOE | ECHOKE | ECHOCTL)
#define TTYDEF_CFLAG (CREAD | CS8 | HUPCL)
#define TTYDEF_SPEED (B38400)

void tty_input(struct tty *tty, int c);

#endif /* KRX_POSIX_TTY_H */
