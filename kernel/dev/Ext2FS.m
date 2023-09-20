#include "Ext2FS.h"
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
 * A block buffer.
 */
typedef struct buf {
        /*! Lock on contents. */
	kmutex_t mutex;
	/*! Owner. */
	struct bufhead *head;
	/*! Buffer tree linkage. */
	RB_ENTRY(buf) rb_link;
	/*! Whether the buf is busy. If so, it can't be put back to disk. */
        bool busy;
	/*! FS logical block number that the buf represents. */
	io_blkoff_t blkno;
	/*! Pointer to the actual data. */
	char *data;
} buf_t;

typedef RB_HEAD(buftree, buf) buftree_t;

typedef struct bufhead {
	void *device;
	size_t block_size;
	buftree_t tree;
} bufhead_t;

struct inode {
	vnode_t vnode;
	/*! linkage in inum-to-inode RB */
	RB_ENTRY(inode) inum_rb_entry;
	/*! common part with disk inode */
	union {
		struct ext2_inode ic;
		struct ext2_inode_large ic_large;
	};
};

struct ext2fs_state {
	struct ext2_super_block *sb;
	/*! locks inode number to vnode queue */
	kmutex_t queue_lock;

	bufhead_t bufhead;
};

static int buf_cmp(buf_t *a, buf_t *b);

RB_PROTOTYPE_STATIC(buftree, buf, rb_link, buf_cmp);
RB_GENERATE_STATIC(buftree, buf, rb_link, buf_cmp);

static int counter = 0;

/* Compare two buffers' block number - for use by the RB tree code. */
static int
buf_cmp(buf_t *a, buf_t *b)
{
        return a->blkno - b->blkno;
}

void bufhead_init(bufhead_t *head, DKDevice *device, size_t block_size)
{
	RB_INIT(&head->tree);
	head->device = device;
	head->block_size = block_size;
}

buf_t *
bread(bufhead_t *head, io_blkoff_t block)
{
	buf_t *buf, key;

	key.blkno = block;
	if ((buf = RB_FIND(buftree, &head->tree, &key)) != NULL) {
		ke_wait(&buf->mutex, "bread", false, false, -1);
		return buf;
	}

	/*! create and read in */
	buf = kmem_alloc(sizeof(buf_t));
	buf->blkno = block;
	buf->head = head;
	ke_mutex_init(&buf->mutex);
	ke_wait(&buf->mutex, "bread", false, false, -1);
	buf->data = kmem_alloc(head->block_size);
	RB_INSERT(buftree, &head->tree, buf);

	/*! now read */
	vm_mdl_t *mdl = vm_mdl_create(buf->data, head->block_size);
	iop_t *iop = iop_new_read(head->device, mdl, head->block_size,
	    block * head->block_size);
	iop_send_sync(iop);
	iop_free(iop);

	return buf;
}

void
buf_release(buf_t *buf)
{
	if (buf == NULL)
		return;
}

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

/*! load an inode from disk. queue_lock shall be locked.  */
- (struct inode *)loadInode:(uint64_t)inum
{
	struct inode *inode;
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

	inode = kmem_alloc(sizeof(*inode));
	inobuf = bread(&state->bufhead, inoblock);
	memcpy(&inode->ic_large, inobuf->data + inooff, fs_ino_size(state));

	return inode;
}

- (void)readdir:(vnode_t*)vnode from:(size_t)start
{
	struct inode *inode = [self loadInode:EXT2_ROOT_INO];
	buf_t *buf = NULL;
	size_t off;

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
		kprintf("<%s>: ino %d (type %d)\n", name,
		    le32_to_native(de->inode), ext2fs_dirent_file_type(de));

		off += le16_to_native(de->rec_len);
	}
}

- initWithVolume:(DKDevice *)volume
       blockSize:(size_t)blockSize
      blockCount:(io_blksize_t)blockCount
      superBlock:(struct ext2_super_block *)sb
{
	self = [super initWithProvider:volume];
	state = kmem_alloc(sizeof(struct ext2fs_state));
	state->sb = sb;
	bufhead_init(&state->bufhead, volume,
	    1024 << le32_to_native(state->sb->s_log_block_size));
	kmem_asprintf(obj_name_ptr(self), "ext2-%d", counter++);

	[self readdir:0];

	return self;
}

@end
