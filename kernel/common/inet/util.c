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

#include <sys/k_log.h>

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

/* borrowed out of mlibc */
const char *
inet_ntop(int af, const void *__restrict src, char *__restrict dst,
    socklen_t size)
{
	switch (af) {
	case AF_INET: {
		const struct in_addr *source = (typeof(source))src;
		uint32_t addr = ntohl(source->s_addr);
		if (ksnprintf(dst, size, "%d.%d.%d.%d", (addr >> 24) & 0xff,
			(addr >> 16) & 0xff, (addr >> 8) & 0xff,
			addr & 0xff) < (int)size)
			return dst;
		break;
	}

	case AF_INET6: {
		const struct in6_addr *source = (typeof(source))src;
		size_t cur_zeroes_off = 0;
		size_t cur_zeroes_len = 0;
		size_t max_zeroes_off = 0;
		size_t max_zeroes_len = 0;

		/* we look for the largest block of zeroed quartet(s) */
		for (size_t i = 0; i < 8; i++) {
			const uint8_t *ptr = source->s6_addr + (i * 2);
			if (!ptr[0] && !ptr[1]) {
				cur_zeroes_len++;
				if (max_zeroes_len < cur_zeroes_len) {
					max_zeroes_len = cur_zeroes_len;
					max_zeroes_off = cur_zeroes_off;
				}
			} else {
				/* advance the offset to the next quartet to
				 * check */
				cur_zeroes_len = 0;
				cur_zeroes_off = i + 1;
			}
		}

		size_t off = 0;
		for (size_t i = 0; i < 8; i++) {
			const uint8_t *ptr = source->s6_addr + (i * 2);

			/* if we are at the beginning of the largest block of
			 * zeroed quartets, place "::" */
			if (i == max_zeroes_off && max_zeroes_len >= 2) {
				if (off < size) {
					dst[off++] = ':';
				}
				if (off < size) {
					dst[off++] = ':';
				}
				i += max_zeroes_len - 1;

				continue;
			}

			/* place a colon if we're not at the beginning of the
			 * string and it is not already there */
			if (off && dst[off - 1] != ':') {
				if (off < size) {
					dst[off++] = ':';
				}
			}

			off += ksnprintf(dst + off, size - off, "%x",
			    ptr[0] << 8 | ptr[1]);
		}

		dst[off] = 0;

		return dst;
	}
	default:
		return NULL;
	}

	return NULL;
}
