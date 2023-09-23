#include "Ext2FS.h"
#include "buf.h"
#include "ddk/ext2_fs.h"
#include "ddk/ext2_inl.h"
#include "kdk/dev.h"
#include "kdk/endian.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"

/*!
 * Locking Protocol
 * ================
 *
 * An I-node's type never changes as long as it exists.
 *
 * per-fs:
 * - ext2_state::queue_lock => guards I-node number to vnode lookup tree
 * - ext2_state::fs_lock => guards superblock, cylinder groups, as such guards
 *   allocation of inodes, blocks.
 * per-inode:
 * - ext2_inode::rwlock => guards read/write of data (including directory
 *   inodes' dirents); as such also guards the I-node's block ptrs.
 * - ext2_inode::metadata_rwlock => guards I-node fields other than block ptrs.
 *
 * ordering: ext2_inode::rwlock -> ext2_inode::metadata_rwlock
 *  -> ext2_state::queue_lock -> ext2_state::fs_lock
 *
 * I-node allocation
 * =================
 *
 * This one should be quite simple because, in principle, the inode shouldn't be
 * referrable until it has been allocated. The corner case would be something
 * like NFS bringing a stale file handle. But that can probably be checked
 * against generation count.
 */

#define VTOI(VNODE) ((struct inode *)(VNODE)->fs_data)

RB_HEAD(ext2_inode_rb, inum_key);

enum ext2_i_mode_type {
	kExt2IModeTypeMask = 0170000,
	kExt2IModeFifo = 0010000,
	kExt2IModeChr = 0020000,
	kExt2IModeDir = 0040000,
	kExt2IModeBlk = 0060000,
	kExt2IModeReg = 0100000,
	kExt2IModeLnk = 0120000,
	kExt2IModeSock = 0140000,
};

struct inum_key {
	/*! linkage in inum-to-inode RB */
	RB_ENTRY(inum_key) inum_rb_entry;
	/*! inode number */
	uint32_t inum;
};

struct inode {
	struct inum_key key;
	/*! associated vnode */
	vnode_t *vnode;
	/*! guards read/write; will become an actual rwlock sometime */
	kmutex_t rwlock;
	/*! guards inode fields */
	kmutex_t metadata_rwlock;
	/*! is it being read in? */
	bool busy;
	/*! common part with disk inode */
	union {
		struct ext2_inode ic;
		struct ext2_inode_large ic_large;
	};
};

struct ext2fs_state {
	/*! locks inode number to vnode queue */
	kmutex_t queue_lock;
	/*! locks superblock, cylinder groups */
	kmutex_t fs_lock;
	/*! buffer cache */
	bufhead_t bufhead;
	/*! superblock in-memory */
	struct ext2_super_block *sb;
	/*! inode number to inode struct lookup */
	struct ext2_inode_rb inode_rb;
};

static int counter = 0;

static inline int32_t
inum_key_cmp(struct inum_key *x, struct inum_key *y)
{
	return (int32_t)x->inum - y->inum;
}

RB_PROTOTYPE(ext2_inode_rb, inum_key, inum_rb_entry, inum_key_cmp);
RB_GENERATE(ext2_inode_rb, inum_key, inum_rb_entry, inum_key_cmp);

@implementation Ext2FS

+ (BOOL)probeWithVolume:(DKDevice *)volume
	      blockSize:(size_t)blockSize
	     blockCount:(io_blksize_t)blockCount;
{
	vm_mdl_t *mdl = vm_mdl_buffer_alloc(1);
	iop_t *iop = iop_new_read(volume, mdl, sizeof(struct ext2_super_block),
	    blockSize * 2);
	iop_send_sync(iop);
	iop_free(iop);

	struct ext2_super_block *sb = (void*)P2V(vm_mdl_paddr(mdl, 0));
	if (le16_to_native(sb->s_magic) == EXT2_SUPER_MAGIC) {
		char volid[17] = { 0 };
		memcpy(volid, sb->s_volume_name, 16);
		kprintf("VolID: %s\n", volid);
		[[self alloc] initWithVolume:volume
				   blockSize:blockSize
				  blockCount:blockCount
				  superBlock:sb];
		return YES;
	} else
		return NO;
}

static struct ext2_group_desc *
cylgroup_load(struct ext2fs_state *state, uint32_t cgnum, buf_t **buf_out)
{
	size_t loc = (state->bufhead.block_size > 1024 ?
			     state->bufhead.block_size :
			     state->bufhead.block_size * 2) +
	    cgnum * sizeof(struct ext2_group_desc);
	buf_t *buf = bread(&state->bufhead, loc / state->bufhead.block_size);
	*buf_out = buf;
	return (void *)(buf->data + loc % state->bufhead.block_size);
}

