/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file pcb.h
 * @brief amd64 PCB and trap frame.
 */

#ifndef ECX_KEYRONEX_FRAME_H
#define ECX_KEYRONEX_FRAME_H

#include <stdint.h>

typedef struct __attribute__((packed)) karch_pcb {
	uint64_t rbp, rbx, r12, r13, r14, r15, rdi, rsi, rsp;
	uint64_t align;
	uint8_t fpu[512];
	uintptr_t unused;
} karch_pcb_t;

typedef struct __attribute__((packed)) karch_trapframe {
	uint64_t rax, rbx, rcx, rdx, rdi, rsi, r8, r9, r10, r11, r12, r13, r14,
	    r15, rbp;
	uint64_t code, rip, cs, rflags, rsp, ss;
} karch_trapframe_t;

#endif /* ECX_KEYRONEX_FRAME_H */
