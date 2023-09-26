#include "DOSFS.h"
#include "dev/buf.h"
#include "dev/dosfs_var.h"
#include "dev/safe_endian.h"
#include "kdk/endian.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/tree.h"
#include "kdk/vfs.h"

#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

#define VTOD(VNODE) ((struct dos_node *)(VNODE)->fs_data)

struct dosfs_state {
	/*! buffer cache */
	bufhead_t bufhead;
	/*! in-memory bpb */
	struct dos_bpb *bpb;
	/*! what type of FAT? */
	enum dosfs_fs_type {
		kFAT12,
		kFAT16,
		kFAT32,
	} type;
	/*! file allocation table size */
	size_t FATSz;
	/*! count of sectors in root directory; only for fat12/16 */
	size_t RootDirSectors;
	/*! first sector of the root directory */
	size_t FirstRootDirSector;
	/*! first sector of the data region */
	size_t FirstDataSector;
	/*! bytes per cluster */
	size_t bytes_per_cluster;
	/*! root directory vnode */
	vnode_t *root;
#if 0
	size_t TotSec;
	size_t DataSec;
#endif
};

RB_HEAD(dos_node_name_rb, dos_node_namekey);

struct dos_node_namekey {
	RB_ENTRY(dos_node_namekey) entry;
	char *filename;
	uint32_t hash;
};

struct dos_node {
	/*! key for rb tree */
	struct dos_node_namekey key;
	/*! vnode counterpart */
	vnode_t *vnode;
	/*! the parent directory of this node (or NULL if root). retained */
	vnode_t *parent;
	/*! directory entries in this directory, if it is a dir */
	struct dos_node_name_rb entries_rb;
	io_blkoff_t first_cluster;
	bool is_root;
};

static int
dos_node_namecmp(struct dos_node_namekey *a, struct dos_node_namekey *b)
{
	if (a->hash < b->hash)
		return -1;
	else if (a->hash > b->hash)
		return 1;
	else
		return strcmp(a->filename, b->filename);
}

RB_GENERATE_STATIC(dos_node_name_rb, dos_node_namekey, entry, dos_node_namecmp);

@implementation DOSFS

static buf_t *
dosnode_bread(struct dosfs_state *fs, struct dos_node *dosnode,
    io_blkoff_t cluster)
{
	if (fs->type != kFAT32 && dosnode->is_root) {
		kassert(cluster < fs->RootDirSectors / fs->bpb->BPB_SecPerClus);
		return bread(&fs->bufhead,
		    fs->FirstRootDirSector + cluster * fs->bpb->BPB_SecPerClus);
	}
	kfatal("Implement me\n");
}

size_t
ucs2_to_utf8(leu16_t *ucs2, char *utf8, size_t ucs2_len, bool *terminated)
{
	size_t utf8_index = 0;
	*terminated = false;

	for (size_t i = 0; i < ucs2_len; i++) {
		uint16_t ucs2_char = from_leu16(ucs2[i]);

		if (ucs2_char <= 0x7F) {
			utf8[utf8_index++] = (char)ucs2_char;
		} else if (ucs2_char == 0x0) {
			*terminated = true;
			break;
		} else {
			utf8[utf8_index++] = '?';
		}
	}

	return utf8_index;
}

void
readDir(struct dosfs_state *fs, vnode_t *vn, io_off_t offset)
{
	char lfn_str[260] = { 0 };
	size_t lfn_str_max = 0;
	io_off_t disk_offset;
	buf_t *buf = NULL;
	struct dos_node *dnode = VTOD(vn);

	kassert(offset % 32 == 0x0);

	if (dnode->is_root && offset < 64) {
		/* fake up . and .. dirents */
		kprintf(".\n..\n");
		offset = 64;
	}

	/* if offset > size of the directory, return */

	while (true) {
		size_t clus_dir;
		struct Dir *dir;

		if (dnode->is_root)
			disk_offset = offset - 64;

		clus_dir = (disk_offset / sizeof(struct Dir)) %
		    fs->bytes_per_cluster;

		if (buf == NULL || clus_dir == 0) {
			buf_release(buf);
			buf = dosnode_bread(fs, dnode,
			    offset / fs->bytes_per_cluster);
		}

		dir = &((struct Dir *)buf->data)[clus_dir];

		if (dir->DIR_Name[0] == 0x0)
			break;
		else if (dir->DIR_Name[0] == 0xe5) {
			offset += sizeof(struct Dir);
			continue;
		} else if (dir->DIR_Attr == ATTR_LONG_NAME) {
			struct LongDir *ldir = (void *)dir;
			io_off_t ldir_off = (ldir->LDIR_Ord & 0x3f) - 1;
			io_off_t lfn_off = 0;
			bool terminated = false;

			lfn_off += ucs2_to_utf8(ldir->LDIR_Name1,
			    lfn_str + ldir_off * 13,
			    sizeof(ldir->LDIR_Name1) / sizeof(leu16_t),
			    &terminated);
			if (terminated)
				goto finish_lfn;

			lfn_off += ucs2_to_utf8(ldir->LDIR_Name2,
			    lfn_str + lfn_off + ldir_off * 13,
			    sizeof(ldir->LDIR_Name2) / sizeof(leu16_t),
			    &terminated);
			if (terminated)
				goto finish_lfn;

			lfn_off += ucs2_to_utf8(ldir->LDIR_Name3,
			    lfn_str + lfn_off + ldir_off * 13,
			    sizeof(ldir->LDIR_Name3) / sizeof(leu16_t),
			    &terminated);
			if (terminated)
				goto finish_lfn;

		finish_lfn:
			lfn_str_max = MAX2(lfn_str_max,
			    lfn_off + ldir_off * 13);
			offset += sizeof(struct Dir);
			continue;
		}

		lfn_str[lfn_str_max] = '\0';
		kprintf("Dirent: Name %s; LFN <%s>\n", dir->DIR_Name, lfn_str);
		lfn_str[0] = '\0';
		lfn_str_max = 0;
		offset += sizeof(struct Dir);
	}

	buf_release(buf);
}

