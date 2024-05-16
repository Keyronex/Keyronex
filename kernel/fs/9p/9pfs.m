/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun May 12 2024.
 */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 *  License, v. 2.0. If a copy of the MPL was not distributed with this
 *  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*!
 * @file 9pfs.m
 * @brief 9p2000.L driver
 *
 * TODO:
 * - proper FID allocation
 */

#include <fcntl.h>

#include "9pfs.h"
#include "ddk/DKDevice.h"
#include "dev/virtio/VirtIO9pPort.h"
#include "fs/9p/9p_buf.h"
#include "kdk/dev.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kdk/vfs.h"

typedef uint32_t fid_t;

struct ninepfs_state {
	vfs_t vfs;
	uint16_t req_tag;
	fid_t fid_counter;
	RB_HEAD(ninep_node_rb, ninep_node) node_cache;
	kmutex_t node_cache_lock;
	VirtIO9pPort *provider;
};

struct ninep_node {
	RB_ENTRY(ninep_node) rb_entry;
	struct ninep_qid qid;
	fid_t fid, paging_fid;
	vnode_t *vnode; /* ninep_nodes always have an associated vnode */

	kmutex_t rwlock, paging_rwlock;
};

#define PROVIDER ((VirtIO9pPort *)m_provider)
#define VTO9(VNODE) ((struct ninep_node *)(VNODE)->fs_data)
#define VTO9FS(VNODE) ((struct ninepfs_state *)(VNODE)->vfs->vfs_data)

static int counter = 0;
static struct vnode_ops ninep_vnops;

static int64_t
node_cmp(struct ninep_node *x, struct ninep_node *y)
{
	return x->qid.path - y->qid.path;
}

RB_GENERATE(ninep_node_rb, ninep_node, rb_entry, node_cmp);

static fid_t
fid_allocate(struct ninepfs_state *fs)
{
	return fs->fid_counter++;
}

/*!
 * Finds the ninep_node corresponding to a QID if it exists in the cache, adding
 * a reference to its vnode if so, or, if not found, ceeates one.
 * @retval 1 if an extant node was found
 * @retval 0 if a node was created
 */
static int
node_for_qid(struct ninepfs_state *fs, struct ninep_node **out,
    struct ninep_qid qid, fid_t fid)
{
	struct ninep_node key, *found;
	vtype_t vtype;

	key.qid = qid;

	ke_wait(&fs->node_cache_lock, "9p node_for_qid", false, false, -1);

	found = RB_FIND(ninep_node_rb, &fs->node_cache, &key);
	if (found != NULL) {
		/* don't check version, it's just mtime... */
		kassert(
		    found->qid.path == qid.path && found->qid.type == qid.type);
		obj_retain(found->vnode);
		ke_mutex_release(&fs->node_cache_lock);
		return 1;
	}

	found = kmem_alloc(sizeof(struct ninep_node));
	found->qid = qid;
	found->fid = fid;
	found->paging_fid = 0;
	RB_INSERT(ninep_node_rb, &fs->node_cache, found);
	ke_mutex_init(&found->rwlock);
	ke_mutex_init(&found->paging_rwlock);

	switch (qid.type) {
	case 0x80: /* QTDIR */
		vtype = VDIR;
		break;

	case 0x0: /* QTFILE */
		vtype = VREG;
		break;

	default:
		kfatal("Unhandled qid type %x\n", qid.type);
	}

	found->vnode = vnode_new(&fs->vfs, vtype, &ninep_vnops /* ops */,
	    &found->rwlock, &found->paging_rwlock, (uintptr_t)found);

	ke_mutex_release(&fs->node_cache_lock);

	*out = found;

	return 0;
}

static int
fid_clone(struct ninepfs_state *fs, ninep_fid_t fid, ninep_fid_t new_fid)
{
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	int r;

	/* size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s]) */
	buf_in = ninep_buf_alloc("FFh");
	/* size[4] Rwalk tag[2] nwqid[2] nwqid*(wqid[13]) */
	buf_out = ninep_buf_alloc("h");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pWalk;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_addfid(buf_in, new_fid);
	ninep_buf_addu16(buf_in, 0);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pWalk + 1: {
		uint16_t nwnames;
		ninep_buf_getu16(buf_out, &nwnames);
		kassert(nwnames == 0);
		r = 0;
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		kprintf("Failed to clone node id: %d\n", err);
		kassert(err != 0);
		r = -err;
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_out);

	return r;
}

