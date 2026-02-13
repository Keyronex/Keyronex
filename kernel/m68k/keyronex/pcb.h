/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file pcb.h
 * @brief m68k PCB and trap frame.
 */

#ifndef ECX_KEYRONEX_FRAME_H
#define ECX_KEYRONEX_FRAME_H

#include <stdint.h>

typedef struct __attribute__((packed)) karch_pcb {

} karch_pcb_t;

struct __attribute__((packed)) ssw_68040 {
	uint16_t cp : 1;
	uint16_t cu : 1;
	uint16_t ct : 1;
	uint16_t cm : 1;
	uint16_t ma : 1;
	uint16_t atc : 1;
	uint16_t lk : 1;
	uint16_t rw : 1;
	uint16_t x : 1;
	uint16_t size : 2;
	uint16_t tt : 2;
	uint16_t tm : 3;
};

struct __attribute__((packed)) fslw_68060 {
	uint32_t fixme : 32;
};

typedef struct __attribute__((packed)) karch_trapframe {
	/* Pushed by ourselves. */
	uint32_t usp;
	uint32_t d0, d1, d2, d3, d4, d5, d6, d7;
	uint32_t a0, a1, a2, a3, a4, a5, a6;
#if 0
	uint32_t padding;
#endif

	/* Pushed by the CPU */
	uint16_t sr;
	uint32_t pc;
	uint16_t format : 4;
	uint16_t vector_offset : 12;

	/* Variable parts follow. */
	union {
		/*! 68060 bus error */
		struct __attribute__((packed)) format_4 {
			uint32_t fa;
			struct fslw_68060 fslw;
		} format_4;
		/*! 68040 bus error */
		struct __attribute__((packed)) format_7 {
			/*! effective address*/
			uint32_t ea;
			/*! special status word */
			struct ssw_68040 ssw;
			/*! write-back status */
			uint16_t wb3s, wb2s, wb1s;
			/*! fault address */
			uint32_t fa;
			/* write-back data/addresses (wb1wd can = pd0) */
			uint32_t wb3a, wb3d, wb2a, fb2d, wb1a, wb1d;
			/*! push data lw */
			uint32_t pd1, pd2, pd3;
		} format_7;
	};
} karch_trapframe_t;

#endif /* ECX_KEYRONEX_FRAME_H */