static vnode_t *
dosfs_vnode_make(vnode_t *parent, const char *name)
{
	struct dos_node *node = kmem_alloc(sizeof(*node));
	if (name == NULL || parent == NULL) {
		kassert(name == NULL && parent == NULL);
		node->is_root = true;
	}
	node->vnode = vnode_alloc();
	node->vnode->type = false;
	node->vnode->fs_data = (uintptr_t)node;
	return node->vnode;
}

+ (BOOL)probeWithVolume:(DKDevice *)volume
	      blockSize:(size_t)blockSize
	     blockCount:(io_blksize_t)blockCount;
{
	struct dosfs_state *fs;
	vm_mdl_t *mdl = vm_mdl_buffer_alloc(1);
	iop_t *iop = iop_new_read(volume, mdl, 512, 0);
	iop_send_sync(iop);
	iop_free(iop);

	struct dos_bpb *bpb = (void *)P2V(vm_mdl_paddr(mdl, 0));
	size_t TotSec, DataSec;

	fs = kmem_alloc(sizeof(*fs));

	fs->bpb = bpb;
	fs->bytes_per_cluster = (from_leu16(fs->bpb->BPB_BytsPerSec) *
	    fs->bpb->BPB_SecPerClus);
	fs->RootDirSectors = ((from_leu16(bpb->BPB_RootEntCnt) * 32) +
				 (from_leu16(bpb->BPB_BytsPerSec) - 1)) /
	    from_leu16(bpb->BPB_BytsPerSec);

	bufhead_init(&fs->bufhead, volume, from_leu16(bpb->BPB_BytsPerSec));

	if (bpb->BPB_FATSz16.value != 0)
		fs->FATSz = from_leu16(bpb->BPB_FATSz16);
	else
		fs->FATSz = from_leu32(bpb->bpb_32.BPB_FATSz32);

	if (fs->type != kFAT32)
		fs->FirstRootDirSector = from_leu16(bpb->BPB_RsvdSecCnt) +
		    (bpb->BPB_NumFATs * fs->FATSz);
	fs->FirstDataSector = from_leu16(bpb->BPB_RsvdSecCnt) +
	    (bpb->BPB_NumFATs * fs->FATSz) + fs->RootDirSectors;

	if (bpb->BPB_TotSec16.value != 0)
		TotSec = from_leu16(bpb->BPB_TotSec16);
	else
		TotSec = from_leu32(bpb->BPB_TotSec32);

	DataSec = TotSec -
	    (from_leu16(bpb->BPB_RsvdSecCnt) + (bpb->BPB_NumFATs * fs->FATSz) +
		fs->RootDirSectors);

	size_t CountOfClusters = DataSec / bpb->BPB_SecPerClus;

	kprintf(
	    "Clusters: %zu; fatsz %lu; Rootentcount %hu; root dir secs = %lu; firstdatasector = %lu\n",
	    CountOfClusters, fs->FATSz, from_leu16(bpb->BPB_RootEntCnt),
	    fs->RootDirSectors, fs->FirstDataSector);

	if (CountOfClusters < 4085)
		fs->type = kFAT12;
	else if (CountOfClusters < 65525)
		fs->type = kFAT16;
	else
		fs->type = kFAT32;

	fs->root = dosfs_vnode_make(NULL, NULL);

	readDir(fs, fs->root, 0);

	for (;;)
		;
}

@end
