/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file stropts.h
 * @brief STREAMS opts
 */

#ifndef ECX_SYS_STROPTS_H
#define ECX_SYS_STROPTS_H

struct strioctl {
	int cmd;
	int len;
	void *data;
	int rval;
};

enum str_option_flags {
	SO_READMODE = 0x01, /* set read mode */
};

enum str_read_mode {
	STR_RNORM, /* Byte stream (noncanon TTY, pipes, fifos, SOCK_STREAM) */
	STR_RMSGD, /* Read one message, discard remainder (SOCK_DGRAM) */
	STR_RMSGN, /* Read one message, leave remainder (canon TTY) */
};

struct stroptions {
	enum str_option_flags flags;	/* option flags */
	enum str_read_mode readopt;	/* read mode option */
};


#endif /* ECX_SYS_STROPTS_H */
