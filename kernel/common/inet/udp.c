/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 30 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file udp.c
 * @brief User datagram protocol implementation.
 */

#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/tihdr.h>

#include <inet/ip.h>

static int udp_ipv4_ropen(queue_t *, void *devp);
static int udp_ipv6_ropen(queue_t *, void *devp);
static void udp_wput(queue_t *, mblk_t *);

struct qinit udp_ipv4_rinit = {
	.qopen = udp_ipv4_ropen,
};

struct qinit udp_ipv6_rinit = {
	.qopen = udp_ipv6_ropen,
};

struct qinit udp_winit = {
	.putp = udp_wput,
};

struct streamtab udp_ipv4_streamtab = {
	.rinit = &udp_ipv4_rinit,
	.winit = &udp_winit,
};

struct streamtab udp_ipv6_streamtab = {
	.rinit = &udp_ipv6_rinit,
	.winit = &udp_winit,
};

typedef struct udp {
	kspinlock_t lock;
} udp_t;

static int
udp_ipv4_ropen(queue_t *rq, void *devp)
{
	return 0;
}

static int
udp_ipv6_ropen(queue_t *rq, void *devp)
{
	return 0;
}

static void
udp_wput(queue_t *wq, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_IOCTL: {
		struct strioctl *ioc = (typeof(ioc))mp->rptr;

		switch (ioc->ic_cmd) {
		case SIOCGIFMTU:
			return ip_uwput_ioctl_sgif(wq, mp);
		}

		break;
	}

	default:
		ktodo();
	}
}
