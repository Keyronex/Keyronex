/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Apr 27 2023.
 */
/*
 * @file devmgr/mouse.c
 * @brief Temporary (?) simple mouse device for userland use.
 */

#include <dev/mouse.h>

#include <kdk/devmgr.h>
#include <kdk/libkern.h>
#include <kdk/vfs.h>

#include "executive/epoll.h"
#include "kdk/kerndefs.h"
#include "kdk/process.h"

struct mouse {
	struct device dev;
	kspinlock_t lock;
	bool opened;
	struct mouse_packet buf[256];
	short head;
	short tail;
	short count;

	kevent_t read_evobj;
	struct polllist polllist;
};

int devfs_create(struct device *dev, const char *name, struct vnops *devvnops);

static struct mouse scmouse;
static struct vnops mouse_vnops;

void
mouse_dispatch(struct mouse_packet packet)
{
	struct pollhead *ph, *tmp;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&scmouse.lock);
	if (scmouse.count == elementsof(scmouse.buf)) {
		// kfatal("out of space in ring buffer");
		goto out;
	}
	scmouse.buf[scmouse.head++] = packet;
	scmouse.head %= elementsof(scmouse.buf);
	scmouse.count++;
	ke_event_signal(&scmouse.read_evobj);
	LIST_FOREACH_SAFE (ph, &scmouse.polllist.pollhead_list, polllist_entry, tmp) {
		pollhead_raise(ph, EPOLLIN);
	}
out:
	ke_spinlock_release(&scmouse.lock, ipl);
}

struct mouse *
mouse_init(void)
{
	ke_spinlock_init(&scmouse.lock);
	ke_event_init(&scmouse.read_evobj, false);
	devfs_create(&scmouse.dev, "mouse", &mouse_vnops);
	return 0;
}

static int
mouse_chpoll(vnode_t *vn, struct pollhead *ph, enum chpoll_kind kind)
{
	struct mouse *mouse = (struct mouse *)(vn->rdevice);
	int r = 0;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&mouse->lock);

	switch (kind) {
	case kChPollAdd:
		ph->revents = 0;

		if ((ph->event.events & EPOLLIN) && mouse->count != 0) {
			ph->revents |= EPOLLIN;
			r = 1;
		}

		if (r != 1)
			LIST_INSERT_HEAD(&mouse->polllist.pollhead_list, ph,
			    polllist_entry);

		break;

	case kChPollChange:
		kfatal("Unimplemented");

	case kChPollRemove:
		LIST_REMOVE(ph, polllist_entry);
		break;
	}

	ke_spinlock_release(&mouse->lock, ipl);

	return r;
}

static int
mouse_ioctl(vnode_t *vn, unsigned long command, void *data)
{
	return 0;
}

static int
mouse_open(krx_inout vnode_t **vn, int mode)
{
	struct mouse *mouse = (struct mouse *)((*vn)->rdevice);

	mouse->opened = true;

	return 0;
}

static int
mouse_read(vnode_t *vn, void *buf, size_t nbyte, off_t offset)
{
	struct mouse *mouse = (struct mouse *)(vn->rdevice);
	struct mouse_packet packet_read = { .flags = 255 };
	ipl_t ipl;
	kwaitstatus_t w;

	kassert(nbyte >= sizeof(struct mouse_packet));

wait:
	w = ke_wait(&mouse->read_evobj, "mouse_read:mouse->read_evobj", false,
	    false, -1);
	kassert(w == kKernWaitStatusOK);
	ipl = ke_spinlock_acquire(&mouse->lock);
	if (mouse->count == 0) {
		ke_spinlock_release(&mouse->lock, ipl);
		goto wait;
	}

	packet_read = mouse->buf[mouse->tail++];
	mouse->tail %= elementsof(mouse->buf);
	mouse->count--;

	ke_spinlock_release(&mouse->lock, ipl);

	if (packet_read.flags != 255) {
		memcpy(buf, &packet_read, sizeof(packet_read));
	}

	return sizeof(packet_read);
}

static struct vnops mouse_vnops = {
	.chpoll = mouse_chpoll,
	.ioctl = mouse_ioctl,
	.open = mouse_open,
	.read = mouse_read,
};