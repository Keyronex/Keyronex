/*
 * Copyright (c) 2021-2023 NetaScale Object Solutions.
 * Created on Mon Feb 13 2023.
 */

#ifndef KRX_AMD64_AMD64_H
#define KRX_AMD64_AMD64_H

#include <limine.h>
#include <stdint.h>

#include "kdk/amd64/portio.h"

enum {
	kAMD64MSRAPICBase = 0x1b,
	kAMD64MSRTSCDeadline = 0x6e0,
	kAMD64MSRGSBase = 0xc0000101,
	kAMD64MSRKernelGSBase = 0xc0000102,
	kAMD64MSRFSBase = 0xc0000100
};

enum {
	kIntNumSyscall = 128,
	/*! filterable with IPL=high (CR8=15) */
	kIntNumLAPICTimer = 224,
	kIntNumRescheduleIPI = 225,
	/*! unfilterable */
	kIntNumSwitch = 240, /* Manually invoked with INT */
	kIntNumIPIInvlPG = 241,
};

typedef struct tss {
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
} __attribute__((packed)) tss_t;

#define REG_FUNCS(type, regname)                                    \
	static inline type read_##regname()                         \
	{                                                           \
		type val;                                           \
		asm volatile("mov %%" #regname ", %0" : "=r"(val)); \
		return val;                                         \
	}                                                           \
	static inline void write_##regname(type val)                \
	{                                                           \
		asm volatile("mov %0, %%" #regname ::"a"(val));     \
	}

REG_FUNCS(uint64_t, cr0);
REG_FUNCS(uint64_t, cr2);
REG_FUNCS(uint64_t, cr3);
REG_FUNCS(uint64_t, cr4);

#endif /* KRX_AMD64_AMD64_H */
