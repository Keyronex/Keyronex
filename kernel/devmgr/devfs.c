/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Mar 26 2023.
 */

#include "kdk/vfs.h"
#include "tmpfs.h"

int
devfs_create(struct device *dev, const char *name, struct vnops *devvnops)
{
	vnode_t *vn;
	struct vattr vattr;

	vattr.size = 0;
	vattr.type = VCHR;
	vattr.rdevice = dev;
	vattr.rdevops = devvnops;

	return VOP_CREAT(dev_vnode, &vn, name, &vattr);
}