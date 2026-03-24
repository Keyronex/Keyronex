/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Tue Jan 13 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file dlpi.h
 * @brief Data Link Provider Interface definitions (with Keyronex extensions)
 */

#ifndef ECX_SYS_DLPI_H
#define ECX_SYS_DLPI_H

#include <stdint.h>

struct msgb;

typedef uintptr_t t_uscalar_t;
typedef intptr_t t_scalar_t;

#define DL_BIND_REQ 0x01
#define DL_BIND_ACK 0x04

#define DL_KEYRONEX_BIND_REQ 0x1001
#define DL_KEYRONEX_BIND_ACK 0x1002

#define DL_CLDLS 0x0200 /* connectionless data link service */

typedef struct dl_bind_req {
	t_uscalar_t dl_primitive;
	t_uscalar_t dl_sap;
	t_uscalar_t dl_max_conind;
	unsigned short dl_service_mode;
	unsigned short dl_conn_mgmt;
	t_uscalar_t dl_xidtest_flg;
} dl_bind_req_t;

typedef struct dl_bind_ack {
	t_uscalar_t dl_primitive;
	t_uscalar_t dl_sap; /* keyronex extension: -1 = all SAPs */
	t_uscalar_t dl_addr_length;
	t_uscalar_t dl_addr_offset;
	t_uscalar_t dl_max_conind;
	t_uscalar_t dl_xidtest_flg;
} dl_bind_ack_t;

typedef struct dl_keyronex_bind_req {
	t_uscalar_t dl_primitive;
} dl_keyronex_bind_req_t;

typedef struct dl_keyronex_bind_ack {
	t_uscalar_t dl_primitive;
	uint8_t dl_mac[6];
	void **pdata;
	void (**pput)(void *data, struct msgb *);
	/* interface entry points will go here... */
	void *nic_data;
	int (*nic_wput)(void *data, struct msgb *);
} dl_keyronex_bind_ack_t;

union DL_primitives {
	t_uscalar_t dl_primitive;
	dl_bind_req_t bind_req;
	dl_bind_ack_t bind_ack;

	dl_keyronex_bind_req_t keyronex_bind_req;
	dl_keyronex_bind_ack_t keyronex_bind_ack;
};

#endif /* ECX_SYS_DLPI_H */