static buf_t *
inode_bread(struct ext2fs_state *state, struct inode *inode, io_blkoff_t block)
{
	if (block < EXT2_NDIR_BLOCKS) {
		return bread(&state->bufhead,
		    le32_to_native(inode->ic.i_block[block]));
	} else {
		kfatal("Implement me\n");
	}
}

static uint16_t
fs_ino_size(struct ext2fs_state *state)
{
	return le16_to_native(state->sb->s_inode_size);
}

static inline vtype_t
ext2_i_mode_to_vtype(le16_t i_mode)
{
	switch (le16_to_native(i_mode) & kExt2IModeTypeMask) {
	case kExt2IModeFifo:
		return VFIFO;
	case kExt2IModeChr:
		return VCHR;
	case kExt2IModeDir:
		return VDIR;
	case kExt2IModeBlk:
		return VCHR;
	case kExt2IModeReg:
		return VREG;
	case kExt2IModeLnk:
		return VLNK;
	case kExt2IModeSock:
		return VSOCK;
	default:
		kfatal("unexpected inode type\n");
	}
}

/*! load an inode from disk. */
static void
load_inode(struct ext2fs_state *state, struct inode *inode, uint32_t inum)
{
	uint32_t inodes_per_group = le32_to_native(
	    state->sb->s_inodes_per_group);
	size_t cgnum = (inum - 1) / inodes_per_group;
	size_t offset = (inum - 1) % inodes_per_group;
	buf_t *cgbuf, *inobuf;
	struct ext2_group_desc *gd;
	io_blkoff_t inoblock;
	io_off_t inooff;

	gd = cylgroup_load(state, cgnum, &cgbuf);

	inoblock = le32_to_native(gd->bg_inode_table) +
	    (offset * fs_ino_size(state)) / state->bufhead.block_size;
	inooff = (offset * fs_ino_size(state)) % state->bufhead.block_size;
	buf_release(cgbuf);

	inobuf = bread(&state->bufhead, inoblock);
	memcpy(&inode->ic_large, inobuf->data + inooff, fs_ino_size(state));
	ext2_i_mode_to_vtype(inode->ic_large.i_mode);
	buf_release(inobuf);
}

static vnode_t *
vnode_for_inum(struct ext2fs_state *fs, uint32_t inum, kmutex_t *to_release)
{
	struct inum_key key, *found_key;
	struct inode *inode;

	key.inum = inum;

	ke_wait(&fs->queue_lock, "ext2_vnode_for_inum", false, false, -1);
	if (to_release != NULL)
		ke_mutex_release(to_release);
	found_key = RB_FIND(ext2_inode_rb, &fs->inode_rb, &key);
	if (found_key != NULL) {
		struct inode *inode = (void *)found_key;
		vnode_t *vnode;
		if (inode->busy)
			kfatal("wait here\n");
		vnode = obj_retain(inode->vnode);
		ke_mutex_release(&fs->queue_lock);
		return vnode;
	}

	inode = kmem_alloc(sizeof(*inode));
	inode->key.inum = inum;
	inode->busy = true;
	RB_INSERT(ext2_inode_rb, &fs->inode_rb, &inode->key);
	ke_mutex_release(&fs->queue_lock);

	load_inode(fs, inode, inum);
	ke_mutex_init(&inode->rwlock);
	inode->vnode = vnode_alloc();
	inode->vnode->type = inode->busy = false;
	inode->vnode->fs_data = (uintptr_t)inode;

	return inode->vnode;
}

/*!
 * @pre fs->fs_lock locked
 */
#if 0
static io_blkoff_t
alloc_block(struct ext2fs_state *fs)
{
	for (size_t cgnum = 0; cgnum < le32_to_native(fs->sb->s_blocks_count);
	     cgnum++) {
		buf_t *cgbuf, *bitmap_buf;
		struct ext2_group_desc *gd;

		gd = cylgroup_load(fs, cgnum, &cgbuf);
		if (gd->bg_free_blocks_count == 0) {
			buf_release(cgbuf);
			continue;
		}

		bitmap_buf = bread(&fs->bufhead, le32_to_native(gd->bg_block_bitmap) * fs->bufhead.block_size);

	}

	kfatal("failed to allocate block\n");
}
#endif

