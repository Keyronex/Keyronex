/*
 * Copyright (c) 2023-2025 Cloudarox Solutions.
 * Created on Thu Sep 14 2025.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file SimpleFramebuffer.m
 * @brief Simple framebuffer class.
 */

#include <sys/errno.h>
#include <sys/mman.h>
#include <sys/proc.h>

#include <devicekit/SimpleFramebuffer.h>
#include <fs/devfs/devfs.h>

static struct dev_ops fb_devops;
static dev_class_t fb_devclass = {
	.kind = DEV_KIND_CHAR,
	.charops = &fb_devops
};

@implementation DKFramebuffer
@synthesize fix_info = m_fix_info;
@synthesize var_info = m_var_info;
@end

@implementation SimpleFramebuffer

- (instancetype)initWithFix:(struct fb_fix_screeninfo *)fixinfo
			var:(struct fb_var_screeninfo *)varinfo
{
	self = [super init];

	m_fix_info = *fixinfo;
	m_var_info = *varinfo;

	return self;
}

- (void)start
{
#if 0
	FBTerminal *terminal = [[FBTerminal alloc] initWithFramebuffer:self];
	[terminal attachToProvider:self onAxis:kDKDeviceAxis];
	[terminal start];
#endif
	devfs_create_node(&fb_devclass, self, "fb0");
}

@end

static int
fbdev_read(void *dev, void *buf, size_t len, io_off_t offset, int)
{
	kdprintf(" == fbdev_read is unimplemented\n");
	return -ENOSYS;
}

static int
fbdev_write(void *dev, const void *buf, size_t len, io_off_t offset, int)
{
	kdprintf(" == fbdev_write is unimplemented\n");
	return -ENOSYS;
}

static int
fbdev_mmap(void *addr, size_t len, int prot, int flags, void *dev,
    io_off_t offset, vaddr_t *window)
{
	SimpleFramebuffer *self = dev;
	int r;

	r = vm_map_phys(proc_curproc()->vm_map,
	    self->m_fix_info.smem_start + offset, window, len, prot,
	    kCacheModeWC, flags & MAP_FIXED);

	return r;
}

static int
fbdev_ioctl(void *dev, unsigned long command, void *arg)
{
	SimpleFramebuffer *self = dev;

	switch (command) {
	case FBIOGET_VSCREENINFO:
		*(struct fb_var_screeninfo *)arg = self->m_var_info;
		return 0;
	case FBIOGET_FSCREENINFO:
		*(struct fb_fix_screeninfo *)arg = self->m_fix_info;
		return 0;
	case FBIOPUT_VSCREENINFO:
		self->m_var_info = *(struct fb_var_screeninfo *)arg;
		return 0;
	case FBIOPUTCMAP:
	case FBIOBLANK:
		return 0;

	default:
		kdprintf(" == fbdev_ioctl: unknown command %lu\n", command);
		return -EINVAL;
	}
}

static struct dev_ops fb_devops = {
	.read = fbdev_read,
	.write = fbdev_write,
	.mmap = fbdev_mmap,
	.ioctl = fbdev_ioctl,
};
