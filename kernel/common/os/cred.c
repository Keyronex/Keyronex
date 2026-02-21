/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file cred.c
 * @brief Credentials
 */

#include <sys/krx_cred.h>

int
sys_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
	*ruid = 0;
	*euid = 0;
	*suid = 0;
	return 0;
}

int
sys_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
	*rgid = 0;
	*egid = 0;
	*sgid = 0;
	return 0;
}
