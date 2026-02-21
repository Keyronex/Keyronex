/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file krx_cred.h
 * @brief Credentials
 */

#ifndef ECX_SYS_KRX_CRED_H
#define ECX_SYS_KRX_CRED_H

#include <sys/types.h>

int sys_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
int sys_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);

#endif /* ECX_SYS_KRX_CRED_H */
