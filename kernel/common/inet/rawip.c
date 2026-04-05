/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 30 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file raw.c
 * @brief Raw internet sockets.
 */


#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/tihdr.h>

static int rawip_ipv4_ropen(queue_t *, void *devp);
static int rawip_ipv6_ropen(queue_t *, void *devp);
static void rawip_wput(queue_t *, mblk_t *);

struct qinit rawip_ipv4_rinit = {
	.qopen = rawip_ipv4_ropen,
};

struct qinit rawip_ipv6_rinit = {
	.qopen = rawip_ipv6_ropen,
};

struct qinit rawip_winit = {
	.putp = rawip_wput,
};

struct streamtab raw_ipv4_streamtab = {
	.rinit = &rawip_ipv4_rinit,
	.winit = &rawip_winit,
};

struct streamtab raw_ipv6_streamtab = {
	.rinit = &rawip_ipv6_rinit,
	.winit = &rawip_winit,
};

typedef struct rawip {
	kspinlock_t lock;
} rawip_t;

static int
rawip_ipv4_ropen(queue_t *rq, void *devp)
{
	return 0;
}

static int
rawip_ipv6_ropen(queue_t *rq, void *devp)
{
	return 0;
}

static void
rawip_wput(queue_t *wq, mblk_t *mp)
{
	ktodo();
}
