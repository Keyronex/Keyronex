/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Apr 09 2023.
 */

#include <sys/errno.h>

#include <abi-bits/fcntl.h>

#include "executive/epoll.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
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
	/* (p) to check, (m+p) to set) */
	size_t count;

	/* (m) */
	size_t read_head, write_head;

	/* (p) */
	struct polllist polllist;
	/* (p to check, m+p to set) */
	kevent_t readable, writeable;
};

int
ps_allocfiles(size_t n, int *out)
{
	eprocess_t *eproc = ps_curproc();
	int fds[4], nalloced = 0;
	int r = 0;

	kassert(n < 4);

	ke_wait(&eproc->fd_mutex, "ps_allocfile:eproc->fd_mutex", false, false,
	    -1);
	for (int i = 0; i < elementsof(eproc->files); i++) {
		if (eproc->files[i] == NULL) {
			fds[nalloced++] = i;
			if (nalloced == n)
				break;
		}
	}

	if (n > 0) {
		kdprintf("failed to allocate enough FDs\n");
		r = -EBADF;
	} else {
		for (int i = 0; i < nalloced; i++) {
			eproc->files[fds[i]] = (void *)0xDEADBEEF;
			out[i] = fds[i];
		}
	}

	ke_mutex_release(&eproc->fd_mutex);

	return r;
}

int
sys_pipe(int *out[2])
{
	struct fifonode *pnode;
	int fds[2];
	int r;

	r = ps_allocfiles(2, fds);

	pnode = kmem_alloc(sizeof(*pnode));
	ke_mutex_init(&pnode->mtx);
	pnode->data = kmem_alloc(PGSIZE);
	pnode->size = PGSIZE;
	pnode->count = 0;
	pnode->read_head = pnode->write_head = 0;

	ke_spinlock_init(&pnode->polllist_lock);
	LIST_INIT(&pnode->polllist.pollhead_list);
	ke_event_init(&pnode->readable, false);
	ke_event_init(&pnode->writeable, true);

	kfatal("todo: complete\n");
}

static int
fifofs_chpoll(vnode_t *vn, struct pollhead *ph, enum chpoll_kind kind)
{
	struct fifonode *pnode = (struct fifonode *)vn->data;
	ipl_t ipl = ke_spinlock_acquire(&pnode->polllist_lock);
	int r = 0;

	if (kind == kChPollAdd) {
		if (pnode->read_head != pnode->write_head) {
			ph->revents = EPOLLIN | EPOLLOUT;
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
		goto wait;
	}

	ipl = ke_spinlock_acquire(&pnode->polllist_lock);

	if (pnode->count - nbyte <= 0) {
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

	return nread;
}

static int
fifofs_write(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	struct fifonode *p = (struct fifonode *)vn->data;
	ipl_t ipl;
	size_t nwritten = 0;
	uint8_t *cbuf = buf;

	if (vn->data2 != O_WRONLY)
		return -EINVAL;

wait:
	ke_wait(&p->mtx, "fifofs_write:mtx", false, false, -1);

	while (nwritten < nbyte) {
		size_t to_write = nbyte - nwritten;

		if (p->count == p->size) {
			ke_mutex_release(&p->mtx);
			goto wait;
		}

		ipl = ke_spinlock_acquire(&p->polllist_lock);

		if (p->count + to_write >= p->size) {
			to_write = MIN2(p->size - p->count, to_write);
			ke_event_clear(&p->writeable);
		}

		p->count += to_write;

		ke_spinlock_release(&p->polllist_lock, ipl);

		nwritten += to_write;

		while (to_write-- > 0) {
			p->data[p->write_head++ % p->size] = *cbuf++;
		}
	}

	return nwritten;
}

static struct vnops fifofs_vnops = {
	.chpoll = fifofs_chpoll,
	.read = fifofs_read,
	.write = fifofs_write,
};