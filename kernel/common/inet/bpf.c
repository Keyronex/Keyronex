/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Mon Mar 30 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file bpf.c
 * @brief Berkeley Packet Filter
 */

#include <sys/libkern.h>
#include <sys/stream.h>

#include <linux/filter.h>

static bool
str_mmemcpy_out(const mblk_t *mp, size_t off, void *dst, size_t len)
{
	uint8_t *out = dst;

	while (mp != NULL) {
		size_t seglen = STR_MBLKL(mp);

		if (off < seglen)
			break;

		off -= seglen;
		mp = mp->cont;
	}

	if (mp == NULL)
		return 0;

	while (len != 0 && mp != NULL) {
		size_t seglen = STR_MBLKL(mp);
		size_t n;

		if (off >= seglen) {
			off -= seglen;
			mp = mp->cont;
			continue;
		}

		seglen -= off;
		n = (seglen < len) ? seglen : len;

		memcpy(out, mp->rptr + off, n);
		out += n;
		len -= n;

		off = 0;
		mp = mp->cont;
	}

	return (len == 0);
}

static bool
mblk_load_u8(const mblk_t *mp, size_t off, uint32_t *v)
{
	uint8_t x;

	if (!str_mmemcpy_out(mp, off, &x, sizeof(x)))
		return false;

	*v = x;
	return true;
}

static bool
mblk_load_be16(const mblk_t *mp, size_t off, uint32_t *v)
{
	uint8_t b[2];

	if (!str_mmemcpy_out(mp, off, b, sizeof(b)))
		return false;

	*v = ((uint32_t)b[0] << 8) | (uint32_t)b[1];
	return true;
}

static bool
mblk_load_be32(const mblk_t *mp, size_t off, uint32_t *v)
{
	uint8_t b[4];

	if (!str_mmemcpy_out(mp, off, b, sizeof(b)))
		return false;

	*v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	    ((uint32_t)b[2] << 8) | (uint32_t)b[3];
	return true;
}

