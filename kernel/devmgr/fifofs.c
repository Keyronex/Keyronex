/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Apr 09 2023.
 */

#include <sys/errno.h>

#include <abi-bits/fcntl.h>
#include <abi-bits/stat.h>

#include "executive/epoll.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vfs.h"

static struct vnops fifofs_vnops;

/*!
 * Lock ordering: mtx -> pollist_lock
 * (p) => polllist_lock
 * (m) => mtx
 */
struct fifonode {
	kmutex_t mtx;
	kspinlock_t polllist_lock;

	/* (m) */
	size_t read_refcnt, write_refcnt;
	/* (~, m to modify) */
	uint8_t *data;
	/* (~) */
	size_t size;
	/* (m or p to check, m+p to set) */
	size_t count;

	/* (m) */
	size_t read_head, write_head;

	/* (p) */
	struct polllist polllist;
	/* (p to check, m+p to set) */
	kevent_t readable, writeable;
	/* (m or p to check, m+p to set) */
	size_t nreaders, nwriters;
};

static void
vnode_init(vnode_t *vn, vtype_t type, vfs_t *vfs, struct vnops *ops)
{
	obj_initialise_header(&vn->vmobj.objhdr, kObjTypeVNode);
	vn->type = type;
	vn->vfsp = vfs;
	vn->ops = ops;
	vn->vfsmountedhere = NULL;
	vn->size = 0;
	ke_mutex_init(&vn->lock);
}

int
sys_pipe(int *out, int flags)
{
	struct fifonode *pnode;
	int fds[2];
	vnode_t *read_vn, *write_vn;
	int r;

	r = ps_allocfiles(2, fds);
	kassert(r == 0);

	pnode = kmem_alloc(sizeof(*pnode));
	ke_mutex_init(&pnode->mtx);
	pnode->data = kmem_alloc(PGSIZE);
	pnode->size = PGSIZE;
	pnode->count = 0;
	pnode->read_head = pnode->write_head = 0;
	pnode->nreaders = pnode->nwriters = 1;

	ke_spinlock_init(&pnode->polllist_lock);
	LIST_INIT(&pnode->polllist.pollhead_list);
	ke_event_init(&pnode->readable, false);
	ke_event_init(&pnode->writeable, true);

	read_vn = kmem_alloc(sizeof(vnode_t));
	write_vn = kmem_alloc(sizeof(vnode_t));

	vnode_init(read_vn, VFIFO, NULL, &fifofs_vnops);
	vnode_init(write_vn, VFIFO, NULL, &fifofs_vnops);

	read_vn->data = (uintptr_t)pnode;
	read_vn->data2 = O_RDONLY;
	write_vn->data = (uintptr_t)pnode;
	write_vn->data2 = O_WRONLY;

	for (int i = 0; i < 2; i++) {
		ps_curproc()->files[fds[i]] = kmem_alloc(sizeof(struct file));
		obj_initialise_header(&ps_curproc()->files[fds[i]]->objhdr,
		    kObjTypeFile);
		ps_curproc()->files[fds[i]]->offset = 0;
	}

	ps_curproc()->files[fds[0]]->vn = read_vn;
	ps_curproc()->files[fds[1]]->vn = write_vn;

	out[0] = fds[0];
	out[1] = fds[1];

	return 0;
}

static int
fifofs_close(vnode_t *vn)
{
	struct fifonode *pnode = (struct fifonode *)vn->data;
	ipl_t ipl;

	ke_wait(&pnode->mtx, "fifofs_close:pnode->mtx", false, false, -1);
	ipl = ke_spinlock_acquire(&pnode->polllist_lock);

	if (vn->data2 == O_WRONLY) {
		if (--pnode->nwriters == 0) {
			// pollhead_raise(&pnode->polllist, int events)
			ke_event_signal(&pnode->readable);
		}
	} else {
		kassert(vn->data2 == O_RDONLY);
		if (--pnode->nreaders == 0) {
			// pollhead_raise(&pnode->polllist, int events)
			ke_event_signal(&pnode->writeable);
		}
	}

	ke_spinlock_release(&pnode->polllist_lock, ipl);
	ke_mutex_release(&pnode->mtx);

	return 0;
}

