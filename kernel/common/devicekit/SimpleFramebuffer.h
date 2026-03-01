/*
 * Copyright (c) 2023-2025 Cloudarox Solutions.
 * Created on Thu Sep 14 2023.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file SimpleFramebuffer.h
 * @brief Simple framebuffer.
 */

#ifndef ECX_DEVICEKIT_SIMPLEFRAMEBUFFER_H
#define ECX_DEVICEKIT_SIMPLEFRAMEBUFFER_H

#include <devicekit/DKFramebuffer.h>

@interface SimpleFramebuffer : DKFramebuffer {
}

- (instancetype)initWithFix:(struct fb_fix_screeninfo *)fixinfo
			var:(struct fb_var_screeninfo *)varinfo;

@end

#endif /* ECX_DEVICEKIT_SIMPLEFRAMEBUFFER_H */
