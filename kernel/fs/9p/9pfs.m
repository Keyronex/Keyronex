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

#include <netinet/in.h>

#include <abi-bits/socket.h>
#include <fcntl.h>

#include "9pfs.h"
#include "ddk/DKDevice.h"
#include "dev/9pSockTransport.h"
#include "dev/virtio/VirtIO9pPort.h"
#include "fs/9p/9p_buf.h"
#include "kdk/dev.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vfs.h"
#include "lwip/ip_addr.h"
#include "net/keysock_dev.h"

typedef uint32_t fid_t;

struct ninep_node {
	RB_ENTRY(ninep_node) rb_entry;
	struct ninep_qid qid;
	fid_t fid, paging_fid;
	vnode_t *vnode; /* ninep_nodes always have an associated vnode */

	vattr_t vattr; /*!< cached attributes */

	kmutex_t rwlock, paging_rwlock;
};

struct ninepfs_state {
	vfs_t *vfs;
	uint16_t req_tag;
	fid_t fid_counter;
	RB_HEAD(ninep_node_rb, ninep_node) node_cache;
	kmutex_t node_cache_lock;
	struct ninep_node *root_node;
	VirtIO9pPort *provider;
};

#define PROVIDER ((VirtIO9pPort *)m_provider)
#define VTO9(VNODE) ((struct ninep_node *)(VNODE)->fs_data)
#define VTO9FS(VNODE) ((struct ninepfs_state *)(VNODE)->vfs->vfs_data)

static int counter = 0;
static struct vnode_ops ninep_vnops;

static int do_getattr(struct ninepfs_state *fs, fid_t fid, vattr_t *vattr);

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
	int r;

	key.qid = qid;

	ke_wait(&fs->node_cache_lock, "9p node_for_qid", false, false, -1);

	found = RB_FIND(ninep_node_rb, &fs->node_cache, &key);
	if (found != NULL) {
		/* don't check version, it's just mtime... */
		kassert(
		    found->qid.path == qid.path && found->qid.type == qid.type);
		vn_retain(found->vnode);
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

	found->vnode = vnode_new(fs->vfs, vtype, &ninep_vnops /* ops */,
	    &found->rwlock, &found->paging_rwlock, (uintptr_t)found);

	ke_wait(&found->rwlock, "", false, false, -1);

	ke_mutex_release(&fs->node_cache_lock);

	r = do_getattr(fs, fid, &found->vattr);

	/* todo: mark the vnode bad so waiters can see... */
	if (r != 0)
		kfatal("node_for_qid: getattr fail\n");

	ke_mutex_release(&found->rwlock);

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

static int
do_getattr(struct ninepfs_state *fs, fid_t fid, vattr_t *vattr)
{
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;

	uint64_t valid;
	struct ninep_qid qid;
	uint64_t nlink, blocks;

	int r;

	/* size[4] Tgetattr tag[2] fid[4] request_mask[8] */
	buf_in = ninep_buf_alloc("Fl");
	/*
	 * size[4] Rgetattr tag[2] valid[8] qid[13] mode[4] uid[4] gid[4]
	 *       nlink[8] rdev[8] size[8] blksize[8] blocks[8]
	 *       atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8]
	 *       ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8]
	 *       gen[8] data_version[8]
	 */
	buf_out = ninep_buf_alloc("lQdddlllllllllllllll");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pGetattr;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_addu64(buf_in, k9pGetattrBasic);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pGetattr + 1:
		break;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		ninep_buf_free(buf_out);
		r = -err;
		kassert(r != 0);
		kfatal("Fucked it up!\n");
		break;
	}

	default:
		kfatal("Unexpected ninep result")
	}

	ninep_buf_getu64(buf_out, &valid);
	kassert(valid == k9pGetattrBasic);
	ninep_buf_getqid(buf_out, &qid);
	ninep_buf_getu32(buf_out, &vattr->mode);
	ninep_buf_getu32(buf_out, &vattr->uid);
	ninep_buf_getu32(buf_out, &vattr->gid);
	ninep_buf_getu64(buf_out, &nlink);
	ninep_buf_getu64(buf_out, &vattr->rdev);
	ninep_buf_getu64(buf_out, &vattr->size);
	ninep_buf_getu64(buf_out, &vattr->blocksize);
	ninep_buf_getu64(buf_out, &blocks);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr->atim.tv_sec);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr->atim.tv_nsec);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr->mtim.tv_sec);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr->mtim.tv_nsec);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr->ctim.tv_sec);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr->ctim.tv_nsec);

	vattr->fileid = qid.path;
	vattr->type = mode_to_vtype(vattr->mode);
	vattr->nlink = nlink;
	vattr->disksize = blocks * vattr->blocksize;

	return 0;
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