static int
node_make_paging_fid(struct ninepfs_state *fs, struct ninep_node *node)
{
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	ninep_fid_t new_fid;
	int r;

	if (node->paging_fid != 0)
		return 0;

	if (node->vnode->type != VREG && node->vnode->type != VDIR)
		return -1; /* EBADF */

	new_fid = fid_allocate(fs);

	r = fid_clone(fs, node->fid, new_fid);
	kassert(r == 0);

	/* size[4] Tlopen tag[2] fid[4] flags[4] */
	buf_in = ninep_buf_alloc("Fd");
	/* size[4] Rlopen tag[2] qid[13] iounit[4] */
	buf_out = ninep_buf_alloc("Qd");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pLopen;
	ninep_buf_addfid(buf_in, new_fid);
	ninep_buf_addu32(buf_in,
	    node->vnode->type == VDIR ? O_DIRECTORY : O_RDWR);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);

	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pLopen + 1: {
		r = 0;
		node->paging_fid = new_fid;
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		kprintf("Failed to clone node id: %d\n", err);
		kassert(err != 0);
		r = -err;
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_out);

	return r;
}

int
ninep_lookup(vnode_t *dvn, vnode_t **out, const char *name)
{
	struct ninep_node *dn = VTO9(dvn);
	struct ninepfs_state *fs = VTO9FS(dvn);
	fid_t new_fid = fid_allocate(fs);
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	int r;

	/* size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s]) */
	buf_in = ninep_buf_alloc("FFhS64");
	/* size[4] Rwalk tag[2] nwqid[2] nwqid*(wqid[13]) */
	buf_out = ninep_buf_alloc("hQ");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pWalk;
	ninep_buf_addfid(buf_in, dn->fid);
	ninep_buf_addfid(buf_in, new_fid);
	ninep_buf_addu16(buf_in, 1);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pWalk + 1: {
		uint16_t nwqid;
		struct ninep_qid qid;
		struct ninep_node *node;

		ninep_buf_getu16(buf_out, &nwqid);
		kassert(nwqid == 1);
		ninep_buf_getqid(buf_out, &qid);
		ninep_buf_free(buf_out);

		r = node_for_qid(fs, &node, qid, new_fid);
		if (r == 1) {
			kfatal("todo clunk new_fid");
		} else if (r < 0) {
			kfatal("node_for_qid failed?\n");
		}
		*out = node->vnode;

		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		ninep_buf_free(buf_out);
		r = -err;
		kassert(r != 0);
		goto out;
	}

	default: {
		kfatal("9p error\n");
	}
	}

out:
	return r;
}

@implementation NinepFS

