/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file signal.c
 * @brief Signals
 */

#include <sys/krx_signal.h>
#include <sys/errno.h>

int
sys_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	return -ENOSYS;
}

int
sys_sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
	return -ENOSYS;
}
