/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2020-2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

#ifndef DEV_H_
#define DEV_H_

#include <sys/types.h>
#include <nanokern/queue.h>

#include <stdbool.h>

struct knote;
struct proc;
struct vnode;

typedef struct cdevsw {
	bool valid : 1, is_tty : 1;
	void *private;
	int (*open)(dev_t dev, struct vnode **out, int mode);
	int (*read)(dev_t dev, void *buf, size_t nbyte, off_t off);
	int (*write)(dev_t dev, void *buf, size_t nbyte, off_t off);
	int (*kqfilter)(dev_t dev, struct knote *kn);
} cdevsw_t;

typedef struct specdev {
	LIST_ENTRY(specdev) queue;
	dev_t		    dev;
	LIST_HEAD(, vnode) vnodes;
} specdev_t;

/* Attach an entry to the  device switch; its major number is returned. */
int cdevsw_attach(cdevsw_t *bindings);

/*! Make a DevFS node (really just a tmpfs node right now) */
int devfs_make_node(dev_t dev, const char *name);

/*! Setup a (per-actual-file-referencing-the-device) vnode. */
void spec_setup_vnode(struct vnode *vn, dev_t dev);

int spec_open(struct vnode *vn, struct vnode **out, int mode);
int spec_read(struct vnode *vn, void *buf, size_t nbyte, off_t off);
int spec_write(struct vnode *vn, void *buf, size_t nbyte, off_t off);
int spec_kqfilter(struct vnode *vn, struct knote *kn);

extern cdevsw_t cdevsw[64];

#endif /* DEV_H_ */