uint32_t
bpf_filter(const struct sock_filter *pc, size_t codelen, const mblk_t *mp)
{
	uint32_t A = 0, X = 0, M[16] = { 0 };
	const struct sock_filter *end = pc + codelen;
	size_t len = str_msgsize(mp);

	goto begin;

loop:
	pc++;
	if (pc >= end) {
		kdprintf("bpf_filter: pc out of range\n");
		return 0;
	}

begin:
	switch (pc->code) {
	case BPF_LD | BPF_W | BPF_ABS:
		if (!mblk_load_be32(mp, pc->k, &A)) {
			kdprintf("bpf: failed to load 4 bytes at offset %u\n",
			    pc->k);
			return 0;
		}
		goto loop;

	case BPF_LD | BPF_H | BPF_ABS:
		if (!mblk_load_be16(mp, pc->k, &A)) {
			kdprintf("bpf: failed to load 2 bytes at offset %u\n",
			    pc->k);
			return 0;
		}
		goto loop;

	case BPF_LD | BPF_B | BPF_ABS:
		if (!mblk_load_u8(mp, pc->k, &A)) {
			kdprintf("bpf: failed to load byte at offset %u\n",
			    pc->k);
			return 0;
		}
		goto loop;

	case BPF_LD | BPF_W | BPF_IND: {
		uint32_t off;
		if (__builtin_add_overflow(X, pc->k, &off)) {
			kdprintf("bpf: overflow for LD|W|IND (X=%u k=%u)\n", X,
			    pc->k);
			return 0;
		}
		if (!mblk_load_be32(mp, off, &A)) {
			kdprintf("bpf: failed to load 4 bytes at offset %u\n",
			    off);
			return 0;
		}
		goto loop;
	}

	case BPF_LD | BPF_H | BPF_IND: {
		uint32_t off;
		if (__builtin_add_overflow(X, pc->k, &off)) {
			kdprintf("bpf: overflow for LD|H|IND (X=%u k=%u)\n", X,
			    pc->k);
			return 0;
		}
		if (!mblk_load_be16(mp, off, &A)) {
			kdprintf("bpf: failed to load 2 bytes at offset %u\n",
			    off);
			return 0;
		}
		goto loop;
	}

	case BPF_LD | BPF_B | BPF_IND: {
		uint32_t off;
		if (__builtin_add_overflow(X, pc->k, &off)) {
			kdprintf("bpf: overflow for LD|B|IND (X=%u k=%u)\n", X,
			    pc->k);
			return 0;
		}
		if (!mblk_load_u8(mp, off, &A)) {
			kdprintf("bpf: failed to load byte at offset %u\n",
			    off);
			return 0;
		}
		goto loop;
	}

	case BPF_LD | BPF_W | BPF_LEN:
		A = (uint32_t)len;
		goto loop;

	case BPF_LDX | BPF_W | BPF_LEN:
		X = (uint32_t)len;
		goto loop;

	case BPF_LDX | BPF_B | BPF_MSH: {
		uint32_t v;
		if (!mblk_load_u8(mp, pc->k, &v)) {
			kdprintf("bpf: failed to load byte at offset %u\n",
			    pc->k);
			return 0;
		}
		X = (v & 0x0f) << 2;
		goto loop;
	}

	case BPF_LD | BPF_IMM:
		A = pc->k;
		goto loop;

	case BPF_LDX | BPF_IMM:
		X = pc->k;
		goto loop;

	case BPF_LD | BPF_MEM:
		if (pc->k >= 16)
			return 0;
		A = M[pc->k];
		goto loop;

	case BPF_LDX | BPF_MEM:
		if (pc->k >= 16)
			return 0;
		X = M[pc->k];
		goto loop;

	case BPF_ST:
		if (pc->k >= 16)
			return 0;
		M[pc->k] = A;
		goto loop;

	case BPF_STX:
		if (pc->k >= 16)
			return 0;
		M[pc->k] = X;
		goto loop;

	case BPF_ALU | BPF_ADD | BPF_K:
		A += pc->k;
		goto loop;

	case BPF_ALU | BPF_ADD | BPF_X:
		A += X;
		goto loop;

	case BPF_ALU | BPF_SUB | BPF_K:
		A -= pc->k;
		goto loop;

	case BPF_ALU | BPF_SUB | BPF_X:
		A -= X;
		goto loop;

	case BPF_ALU | BPF_MUL | BPF_K:
		A *= pc->k;
		goto loop;

	case BPF_ALU | BPF_MUL | BPF_X:
		A *= X;
		goto loop;

	case BPF_ALU | BPF_DIV | BPF_K:
		if (pc->k == 0)
			return 0;
		A /= pc->k;
		goto loop;

	case BPF_ALU | BPF_DIV | BPF_X:
		if (X == 0)
			return 0;
		A /= X;
		goto loop;

	case BPF_ALU | BPF_AND | BPF_K:
		A &= pc->k;
		goto loop;

	case BPF_ALU | BPF_AND | BPF_X:
		A &= X;
		goto loop;

	case BPF_ALU | BPF_OR | BPF_K:
		A |= pc->k;
		goto loop;

	case BPF_ALU | BPF_OR | BPF_X:
		A |= X;
		goto loop;

	case BPF_ALU | BPF_LSH | BPF_K:
		A <<= pc->k;
		goto loop;

	case BPF_ALU | BPF_LSH | BPF_X:
		A <<= X;
		goto loop;

	case BPF_ALU | BPF_RSH | BPF_K:
		A >>= pc->k;
		goto loop;

	case BPF_ALU | BPF_RSH | BPF_X:
		A >>= X;
		goto loop;

	case BPF_ALU | BPF_NEG:
		A = -A;
		goto loop;

	case BPF_MISC | BPF_TAX:
		X = A;
		goto loop;

	case BPF_MISC | BPF_TXA:
		A = X;
		goto loop;

	case BPF_JMP | BPF_JA:
		pc += pc->k;
		goto loop;

	case BPF_JMP | BPF_JGT | BPF_K:
		pc += (A > pc->k) ? pc->jt : pc->jf;
		goto loop;

	case BPF_JMP | BPF_JGT | BPF_X:
		pc += (A > X) ? pc->jt : pc->jf;
		goto loop;

	case BPF_JMP | BPF_JGE | BPF_K:
		pc += (A >= pc->k) ? pc->jt : pc->jf;
		goto loop;

	case BPF_JMP | BPF_JGE | BPF_X:
		pc += (A >= X) ? pc->jt : pc->jf;
		goto loop;

	case BPF_JMP | BPF_JEQ | BPF_K:
		pc += (A == pc->k) ? pc->jt : pc->jf;
		goto loop;

	case BPF_JMP | BPF_JEQ | BPF_X:
		pc += (A == X) ? pc->jt : pc->jf;
		goto loop;

	case BPF_JMP | BPF_JSET | BPF_K:
		pc += ((A & pc->k) != 0) ? pc->jt : pc->jf;
		goto loop;

	case BPF_JMP | BPF_JSET | BPF_X:
		pc += ((A & X) != 0) ? pc->jt : pc->jf;
		goto loop;

	case BPF_RET | BPF_A:
		return A;

	case BPF_RET | BPF_K:
		return pc->k;

	default:
		kdprintf("bpf_filter: invalid instruction code 0x%02x?\n",
		    pc->code);
		return 0;
	}
}