- (iop_return_t)dispatchIOP:(iop_t *)iop
{
	iop_frame_t *frame = iop_stack_current(iop), *next_frame;
	struct ninep_node *node;
	struct ninep_buf *buf_in, *buf_out;

	kassert(frame->function == kIOPTypeRead ||
	    frame->function == kIOPTypeWrite);
	kassert(frame->vnode != NULL);
#if 0
	DKDevLog(self,
	    "Dispatching a read request - offset %" PRId64 " length %zu\n",
	    frame->rw.offset, frame->rw.bytes);
#endif

	if (frame->function == kIOPTypeRead) {
		kassert(frame->mdl->write == true);
	} else {
		kassert(frame->mdl->write == false);
	}

	node = VTO9(frame->vnode);
	if (node->paging_fid == 0) {
		int r = node_make_paging_fid(m_state, node);
		kassert(r == 0);
	}

	/* size[4] Tread tag[2] fid[4] offset[8] count[4] */
	buf_in = ninep_buf_alloc("Fld");
	/* size[4] Rread tag[2] count[4] data[count] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = to_leu16(m_state->req_tag++);
	buf_in->data->kind = frame->function == kIOPTypeRead ? k9pRead :
							       k9pWrite;
	ninep_buf_addfid(buf_in, node->paging_fid);
	ninep_buf_addu64(buf_in, frame->rw.offset);
	ninep_buf_addu32(buf_in, frame->rw.bytes);
	ninep_buf_close(buf_in);

	next_frame = iop_stack_initialise_next(iop);
	iop_frame_setup_9p(next_frame, buf_in, buf_out, frame->mdl);

	return kIOPRetContinue;
}

- (iop_return_t)completeIOP:(iop_t *)iop
{
	iop_frame_t *frame = iop_stack_previous(iop);

	kassert(frame->function == kIOPType9p);

	switch (frame->ninep.ninep_out->data->kind) {
	case k9pRead + 1:
	case k9pWrite + 1: {
		uint32_t count;

		ninep_buf_getu32(frame->ninep.ninep_out, &count);
		kassert(count <= iop_stack_current(iop)->rw.bytes);

		ninep_buf_free(frame->ninep.ninep_in);
		ninep_buf_free(frame->ninep.ninep_out);

		return kIOPRetCompleted;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(frame->ninep.ninep_out, &err);
		DKDevLog(self, "Pager I/O got error code %d\n", err);
	}
	default:
		kfatal("9p error\n");
	}
}

- (int)negotiateVersion
{
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	int r = 0;

	buf_in = ninep_buf_alloc("dS8");
	buf_out = ninep_buf_alloc("dS16");

	buf_in->data->tag = to_leu16(-1);
	buf_in->data->kind = k9pVersion;
	ninep_buf_addu32(buf_in, 8288);
	ninep_buf_addstr(buf_in, k9pVersion2000L);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(PROVIDER, buf_in, buf_out, NULL);
	iop_send_sync(iop);

	switch (buf_out->data->kind) {
	case k9pVersion + 1: {
		char *ver;
		uint32_t msize;

		ninep_buf_getu32(buf_out, &msize);
		ninep_buf_getstr(buf_out, &ver);

		DKDevLog(self, "Negotiated 9p version %s, message size %d\n",
		    ver, msize);
		break;
	}

	default: {
		DKDevLog(self, "Bad 9p version.\n");
		r = -1;
	}
	}

	ninep_buf_free(buf_in);
	ninep_buf_free(buf_out);
	iop_free(iop);

	return r;
}

- (int)attach
{
	int r = 0;
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;

	/* size[4] Tattach tag[2] fid[4] afid[4] uname[s] aname[s] n_uname[4] */
	buf_in = ninep_buf_alloc("FFS4S4d");
	/* size[4] Rattach tag[2] qid[13] */
	buf_out = ninep_buf_alloc("Q");

	buf_in->data->tag = to_leu16(m_state->req_tag++);
	buf_in->data->kind = k9pAttach;
	ninep_buf_addfid(buf_in, m_state->fid_counter++);
	ninep_buf_addfid(buf_in, 0);
	ninep_buf_addstr(buf_in, "root");
	ninep_buf_addstr(buf_in, "root");
	ninep_buf_addu32(buf_in, 0);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(PROVIDER, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);

	switch (buf_out->data->kind) {
	case k9pAttach + 1: {
		struct ninep_qid qid;
		struct ninep_node *root_node;

		r = ninep_buf_getqid(buf_out, &qid);
		if (r != 0)
			kfatal("Couldn't get a QID!\n");

		r = node_for_qid(m_state, &root_node, qid, 1);
		kassert(r == 0); /* can't be existing already */

		DKDevLog(self,
		    "Attached, root FID type %d ver %d path %" PRIu64 "\n",
		    qid.type, qid.version, qid.path);

		nc_make_root(&m_state->vfs, root_node->vnode);

		break;
	}

	default: {
		kfatal("9p failure\n");
	}
	}

	ninep_buf_free(buf_in);
	ninep_buf_free(buf_out);

	return r;
}

- (instancetype)initWithProvider:(VirtIO9pPort *)provider
{
	int r;

	self = [super initWithProvider:provider];

	kmem_asprintf(obj_name_ptr(self), "virtio-9p-%u", counter++);

	m_state = kmem_alloc(sizeof(struct ninepfs_state));
	m_state->fid_counter = 1;
	m_state->req_tag = 0;
	m_state->provider = PROVIDER;
	RB_INIT(&m_state->node_cache);
	ke_mutex_init(&m_state->node_cache_lock);
	m_state->vfs.device = self;
	m_state->vfs.vfs_data = (uintptr_t)m_state;

	r = [self negotiateVersion];
	if (r != 0)
		kfatal("Failed to negotiate version.\n");

	r = [self attach];
	if (r != 0)
		kfatal("Failed to attach\n");

	return self;
}

+ (BOOL)probeWithProvider:(VirtIO9pPort *)provider
{
	return [[self alloc] initWithProvider:provider] != nil;
}

@end

static struct vnode_ops ninep_vnops = {
	.lookup = ninep_lookup,
};