static int
fifofs_chpoll(vnode_t *vn, struct pollhead *ph, enum chpoll_kind kind)
{
	struct fifonode *pnode = (struct fifonode *)vn->data;
	ipl_t ipl = ke_spinlock_acquire(&pnode->polllist_lock);
	int r = 0;

	if (kind == kChPollAdd) {
		ph->revents = 0;

		if ((pnode->count > 0) && ph->event.events & EPOLLIN) {
			ph->revents |= EPOLLIN;
			r = 1;
		} else if (pnode->count < pnode->size &&
		    ph->event.events & EPOLLOUT) {
			ph->revents |= EPOLLOUT;
			r = 1;
		}

		LIST_INSERT_HEAD(&pnode->polllist.pollhead_list, ph,
		    polllist_entry);
	} else if (kind == kChPollChange) {
		kfatal("Unimplemented");
	} else if (kind == kChPollRemove) {
		LIST_REMOVE(ph, polllist_entry);
	}

	ke_spinlock_release(&pnode->polllist_lock, ipl);

	return r;
}

static int
fifofs_read(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	struct fifonode *pnode = (struct fifonode *)vn->data;
	ipl_t ipl;
	size_t nread = 0;
	uint8_t *cbuf = buf;

	if (vn->data2 != O_RDONLY)
		return -EINVAL;

wait:
	ke_wait(&pnode->mtx, "fifofs_read:mtx", false, false, -1);

	if (pnode->count == 0) {
		ke_mutex_release(&pnode->mtx);

		if (pnode->nwriters == 0)
			return 0;

		ke_wait(&pnode->readable, "fifofs_read:readable", false, false,
		    -1);
		goto wait;
	}

	ipl = ke_spinlock_acquire(&pnode->polllist_lock);

	if (nbyte > pnode->count) {
		nbyte = pnode->count;
		pnode->count = 0;
		ke_event_clear(&pnode->readable);
	} else
		pnode->count -= nbyte;

	ke_event_signal(&pnode->writeable);

	ke_spinlock_release(&pnode->polllist_lock, ipl);

	nread = nbyte;

	while (nbyte-- > 0) {
		*cbuf++ = pnode->data[pnode->read_head++ % pnode->size];
	}

	ke_mutex_release(&pnode->mtx);

	return nread;
}

static int
fifofs_write(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	struct fifonode *pnode = (struct fifonode *)vn->data;
	ipl_t ipl;
	size_t nwritten = 0;
	uint8_t *cbuf = buf;

	if (vn->data2 != O_WRONLY)
		return -EINVAL;

wait:
	ke_wait(&pnode->mtx, "fifofs_write:mtx", false, false, -1);

	while (nwritten < nbyte) {
		size_t to_write = nbyte - nwritten;

		if (pnode->nreaders == 0) {
			ke_mutex_release(&pnode->mtx);
			return -EPIPE;
		}

		if (pnode->count == pnode->size) {
			ke_mutex_release(&pnode->mtx);
			ke_wait(&pnode->writeable, "fifofs_read:writeable",
			    false, false, -1);
			goto wait;
		}

		ipl = ke_spinlock_acquire(&pnode->polllist_lock);

		if (pnode->count + to_write >= pnode->size) {
			to_write = MIN2(pnode->size - pnode->count, to_write);
			ke_event_clear(&pnode->writeable);
		}

		pnode->count += to_write;

		ke_spinlock_release(&pnode->polllist_lock, ipl);

		nwritten += to_write;

		while (to_write-- > 0) {
			pnode->data[pnode->write_head++ % pnode->size] =
			    *cbuf++;
		}

		ke_event_signal(&pnode->readable);
	}

	ke_mutex_release(&pnode->mtx);

	return nwritten;
}

static int
fifofs_getattr(vnode_t *vn, vattr_t *out)
{
	memset(out, 0x0, sizeof(*out));
	out->type = S_IFIFO;
	return 0;
}

static struct vnops fifofs_vnops = {
	.close = fifofs_close,
	.chpoll = fifofs_chpoll,
	.read = fifofs_read,
	.write = fifofs_write,
	.getattr = fifofs_getattr,
};