static io_blkoff_t
cg_alloc_block(struct ext2fs_state *fs, uint32_t cgnum)
{
	io_blkoff_t block_allocated = -1;
	buf_t *cgbuf, *bitmap_buf;
	struct ext2_group_desc *gd;
	uint8_t *bitmap_data;
	size_t bitmap_nbytes = le32_to_native(fs->sb->s_blocks_per_group) / 8;

	kassert(bitmap_nbytes == fs->bufhead.block_size);

	gd = cylgroup_load(fs, cgnum, &cgbuf);
	if (le16_to_native(gd->bg_free_blocks_count) == 0) {
		buf_release(cgbuf);
		return -1;
	}

	bitmap_buf = bread(&fs->bufhead, le32_to_native(gd->bg_block_bitmap));
	bitmap_data = (uint8_t *)bitmap_buf->data;

	for (io_blkoff_t byte_off = 0; byte_off < bitmap_nbytes; byte_off++) {
		uint8_t byte = bitmap_data[byte_off];

		if (byte == 0xff)
			continue;

		for (int bit_off = 0; bit_off < 8; bit_off++) {
			if ((byte & (1 << bit_off)))
				continue;

			io_blkoff_t block_num = cgnum *
				le32_to_native(fs->sb->s_blocks_per_group) +
			    byte_off * 8 + bit_off;

			bitmap_data[byte_off] |= (1 << bit_off);

			gd->bg_free_blocks_count = native_to_le16(
			    le16_to_native(gd->bg_free_blocks_count) - 1);
			fs->sb->s_free_blocks_count = native_to_le32(
			    le32_to_native(fs->sb->s_free_blocks_count) - 1);

			block_allocated = block_num;
			goto finish;
		}
	}

finish:
	buf_release(bitmap_buf);
	buf_release(cgbuf);
	return block_allocated;
}

static io_blkoff_t
alloc_block(struct ext2fs_state *fs)
{
	for (size_t cgnum = 0; cgnum < le32_to_native(fs->sb->s_blocks_count);
	     cgnum++) {
		io_blkoff_t block = cg_alloc_block(fs, cgnum);
		if (block != -1)
			return block;
	}

	return -1;
}

- (void)readdir:(vnode_t*)vnode from:(size_t)start
{
	struct inode *inode = VTOI(vnode);
	buf_t *buf = NULL;
	size_t off;

	ke_wait(&inode->rwlock, "lookup:inode->rwlock", false, false, -1);

	off = ROUNDDOWN(start, state->bufhead.block_size);
	while (off < le32_to_native(inode->ic.i_size)) {
		struct ext2_dir_entry *de;
		char name[64] = { 0 };

		if (off % state->bufhead.block_size == 0) {
			buf_release(buf);
			buf = inode_bread(state, inode,
			    off / state->bufhead.block_size);
		}

		de = (void *)(buf->data + (off % state->bufhead.block_size));
		memcpy(name, de->name, ext2fs_dirent_name_len(de));
		kprintf("at %zu: <%s>: ino %d (type %d)\n", off, name,
		    le32_to_native(de->inode), ext2fs_dirent_file_type(de));

		off += le16_to_native(de->rec_len);
	}

	ke_mutex_release(&inode->rwlock);
	buf_release(buf);
}

- (vnode_t *)lookup:(const char *)path inDirectoryVNode:(vnode_t *)dvn
{
	struct inode *inode = VTOI(dvn);
	buf_t *buf = NULL;
	size_t off = 0;
	size_t path_len = strlen(path);

	ke_wait(&inode->rwlock, "lookup:inode->rwlock", false, false, -1);

	while (off < le32_to_native(inode->ic.i_size)) {
		struct ext2_dir_entry *de;

		if (off % state->bufhead.block_size == 0) {
			buf_release(buf);
			buf = inode_bread(state, inode,
			    off / state->bufhead.block_size);
		}

		de = (void *)(buf->data + (off % state->bufhead.block_size));

		if (ext2fs_dirent_name_len(de) == path_len &&
		    strncmp(de->name, path, path_len) == 0) {
			buf_release(buf);
			return vnode_for_inum(state, le32_to_native(de->inode),
			    &inode->rwlock);
		}

		off += le16_to_native(de->rec_len);
	}

	ke_mutex_release(&inode->rwlock);
	buf_release(buf);

	return NULL;
}

- initWithVolume:(DKDevice *)volume
       blockSize:(size_t)blockSize
      blockCount:(io_blksize_t)blockCount
      superBlock:(struct ext2_super_block *)sb
{
	self = [super initWithProvider:volume];
	state = kmem_alloc(sizeof(struct ext2fs_state));
	state->sb = sb;
	ke_mutex_init(&state->fs_lock);
	ke_mutex_init(&state->queue_lock);
	bufhead_init(&state->bufhead, volume,
	    1024 << le32_to_native(state->sb->s_log_block_size));
	kmem_asprintf(obj_name_ptr(self), "ext2-%d", counter++);

	vnode_t *root_vn = vnode_for_inum(state, EXT2_ROOT_INO, NULL);
	vnode_t *r1 = [self lookup:"lost+found" inDirectoryVNode:root_vn];
	kprintf("VN: %p; in? %u\n", r1,
	    r1 != NULL ? (VTOI(r1)->key.inum) : 8888);

	size_t block1 = alloc_block(state), block2 = alloc_block(state),
	       block3 = alloc_block(state), block4 = alloc_block(state);

	kprintf("Alloc blocks: %zu, %zu, %zu, %zu\n", block1, block2, block3,
	    block4);

	[self readdir:root_vn from:0];

	for (;;)
		;

	return self;
}

@end
