/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Thu Jan 08 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file DKNIC.h
 * @brief Brief explanation.
 */

#ifndef ECX_DEVICEKIT_DKNIC_H
#define ECX_DEVICEKIT_DKNIC_H

#include <sys/stream.h>

#include <netinet/if_ether.h>

#include <devicekit/DKDevice.h>

@interface DKNIC : DKDevice {
	uint8_t m_mac_address[ETH_ALEN];
}

- (void)setupNIC;
- (void)didReceivePacket:(mblk_t *)mp;

/* subclass responsibitiles follow */

- (void)transmitPacket:(mblk_t *)mp;

@end

#endif /* ECX_DEVICEKIT_DKNIC_H */
