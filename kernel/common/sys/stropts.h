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

#endif /* ECX_SYS_STROPTS_H */
