/*
 * compare.c --- compare whether or not two UUIDs are the same
 *
 * Returns 0 if the two UUIDs are different, and 1 if they are the same.
 *
 * Copyright (C) 1996, 1997 Theodore Ts'o.
 *
 * %Begin-Header%
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * %End-Header%
 */

#include "uuidP.h"
#include <string.h>
#include <libkern/libkern.h>

#define UUCMP(u1,u2) if (u1 != u2) return((u1 < u2) ? -1 : 1);

int uuid_compare(const uuid_t uu1, const uuid_t uu2)
{
	struct uuid	uuid1, uuid2;

	uuid_unpack(uu1, &uuid1);
	uuid_unpack(uu2, &uuid2);

	UUCMP(uuid1.time_low, uuid2.time_low);
	UUCMP(uuid1.time_mid, uuid2.time_mid);
	UUCMP(uuid1.time_hi_and_version, uuid2.time_hi_and_version);
	UUCMP(uuid1.clock_seq, uuid2.clock_seq);
	return memcmp(uuid1.node, uuid2.node, 6);
}

/* Returns 1 if the uuid is the NULL uuid */
int uuid_is_null(const uuid_t uu)
{
	const unsigned char 	*cp;
	int			i;

	for (i=0, cp = uu; i < 16; i++)
		if (*cp++)
			return 0;
	return 1;
}

void uuid_pack(const struct uuid *uu, uuid_t ptr)
{
	uint32_t	tmp;
	unsigned char	*out = ptr;

	tmp = uu->time_low;
	out[3] = (unsigned char) tmp;
	tmp >>= 8;
	out[2] = (unsigned char) tmp;
	tmp >>= 8;
	out[1] = (unsigned char) tmp;
	tmp >>= 8;
	out[0] = (unsigned char) tmp;

	tmp = uu->time_mid;
	out[5] = (unsigned char) tmp;
	tmp >>= 8;
	out[4] = (unsigned char) tmp;

	tmp = uu->time_hi_and_version;
	out[7] = (unsigned char) tmp;
	tmp >>= 8;
	out[6] = (unsigned char) tmp;

	tmp = uu->clock_seq;
	out[9] = (unsigned char) tmp;
	tmp >>= 8;
	out[8] = (unsigned char) tmp;

	memcpy(out+10, uu->node, 6);
}

#if 0
int uuid_parse(const char *in, uuid_t uu)
{
	struct uuid	uuid;
	int 		i;
	const char	*cp;
	char		buf[3];

	if (strlen(in) != 36)
		return -1;
	for (i=0, cp = in; i <= 36; i++,cp++) {
		if ((i == 8) || (i == 13) || (i == 18) ||
		    (i == 23)) {
			if (*cp == '-')
				continue;
			else
				return -1;
		}
		if (i== 36)
			if (*cp == 0)
				continue;
		if (!isxdigit(*cp))
			return -1;
	}
	uuid.time_low = strtoul(in, NULL, 16);
	uuid.time_mid = strtoul(in+9, NULL, 16);
	uuid.time_hi_and_version = strtoul(in+14, NULL, 16);
	uuid.clock_seq = strtoul(in+19, NULL, 16);
	cp = in+24;
	buf[2] = 0;
	for (i=0; i < 6; i++) {
		buf[0] = *cp++;
		buf[1] = *cp++;
		uuid.node[i] = strtoul(buf, NULL, 16);
	}

	uuid_pack(&uuid, uu);
	return 0;
}
#endif

void uuid_unpack(const uuid_t in, struct uuid *uu)
{
	const uint8_t	*ptr = in;
	uint32_t		tmp;

	tmp = *ptr++;
	tmp = (tmp << 8) | *ptr++;
	tmp = (tmp << 8) | *ptr++;
	tmp = (tmp << 8) | *ptr++;
	uu->time_low = tmp;

	tmp = *ptr++;
	tmp = (tmp << 8) | *ptr++;
	uu->time_mid = tmp;

	tmp = *ptr++;
	tmp = (tmp << 8) | *ptr++;
	uu->time_hi_and_version = tmp;

	tmp = *ptr++;
	tmp = (tmp << 8) | *ptr++;
	uu->clock_seq = tmp;

	memcpy(uu->node, ptr, 6);
}

static const char *fmt_lower =
	"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x";

static const char *fmt_upper =
	"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X";

#ifdef UUID_UNPARSE_DEFAULT_UPPER
#define FMT_DEFAULT fmt_upper
#else
#define FMT_DEFAULT fmt_lower
#endif

static void uuid_unparse_x(const uuid_t uu, char *out, const char *fmt)
{
	struct uuid uuid;

	uuid_unpack(uu, &uuid);
	ksnprintf(out, UUID_STRING_LENGTH, fmt,
		uuid.time_low, uuid.time_mid, uuid.time_hi_and_version,
		uuid.clock_seq >> 8, uuid.clock_seq & 0xFF,
		uuid.node[0], uuid.node[1], uuid.node[2],
		uuid.node[3], uuid.node[4], uuid.node[5]);
}

void uuid_unparse_lower(const uuid_t uu, char *out)
{
	uuid_unparse_x(uu, out,	fmt_lower);
}

void uuid_unparse_upper(const uuid_t uu, char *out)
{
	uuid_unparse_x(uu, out,	fmt_upper);
}

void uuid_unparse(const uuid_t uu, char *out)
{
	uuid_unparse_x(uu, out, FMT_DEFAULT);
}
