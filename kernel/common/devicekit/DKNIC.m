/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Thu Jan 08 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file DKNIC.m
 * @brief Generic NIC device base class.
 */

#include <sys/dlpi.h>
#include <sys/stream.h>

#include <devicekit/DKNIC.h>
#include <fs/devfs/devfs.h>

static int nic_open(queue_t *, void *);
static void nic_close(queue_t *);
static void nic_wput(queue_t *, mblk_t *);

static struct qinit nic_rinit = {
	.qopen = nic_open,
	.qclose = nic_close,
};

static struct qinit nic_winit = {
	.putp = nic_wput,
};

static struct streamtab nic_streamtab = {
	.rinit = &nic_rinit,
	.winit = &nic_winit,
};

static dev_ops_t dknic_devops = { .streamtab = &nic_streamtab };

static unsigned int counter = 0;

@implementation DKNIC

- (void)setupNIC
{
	devfs_create_node(DEV_KIND_STREAM_CLONE, &dknic_devops, self, "net%u",
	    counter++);
}

- (void)didReceivePacket:(mblk_t *)mp
{
	kfatal("didReceivePacket\n");
}

- (void)transmitPacket:(mblk_t *)mp
{
	kfatal("transmitPacket: subclass responsibility");
}

@end

static int
nic_open(queue_t *, void *)
{
	ktodo();
}

static void
nic_close(queue_t *)
{
}

static void
nic_wput(queue_t *, mblk_t *)
{
	ktodo();
}
