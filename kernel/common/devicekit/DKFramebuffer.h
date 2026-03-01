/*
 * Copyright (c) 2023-2025 Cloudarox Solutions.
 * Created on Thu Sep 14 2023.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file DKFramebuffer.h
 * @brief Abstract class of framebuffers.
 */

#ifndef ECX_DEVICEKIT_DKFRAMEBUFFER_H
#define ECX_DEVICEKIT_DKFRAMEBUFFER_H

#include <devicekit/DKDevice.h>
#include <linux/fb.h>

/*! abstract class of framebuffers */
@interface DKFramebuffer : DKDevice {
    @public
	struct fb_fix_screeninfo m_fix_info;
	struct fb_var_screeninfo m_var_info;
}

@property struct fb_fix_screeninfo fix_info;
@property struct fb_var_screeninfo var_info;

@end


#endif /* ECX_DEVICEKIT_DKFRAMEBUFFER_H */
