/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Mar 29 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file if_packet.h
 * @brief Linux <linux/if_packet.h> stub
 */

#ifndef ECX_LINUX_IF_PACKET_H
#define ECX_LINUX_IF_PACKET_H

#include <stdint.h>

struct sockaddr_ll {
        unsigned short	sll_family;
        uint16_t	sll_protocol;
        int		sll_ifindex;
        unsigned short 	sll_hatype;
        unsigned char	sll_pkttype;
        unsigned char	sll_halen;
        unsigned char	sll_addr[8];
};

#endif /* ECX_LINUX_IF_PACKET_H */
