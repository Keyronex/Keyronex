/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Mar 01 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file fb.h
 * @brief Linux-style fbdev shim.
 */

#ifndef ECX_LINUX_FB_H
#define ECX_LINUX_FB_H

#include <stdint.h>

#define FBIOGET_VSCREENINFO	0x4600
#define FBIOPUT_VSCREENINFO	0x4601
#define FBIOGET_FSCREENINFO	0x4602
#define FBIOPUTCMAP		0x4605
#define FBIOPAN_DISPLAY		0x4606
#define FBIOBLANK		0x4611

struct fb_fix_screeninfo {
	char id[16];
	uintptr_t smem_start; /* framebuffer physical address */
	uint32_t smem_len;
	uint32_t type;
	uint32_t type_aux;
	uint32_t visual;
	uint16_t xpanstep;
	uint16_t ypanstep;
	uint16_t ywrapstep;
	uint32_t line_length;
	uintptr_t mmio_start;
	uint32_t mmio_len;
	uint32_t accel;
	uint16_t capabilities;
	uint16_t reserved[2];
};

struct fb_bitfield {
	uint32_t offset;
	uint32_t length;
	uint32_t msb_right;
};

struct fb_var_screeninfo {
	uint32_t xres;
	uint32_t yres;
	uint32_t xres_virtual;
	uint32_t yres_virtual;
	uint32_t xoffset;
	uint32_t yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	struct fb_bitfield red;
	struct fb_bitfield green;
	struct fb_bitfield blue;
	struct fb_bitfield transp;
	uint32_t nonstd;
	uint32_t activate;

	uint32_t height;
	uint32_t width;
	uint32_t accel_flags;

	uint32_t pixclock;
	uint32_t left_margin;
	uint32_t right_margin;
	uint32_t upper_margin;
	uint32_t lower_margin;
	uint32_t hsync_len;
	uint32_t vsync_len;
	uint32_t sync;
	uint32_t vmode;
	uint32_t rotate;
	uint32_t colorspace;
	uint32_t reserved[4];
};

struct fb_cmap {
	uint32_t start;
	uint32_t len;
	uint16_t *red;
	uint16_t *green;
	uint16_t *blue;
	uint16_t *transp;
};

#define FB_TYPE_PACKED_PIXELS	0

#define FB_ACTIVATE_NOW		0
#define FB_ACTIVATE_TEST	2

#define FB_SYNC_HOR_HIGH_ACT 	1
#define FB_SYNC_VERT_HIGH_ACT	2
#define FB_SYNC_COMP_HIGH_ACT	8
#define FB_SYNC_BROADCAST	16

#define FB_VISUAL_TRUECOLOR	2
#define FB_VISUAL_PSEUDOCOLOR	3
#define FB_VISUAL_DIRECTCOLOR	4

#define FB_VMODE_NONINTERLACED  0
#define FB_VMODE_INTERLACED	1
#define FB_VMODE_DOUBLE		2
#define FB_VMODE_MASK		255

#endif /* ECX_LINUX_FB_H */
