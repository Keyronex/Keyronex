/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file krx_signal.h
 * @brief Signals
 */

#ifndef ECX_SYS_KRX_SIGNAL_H
#define ECX_SYS_KRX_SIGNAL_H

#include <sys/signal.h>

int sys_sigaction(int signum, const struct sigaction *act,
    struct sigaction *oldact);
int sys_sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

#endif /* ECX_SYS_KRX_SIGNAL_H */
