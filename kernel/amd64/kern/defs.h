/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Feb 22 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file defs.h
 * @brief Brief explanation.
 */

#ifndef ECX_KERN_DEFS_H
#define ECX_KERN_DEFS_H

#include <stdint.h>

struct __attribute__((packed)) tss {
	uint32_t reserved;
	uint64_t rsp0;
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist1;
	uint64_t ist2;
	uint64_t ist3;
	uint64_t ist4;
	uint64_t ist5;
	uint64_t ist6;
	uint64_t ist7;
	uint64_t reserved2;
	uint32_t iopb;
};

#endif /* ECX_KERN_DEFS_H */