void
ninep_inactive(vnode_t *dvn)
{
	kfatal("Implement me\n");
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
	buf_in = ninep_buf_alloc("FFS4S44d");
	/* size[4] Rattach tag[2] qid[13] */
	buf_out = ninep_buf_alloc("Q");

	buf_in->data->tag = to_leu16(m_state->req_tag++);
	buf_in->data->kind = k9pAttach;
	ninep_buf_addfid(buf_in, m_state->fid_counter++);
	ninep_buf_addfid(buf_in, ~0);
	ninep_buf_addstr(buf_in, "root");
	ninep_buf_addstr(buf_in,
	    "/ws/Projects/Keylite/build/amd64/system-root");
	ninep_buf_addu32(buf_in, 0);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(PROVIDER, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);

	switch (buf_out->data->kind) {
	case k9pAttach + 1: {
		struct ninep_qid qid;

		r = ninep_buf_getqid(buf_out, &qid);
		if (r != 0)
			kfatal("Couldn't get a QID!\n");

		r = node_for_qid(m_state, &m_state->root_node, qid, 1);
		kassert(r == 0); /* can't be existing already */

		DKDevLog(self,
		    "Attached, root FID type %d ver %d path %" PRIu64 "\n",
		    qid.type, qid.version, qid.path);

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

- (instancetype)initWithProvider:(VirtIO9pPort *)provider vfs:(vfs_t *)vfs
{
	int r;

	self = [super initWithProvider:provider];

	kmem_asprintf(obj_name_ptr(self), "9pfs-%u", counter++);

	m_state = kmem_alloc(sizeof(struct ninepfs_state));
	m_state->fid_counter = 1;
	m_state->req_tag = 0;
	m_state->provider = PROVIDER;
	RB_INIT(&m_state->node_cache);
	ke_mutex_init(&m_state->node_cache_lock);
	m_state->vfs = vfs;
	vfs->device = self;
	vfs->vfs_data = (uintptr_t)m_state;
	vfs->file_refcnt = 0;
	ke_spinlock_init(&vfs->vnode_list_lock);
	TAILQ_INIT(&vfs->vnode_list);

	r = [self negotiateVersion];
	if (r != 0)
		kfatal("Failed to negotiate version.\n");

	r = [self attach];
	if (r != 0)
		kfatal("Failed to attach\n");

	[self registerDevice];
	DKLogAttach(self);

	return self;
}

+ (BOOL)probeWithProvider:(VirtIO9pPort *)provider
{
	return [[self alloc] initWithProvider:provider] != nil;
}

- (vnode_t *)root
{
	return m_state->root_node->vnode;
}

@end

static int ninep_sync(vfs_t *vfs)
{
	return 0;
}

struct mount_args {
	enum {
		kTCPTrans,
		kVirtIOTrans,
	} transport;
	const char *server;
	const char *aname;
};

typedef void (*kv_callback)(const char *key, const char *value, void *userdata);

void
parse_mount_args(const char *input, kv_callback callback, void *userdata)
{
	char *buffer = strdup(input);
	char *saveptr1;
	char *pair = strtok_r(buffer, ",", &saveptr1);
	while (pair != NULL) {
		char *saveptr2;
		char *key = strtok_r(pair, "=", &saveptr2);
		if (key != NULL) {
			char *value = strtok_r(NULL, "=", &saveptr2);
			callback(key, value, userdata);
		}

		pair = strtok_r(NULL, ",", &saveptr1);
	}
}

static void
mount_arg(const char *key, const char *val, void *data)
{
	struct mount_args *args = data;

	if (strcmp(key, "trans") == 0) {
		if (strcmp(val, "virtio") == 0)
			args->transport = kVirtIOTrans;
		else if (strcmp(val, "tcp") == 0)
			args->transport = kTCPTrans;
		else
			kfatal("unknown 9p transport %s\n", val);

	} else if (strcmp(key, "server") == 0) {
		args->server = val;
	} else if (strcmp(key, "aname") == 0) {
		args->aname = val;
	} else {
		kfatal("unknown 9p argument key %s\n", key);
	}
}

static int
ninep_mount(namecache_handle_t over, const char *argstr)
{
	struct mount_args args;
	vfs_t *vfs;
	id port;
	NinepFS *fs;

	parse_mount_args(argstr, mount_arg, &args);

	if (args.aname == NULL || args.server == NULL)
		kfatal("Missing 9p arguments.\n");

	vfs = kmem_alloc(sizeof(vfs_t));

	if (args.transport == kVirtIOTrans) {
		port = [VirtIO9pPort forTag:args.server];
		if (port == NULL)
			kfatal("No 9p port for tag %s\n", args.server);
	} else {
		iop_t *iop = iop_new(keysock_dev);
		struct sockaddr_storage sa;
		struct sockaddr_in *sin = (void *)&sa;
		struct socknode *sock;
		int r;

		struct socknode *new_tcpnode(void);
		sock = new_tcpnode();

		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = __builtin_bswap32(0x0a000202);
		sin->sin_port = __builtin_bswap16(564);

		iop->stack[0].vnode = (void *)sock;
		iop->stack[0].dev = keysock_dev;
		iop->stack[0].function = kIOPTypeConnect;
		iop->stack[0].mdl = NULL;
		iop->stack[0].connect.sockaddr = &sa;

		iop_send_sync(iop);

		(void)r;

		port = [[Socket9pPort alloc] initWithConnectedSocket:sock];
	}

	fs = [[NinepFS alloc] initWithProvider:port vfs:vfs];

	nc_make_root(vfs, [fs root]);

	return 0;
}

static struct vnode_ops ninep_vnops = {
	.lookup = ninep_lookup,
	.inactive = ninep_inactive,
};

struct vfs_ops ninep_vfsops = {
	.mount = ninep_mount,
	.sync = ninep_sync,
};
