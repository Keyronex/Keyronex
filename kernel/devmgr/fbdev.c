/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Apr 23 2023.
 */

#include <kdk/devmgr.h>
#include <kdk/vfs.h>
#include <linux/fb.h>

#include "kdk/vm.h"

int devfs_create(struct device *dev, const char *name, struct vnops *devvnops);

static struct fbdev {
	struct device dev;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
} fbdev;
static struct vnops fbdev_vnops;

int
fbdev_init(void)
{
	devfs_create(&fbdev.dev, "fb0", &fbdev_vnops);
	syscon_getfbinfo(&fbdev.var, &fbdev.fix);
	return 0;
}

static int
fbdev_ioctl(vnode_t *vn, unsigned long command, void *arg)
{
	/* syscon_inhibit(); */
	switch (command) {
	case FBIOGET_VSCREENINFO:
		*(struct fb_var_screeninfo *)arg = fbdev.var;
		return 0;
	case FBIOGET_FSCREENINFO:
		*(struct fb_fix_screeninfo *)arg = fbdev.fix;
		return 0;
	case FBIOPUT_VSCREENINFO:
		fbdev.var = *(struct fb_var_screeninfo *)arg;
		return 0;
	case FBIOPUTCMAP:
	case FBIOBLANK:
		return 0;
	}

	kdprintf("unsupported fbdev ioctl 0x%lx\n", command);
	return 0;
}

static int
fbdev_mmap(vnode_t *vn, vm_map_t *map, krx_inout vaddr_t *vaddrp, size_t size,
    voff_t offset, vm_protection_t initial_protection,
    vm_protection_t max_protection, enum vm_inheritance inheritance, bool exact,
    bool copy)
{
	kassert(!copy);
	kassert(!offset);
	kassert(inheritance == kVMInheritShared);

	return vm_map_phys(map, fbdev.fix.smem_start, vaddrp, size,
	    initial_protection, max_protection, inheritance, exact);
}

static int
fbdev_read(vnode_t *vn, void *buf, size_t nbyte, off_t offset, int unused)
{
	kfatal("unimplemented\n");
}

static int
fbdev_write(vnode_t *vn, void *buf, size_t nbyte, off_t offset, int unused)
{
	kfatal("unimplemented\n");
}

static struct vnops fbdev_vnops = {
	.ioctl = fbdev_ioctl,
	.mmap = fbdev_mmap,
	.read = fbdev_read,
	.write = fbdev_write,
};