bool
bpf_validate(const struct sock_fprog *fprog)
{
	if (fprog->len == 0 || fprog->len > 4096)
		return false;

	for (size_t i = 0; i < fprog->len; i++) {
		const struct sock_filter *insn = &fprog->filter[i];

		switch (insn->code) {
		case BPF_LD | BPF_W | BPF_ABS:
		case BPF_LD | BPF_H | BPF_ABS:
		case BPF_LD | BPF_B | BPF_ABS:
		case BPF_LD | BPF_W | BPF_IND:
		case BPF_LD | BPF_H | BPF_IND:
		case BPF_LD | BPF_B | BPF_IND:
		case BPF_LD | BPF_W | BPF_LEN:
		case BPF_LDX | BPF_W | BPF_LEN:
		case BPF_LDX | BPF_B | BPF_MSH:
		case BPF_LD | BPF_IMM:
		case BPF_LDX | BPF_IMM:
		case BPF_ALU | BPF_ADD | BPF_K:
		case BPF_ALU | BPF_ADD | BPF_X:
		case BPF_ALU | BPF_SUB | BPF_K:
		case BPF_ALU | BPF_SUB | BPF_X:
		case BPF_ALU | BPF_MUL | BPF_K:
		case BPF_ALU | BPF_MUL | BPF_X:
		case BPF_ALU | BPF_DIV | BPF_X:
		case BPF_ALU | BPF_AND | BPF_K:
		case BPF_ALU | BPF_AND | BPF_X:
		case BPF_ALU | BPF_OR  | BPF_K:
		case BPF_ALU | BPF_OR  | BPF_X:
		case BPF_ALU | BPF_LSH | BPF_K:
		case BPF_ALU | BPF_LSH | BPF_X:
		case BPF_ALU | BPF_RSH | BPF_K:
		case BPF_ALU | BPF_RSH | BPF_X:
		case BPF_ALU | BPF_NEG:
		case BPF_MISC | BPF_TAX:
		case BPF_MISC | BPF_TXA:
		case BPF_RET | BPF_A:
		case BPF_RET | BPF_K:
			break;

		case BPF_ALU | BPF_DIV | BPF_K:
			if (insn->k == 0)
				return false;
			break;

		case BPF_LD | BPF_MEM:
		case BPF_LDX | BPF_MEM:
		case BPF_ST:
		case BPF_STX:
			if (insn->k >= 16)
				return false;
			break;

		case BPF_JMP | BPF_JA: {
			uint32_t to;

			/*
			 * i + 1 is certainly valid as i <= 4096, but k may be
			 * too big.
			 */
			if (__builtin_add_overflow(i + 1, insn->k, &to))
				return false;

			if (to >= fprog->len)
				return false;

			break;
		}

		case BPF_JMP | BPF_JGT | BPF_K:
		case BPF_JMP | BPF_JGT | BPF_X:
		case BPF_JMP | BPF_JGE | BPF_K:
		case BPF_JMP | BPF_JGE | BPF_X:
		case BPF_JMP | BPF_JEQ | BPF_K:
		case BPF_JMP | BPF_JEQ | BPF_X:
		case BPF_JMP | BPF_JSET | BPF_K:
		case BPF_JMP | BPF_JSET | BPF_X: {
			uint32_t t, f;

			if (__builtin_add_overflow(i + 1, insn->jt, &t))
				return false;
			if (__builtin_add_overflow(i + 1, insn->jf, &f))
				return false;

			if (t >= fprog->len || f >= fprog->len)
				return false;

			break;
		}

		default:
			return false;
		}
	}

	/* must end with a return */
	if (BPF_CLASS(fprog->filter[fprog->len - 1].code) != BPF_RET)
		return false;

	return true;
}
