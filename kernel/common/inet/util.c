/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Jan 09 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file util.c
 * @brief General utilities for the internet stack.
 */

#include <arpa/inet.h>

uint16_t
ip_checksum(void *data, size_t len)
{
	uint16_t *buf = data;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *buf++;
		len -= 2;
	}

	if (len > 0)
		sum += *(uint8_t *)buf;

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return ~sum;
}


uint32_t
htonl(uint32_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap32(x);
#else
	return x;
#endif
}

uint16_t
htons(uint16_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap16(x);
#else
	return x;
#endif
}

uint32_t
ntohl(uint32_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap32(x);
#else
	return x;
#endif
}

uint16_t
ntohs(uint16_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap16(x);
#else
	return x;
#endif
}
