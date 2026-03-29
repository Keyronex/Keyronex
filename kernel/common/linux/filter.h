/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Mar 29 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file filter.h
 * @brief Linux-compatible BPF stub.
 */

#ifndef ECX_LINUX_FILTER_H
#define ECX_LINUX_FILTER_H

#include <stdint.h>

struct sock_filter {
	uint16_t	code;
	uint8_t		jt;
	uint8_t		jf;
	uint32_t	k;
};

struct sock_fprog {
	unsigned short		len;
	struct sock_filter	*filter;
};

#define BPF_CLASS(code) ((code) & 0x07)
#define BPF_LD		0x00
#define BPF_LDX		0x01
#define BPF_ST		0x02
#define BPF_STX		0x03
#define BPF_ALU		0x04
#define BPF_JMP		0x05
#define BPF_RET		0x06
#define BPF_MISC	0x07

#define BPF_SIZE(code) ((code) & 0x18)
#define BPF_W	0x00
#define BPF_H	0x08
#define BPF_B	0x10

#define BPF_MODE(code) ((code) & 0xe0)
#define BPF_IMM	0x00
#define BPF_ABS	0x20
#define BPF_IND	0x40
#define BPF_MEM	0x60
#define BPF_LEN	0x80
#define BPF_MSH	0xa0

#define BPF_OP(code) ((code) & 0xf0)
#define BPF_ADD	0x00
#define BPF_SUB	0x10
#define BPF_MUL	0x20
#define BPF_DIV	0x30
#define BPF_OR	0x40
#define BPF_AND	0x50
#define BPF_LSH	0x60
#define BPF_RSH	0x70
#define BPF_NEG	0x80

#define BPF_JA		0x00
#define BPF_JEQ		0x10
#define BPF_JGT		0x20
#define BPF_JGE		0x30
#define BPF_JSET	0x40

#define BPF_SRC(code) ((code) & 0x08)
#define BPF_K	0x00
#define BPF_X	0x08

#define BPF_RVAL(code) ((code) & 0x18)
#define BPF_A	0x00

#define BPF_MISCOP(code) ((code) & 0xf8)
#define BPF_TAX	0x00

#define BPF_STMT(code, k)		{ (uint16_t)code, 0,  0, k }
#define BPF_JUMP(code, k, jt, jf)	{ (uint16_t)code, jt, jf, k }

#endif /* ECX_LINUX_FILTER_H */
