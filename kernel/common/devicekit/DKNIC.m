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
#include <sys/strsubr.h>
#include <sys/libkern.h>

#include <devicekit/DKNIC.h>
#include <fs/devfs/devfs.h>

static int nic_open(queue_t *, void *);
static void nic_close(queue_t *);
static void nic_rput(queue_t *, mblk_t *);
static void nic_wput(queue_t *, mblk_t *);

static struct qinit nic_rinit = {
	.qopen = nic_open,
	.qclose = nic_close,
	.putp = nic_rput,
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
	m_put = NULL;
	m_data = NULL;
	devfs_create_node(DEV_KIND_STREAM_CLONE, &dknic_devops, self, "net%u",
	    counter++);
}

- (void)didReceivePacket:(mblk_t *)mp
{
	if (m_put == NULL)
		str_freemsg(mp);
	else
		m_put(m_data, mp);
}

- (void)transmitPacket:(mblk_t *)mp
{
	kfatal("transmitPacket: subclass responsibility");
}

#if 0
- (void)wput:(queue_t *)wq bindReq:(mblk_t *)mp
{
	mblk_t *bamp;
	dl_bind_req_t *br = (typeof(br))mp->rptr;
	dl_bind_ack_t *ba;

	bamp = str_allocb(sizeof(dl_bind_req_t) + ETH_ALEN);
	bamp->db->type = M_PROTO;
	ba = (typeof(ba))bamp->wptr;
	ba->dl_primitive = DL_BIND_ACK;
	ba->dl_sap = br->dl_sap;
	ba->dl_addr_length = ETH_ALEN;
	ba->dl_addr_offset = sizeof(dl_bind_req_t);
	ba->dl_max_conind = 0;
	ba->dl_xidtest_flg = 0;
	memcpy(bamp->wptr + ba->dl_addr_offset, self->m_mac_address, ETH_ALEN);
	bamp->wptr += sizeof(dl_bind_req_t) + ETH_ALEN;

	str_freeb(mp);
	str_qreply(wq, bamp);
}
#endif

static int
nic_wput_data(void *data, mblk_t *mp)
{
	DKNIC *self = data;
	[self transmitPacket:mp];
	return 0;
}

- (void)wput:(queue_t *)wq keyronexBindReq:(mblk_t *)mp
{
	mblk_t *bamp;
	dl_keyronex_bind_req_t *br = (typeof(br))mp->rptr;
	dl_keyronex_bind_ack_t *ba;

	bamp = str_allocb(sizeof(dl_keyronex_bind_ack_t));
	bamp->db->type = M_PROTO;
	ba = (typeof(ba))bamp->wptr;
	ba->dl_primitive = DL_KEYRONEX_BIND_ACK;
	ba->pdata = &m_data;
	ba->pput = &m_put;
	ba->nic_data = (void*)self;
	ba->nic_wput = nic_wput_data;

	memcpy(&ba->dl_mac, self->m_mac_address, ETH_ALEN);
	bamp->wptr += sizeof(dl_keyronex_bind_ack_t);

	str_freeb(mp);
	str_qreply(wq, bamp);
}

@end

static int
nic_open(queue_t *rq, void *arg)
{
	DKNIC *self = (DKNIC *)arg;
	kassert(self != nil);
	rq->ptr = rq->other->ptr = self;
	return 0;
}

static void
nic_close(queue_t *)
{
	ktodo();
}

static void
nic_rput(queue_t *rq, mblk_t *mp)
{
	str_putnext(rq, mp);
}

static void
nic_wput(queue_t *wq, mblk_t *mp)
{
	DKNIC *self = (DKNIC *)wq->ptr;
	switch (mp->db->type) {
#if 0
	case M_DATA:
		[self transmitPacket:mp];
		break;
#endif

	case M_PROTO: {
		union DL_primitives *dlp = (typeof(dlp))mp->rptr;
		switch (dlp->dl_primitive) {
#if 0
		case DL_BIND_REQ:
			[self wput:wq bindReq:mp];
			break;
#endif

		case DL_KEYRONEX_BIND_REQ:
			[self wput:wq keyronexBindReq:mp];
			break;

		default:
			kfatal("nic_wput: unhandled DLPI primitive %d",
			    dlp->dl_primitive);
			break;
		}
		break;

	default:
		kfatal("nic_wput: unhandled mblk type %d", mp->db->type);
		break;
	}
	}
}
