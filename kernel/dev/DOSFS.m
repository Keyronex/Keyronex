#include <dirent.h>

#include "DOSFS.h"
#include "ddk/DKDevice.h"
#include "dev/buf.h"
#include "dev/dosfs_var.h"
#include "dev/fslib.h"
#include "dev/safe_endian.h"
#include "kdk/dev.h"
#include "kdk/endian.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/tree.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "vm/vmp.h"

#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

#define VTOD(VNODE) ((struct dos_node *)(VNODE)->fs_data)

struct dosfs_state {
	/*! *not* a holding pointer */
	DOSFS *fsdev;
	/*! vfs */
	vfs_t vfs;
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
	/*! number of clusters */
	size_t CountOfCusters;
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
	/*! rwlock for data contents, size, etc. */
	kmutex_t rwlock;
	/*! the parent directory of this node (or NULL if root). retained */
	vnode_t *parent;
	/*! disk address of containing short dirent */
	io_blkoff_t dirent_daddr;
	/*! directory entries in this directory, if it is a dir */
	struct dos_node_name_rb entries_rb;
	/*! first cluster of file, if not root dir on non-FAT32 */
	io_blkoff_t first_cluster;
	/*! size in bytes of the file */
	uint32_t size;
	/*! is it the root directory? */
	bool is_root;
};

static int counter = 0;

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

static inline bool
cluster_value_is_eof(struct dosfs_state *state, uint32_t value)
{
	if (state->type == kFAT12) {
		if (value >= 0x0FF8)
			return true;
	} else if (state->type == kFAT16) {
		if (value >= 0xFFF8)
			return true;
	} else if (value >= 0x0FFFFFF8)
		return true;
	return false;
}

static uint32_t
read_fat(struct dosfs_state *fs, io_blkoff_t N)
{
	io_blkoff_t FATOffset, ThisFATSecNum, ThisFATEntOffset, ClusEntryVal;
	buf_t *buf;

	if (fs->type == kFAT12)
		FATOffset = N + (N / 2);
	else if (fs->type == kFAT16)
		FATOffset = N * 2;
	else {
		kassert(fs->type == kFAT32);
		FATOffset = N * 4;
	}

	ThisFATSecNum = from_leu16(fs->bpb->BPB_RsvdSecCnt) +
	    (FATOffset / from_leu16(fs->bpb->BPB_BytsPerSec));
	ThisFATEntOffset = FATOffset % from_leu16(fs->bpb->BPB_BytsPerSec);
	buf = bread(&fs->bufhead, ThisFATSecNum, 0);

	switch (fs->type) {
	case kFAT16: {
		leu16_t *fat16 = (leu16_t *)buf->data;
		ClusEntryVal = from_leu16(fat16[ThisFATEntOffset / 2]);
		break;
	}

	case kFAT32: {
		leu32_t *fat32 = (leu32_t *)buf->data;
		ClusEntryVal = (from_leu32(fat32[ThisFATEntOffset / 4])) &
		    0x0FFFFFFF;
		break;
	}

	case kFAT12:
		if (ThisFATEntOffset ==
		    (from_leu16(fs->bpb->BPB_BytsPerSec) - 1)) {
			/* crossing a sector boundary! */
			ClusEntryVal = (uint8_t)buf->data[ThisFATEntOffset];
			buf_release(buf);
			buf = bread(&fs->bufhead, ThisFATSecNum + 1, 0);
			ClusEntryVal |= ((uint8_t)buf->data[0]) << 8;
		} else {
			ClusEntryVal = (uint8_t)buf->data[ThisFATEntOffset] +
			    ((uint8_t)buf->data[ThisFATEntOffset + 1] << 8);
		}

		if (N & 0x0001)
			ClusEntryVal = ClusEntryVal >> 4;
		else
			ClusEntryVal = ClusEntryVal & 0x0FFF;
	}

	buf_release(buf);

	return ClusEntryVal;
}

void
write_fat(struct dosfs_state *fs, io_blkoff_t N, uint32_t ClusEntryVal)
{
	io_blkoff_t FATOffset, ThisFATSecNum, ThisFATEntOffset;
	buf_t *buf;

	if (fs->type == kFAT12)
		FATOffset = N + (N / 2);
	else if (fs->type == kFAT16)
		FATOffset = N * 2;
	else {
		kassert(fs->type == kFAT32);
		FATOffset = N * 4;
	}

	ThisFATSecNum = from_leu16(fs->bpb->BPB_RsvdSecCnt) +
	    (FATOffset / from_leu16(fs->bpb->BPB_BytsPerSec));
	ThisFATEntOffset = FATOffset % from_leu16(fs->bpb->BPB_BytsPerSec);
	buf = bread(&fs->bufhead, ThisFATSecNum, 0);

	switch (fs->type) {
	case kFAT16: {
		leu16_t *fat16 = (leu16_t *)buf->data;
		fat16[ThisFATEntOffset / 2] = to_leu16((uint16_t)ClusEntryVal);
		break;
	}

	case kFAT32: {
		leu32_t *fat32 = (leu32_t *)buf->data;
		uint32_t current_val = from_leu32(fat32[ThisFATEntOffset / 4]);
		uint32_t merged_val = (current_val & 0xF0000000) |
		    (ClusEntryVal & 0x0FFFFFFF);
		fat32[ThisFATEntOffset / 4] = to_leu32(merged_val);
		break;
	}

	case kFAT12: {
		uint16_t current_val, merged_val;

		if (N & 0x0001)
			merged_val = (ClusEntryVal << 4) & 0xFFF0;
		else
			merged_val = ClusEntryVal & 0x0FFF;

		if (ThisFATEntOffset ==
		    (from_leu16(fs->bpb->BPB_BytsPerSec) - 1)) {
			/* crossing a sector boundary */
			buf_t *buf2;
			buf2 = bread(&fs->bufhead, ThisFATSecNum + 1, 0);
			current_val = (buf->data[ThisFATEntOffset] & 0xFF) |
			    ((buf2->data[0] & 0x0F) << 8);

			if (N & 0x0001) {
				merged_val |= (current_val & 0x000F);
			} else {
				merged_val |= (current_val & 0xF000);
			}

			buf->data[ThisFATEntOffset] = (uint8_t)(merged_val &
			    0xFF);
			buf2->data[0] = (uint8_t)((merged_val >> 8) & 0xFF);
			bwrite(buf2);
			buf_release(buf2);
		} else {
			/* not crossing a sector boundary */
			current_val = (buf->data[ThisFATEntOffset] & 0xFF) |
			    (buf->data[ThisFATEntOffset + 1] << 8);

			if (N & 0x0001)
				merged_val |= (current_val & 0x000F);
			else
				merged_val |= (current_val & 0xF000);

			buf->data[ThisFATEntOffset] = (uint8_t)(merged_val &
			    0xFF);
			buf->data[ThisFATEntOffset + 1] =
			    (uint8_t)((merged_val >> 8) & 0xFF);
		}
		break;
	}
	}

	bwrite(buf);
	buf_release(buf);
}

#if 0
static void
iterate_fat(struct dosfs_state *fs)
{
	io_blkoff_t FATSecNum = from_leu16(fs->bpb->BPB_RsvdSecCnt);
	buf_t *buf = bread(&fs->bufhead, FATSecNum,
	    0); // Start with first FAT sector
	const uint8_t *data = (uint8_t *)buf->data;
	size_t offset = 0;
	io_blkoff_t N = 2;

	while (N < fs->CountOfCusters) {
		uint32_t val;

		if (offset == from_leu16(fs->bpb->BPB_BytsPerSec)) {
			buf_release(buf);
			buf = bread(&fs->bufhead, ++FATSecNum, 0);
			data = (uint8_t *)buf->data;
			offset = 0;
		}

		switch (fs->type) {
		case kFAT12: {
			if (offset == from_leu16(fs->bpb->BPB_BytsPerSec) - 1) {
				val = (uint8_t)data[offset];
				buf_release(buf);
				buf = bread(&fs->bufhead, ++FATSecNum, 0);
				data = (const uint8_t *)buf->data;
				val |= ((uint8_t)data[0]) << 8;
				offset = 1;
			} else {
				val = (uint8_t)data[offset] |
				    ((uint8_t)data[offset + 1] << 8);
				offset += 1 + (N & 0x0001);
			}
			if (N & 0x0001)
				val = val >> 4;
			else
				val &= 0x0FFF;

			break;
		}
		case kFAT16: {
			leu16_t *pval = (leu16_t *)(data + offset);
			val = from_leu16(*pval);
			offset += 2;
			break;
		}
		case kFAT32: {
			leu32_t *pval = (leu32_t *)(data + offset);
			val = from_leu32(*pval);
			offset += 4;
			break;
		}
		}

		kprintf("FAT %lld: %u/%x\n", N - 2, val, val);
		N++;
	}
	buf_release(buf);
}
#endif

static buf_t *
dosnode_bread(struct dosfs_state *fs, struct dos_node *dosnode,
    io_blkoff_t cluster)
{
	if (fs->type != kFAT32 && dosnode->is_root) {
		kassert(cluster < fs->RootDirSectors / fs->bpb->BPB_SecPerClus);
		return bread(&fs->bufhead,
		    fs->FirstRootDirSector + cluster * fs->bpb->BPB_SecPerClus,
		    fs->bytes_per_cluster);
	} else if (cluster == 0)
		return bread(&fs->bufhead,
		    fs->FirstDataSector +
			(dosnode->first_cluster - 2) * fs->bpb->BPB_SecPerClus,
		    fs->bytes_per_cluster);

	io_blkoff_t last_cluster = dosnode->first_cluster;
	for (size_t i = cluster; i != 0; i--) {
		last_cluster = read_fat(fs, last_cluster);
		kassert(last_cluster >= 2 &&
		    !cluster_value_is_eof(fs, last_cluster));
	}

	return bread(&fs->bufhead,
	    fs->FirstDataSector + (last_cluster - 2) * fs->bpb->BPB_SecPerClus,
	    fs->bytes_per_cluster);
}

/*!
 * create a vnode for some dirent in a dir vnode
 * \p parent->rwlock held
 */
static vnode_t *
dosfs_vnode_make(struct dosfs_state *fs, vnode_t *parent, const char *name,
    struct Dir *dir)
{
	struct dos_node *node = kmem_alloc(sizeof(*node)), *parent_node;
	ke_mutex_init(&node->rwlock);
	if (name == NULL || parent == NULL) {
		kassert(name == NULL && parent == NULL);
		node->is_root = true;
	} else {
		kassert(name != NULL && parent != NULL);
		parent_node = VTOD(parent);
		node->key.filename = strdup(name);
		node->key.hash = strhash32(name, strlen(name));
		RB_INSERT(dos_node_name_rb, &parent_node->entries_rb,
		    &node->key);
	}
	node->vnode = vnode_alloc();
	node->vnode->type = false;
	node->vnode->fs_data = (uintptr_t)node;
	node->vnode->vfs = &fs->vfs;
	if (dir != NULL) {
		if (fs->type == kFAT32) {
			uint32_t rootCluster =
			    ((uint32_t)from_leu16(dir->DIR_FstClusHI) << 16) |
			    (uint32_t)from_leu16(dir->DIR_FstClusLO);
			node->first_cluster = rootCluster;
		} else {
			node->first_cluster = from_leu16(dir->DIR_FstClusLO);
		}
		node->size = from_leu32(dir->DIR_FileSize);

#if DEBUG_DOSFS > 0
		DKDevLog(fs->fsdev, "FIRST KLUSTER FOR NODE %p %s: %llx;\n",
		    node, dir->DIR_Name, node->first_cluster);
#endif
	}
	return node->vnode;
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

static enum {
	kDirentProcessingContinue,
	kDirentProcessingStop,
	kDirentProcessed
} processDirent(struct Dir *dir, char *lfn_str, size_t *lfn_str_max)
{
	if ((uint8_t)dir->DIR_Name[0] == 0x0)
		return kDirentProcessingStop;
	else if ((uint8_t)dir->DIR_Name[0] == 0xe5)
		return kDirentProcessingContinue;
	else if (dir->DIR_Attr == ATTR_LONG_NAME) {
		struct LongDir *ldir = (void *)dir;
		io_off_t ldir_off = 13 * ((ldir->LDIR_Ord & 0x3f) - 1);
		io_off_t lfn_off = 0;
		bool terminated = false;

		lfn_off += ucs2_to_utf8(ldir->LDIR_Name1, lfn_str + ldir_off,
		    sizeof(ldir->LDIR_Name1) / sizeof(leu16_t), &terminated);
		if (terminated)
			goto finish_lfn;

		lfn_off += ucs2_to_utf8(ldir->LDIR_Name2,
		    lfn_str + lfn_off + ldir_off,
		    sizeof(ldir->LDIR_Name2) / sizeof(leu16_t), &terminated);
		if (terminated)
			goto finish_lfn;

		lfn_off += ucs2_to_utf8(ldir->LDIR_Name3,
		    lfn_str + lfn_off + ldir_off,
		    sizeof(ldir->LDIR_Name3) / sizeof(leu16_t), &terminated);
		if (terminated)
			goto finish_lfn;

	finish_lfn:
		if ((lfn_off + ldir_off) > *lfn_str_max)
			*lfn_str_max = lfn_off + ldir_off;
		return kDirentProcessingContinue;
	} else
		kfatal("Unexpected sort of directory\n");
}

void
printdir(struct dosfs_state *fs, vnode_t *vn, io_off_t offset)
{
	char lfn_str[260] = { 0 };
	size_t lfn_str_max = 0;
	io_off_t disk_offset = offset;
	buf_t *buf = NULL;
	struct dos_node *dnode = VTOD(vn);
	size_t last_offset = offset;

	kassert(offset % 32 == 0x0);

	ke_wait(&dnode->rwlock, "dosfs::readdir::dnode->rwlock", false, false,
	    -1);

	if (dnode->is_root && offset < 64) {
		kassert(offset == 0 || offset == 32);
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
		else
			disk_offset = offset;

		clus_dir = (disk_offset / sizeof(struct Dir)) %
		    fs->bytes_per_cluster;

		if (buf == NULL || clus_dir == 0) {
			buf_release(buf);
			buf = dosnode_bread(fs, dnode,
			    disk_offset / fs->bytes_per_cluster);
		}

		dir = &((struct Dir *)buf->data)[clus_dir];

		switch (processDirent(dir, lfn_str, &lfn_str_max)) {
		case kDirentProcessingContinue:
			offset += sizeof(struct Dir);
			continue;

		case kDirentProcessingStop:
			goto out;

		default:
			/* epsilon */
		}

		lfn_str[lfn_str_max] = '\0';
		kprintf(
		    "Dirent: Name %s; LFN <%s>; Cluster %hu; Attributes %hu; 1st Char %u\n",
		    dir->DIR_Name, lfn_str, from_leu16(dir->DIR_FstClusLO),
		    dir->DIR_Attr, (uint8_t)dir->DIR_Name[0]);
		lfn_str[0] = '\0';
		lfn_str_max = 0;
		last_offset = offset;
		offset += sizeof(struct Dir);
	}

	/* TODO: find out what this is there for */
	(void)last_offset;

out:
	buf_release(buf);
	ke_mutex_release(&dnode->rwlock);
}

vnode_t *
lookup(struct dosfs_state *fs, vnode_t *dvn, const char *name)
{
	char lfn_str[260] = { 0 };
	size_t lfn_str_max = 0;
	io_off_t offset = 0;
	buf_t *buf = NULL;
	struct dos_node *dnode = VTOD(dvn);

	if (dnode->is_root && strcmp(name, "..") == 0) {
		return dnode->parent;
	}

	ke_wait(&dnode->rwlock, "dosfs::lookup:inode->rwlock", false, false,
	    -1);

	struct dos_node_namekey key, *found;
	key.hash = strhash32(name, strlen(name));
	key.filename = (char *)name;
	found = RB_FIND(dos_node_name_rb, &dnode->entries_rb, &key);
	if (found != NULL) {
		struct dos_node *found_node = (struct dos_node *)found;
		vnode_t *vn = vn_retain(found_node->vnode);
		ke_mutex_release(&dnode->rwlock);
		return vn;
	}

	while (true) {
		size_t clus_dir;
		struct Dir *dir;

		clus_dir = (offset / sizeof(struct Dir)) %
		    fs->bytes_per_cluster;

		if (buf == NULL || clus_dir == 0) {
			buf_release(buf);
			buf = dosnode_bread(fs, dnode,
			    offset / fs->bytes_per_cluster);
		}

		dir = &((struct Dir *)buf->data)[clus_dir];
		switch (processDirent(dir, lfn_str, &lfn_str_max)) {
		case kDirentProcessingContinue:
			offset += sizeof(struct Dir);
			continue;

		case kDirentProcessingStop:
			goto out;

		default:;
			/* epsilon */
		}

		if (lfn_str[0] != '\0' && strcmp(lfn_str, name) == 0) {
			vnode_t *vnode = dosfs_vnode_make(fs, dvn, lfn_str,
			    dir);
			ke_mutex_release(&dnode->rwlock);
			buf_release(buf);
			return vnode;
		}

		offset += sizeof(struct Dir);
	}

out:
	ke_mutex_release(&dnode->rwlock);
	buf_release(buf);
	return NULL;
}

vnode_t *
create(vnode_t *dvn, const char *name, vattr_t attr)
{
	kfatal("Unimplemented\n");
}

io_off_t
dos_readdir(vnode_t *dvn, void *buf, size_t nbyte, size_t bytes_read,
    io_off_t seqno)
{
	struct dos_node *dnode = VTOD(dvn);
	(void)dnode;
	kfatal("Implement me\n");
}

static void
dos_extend_locked(struct dosfs_state *fs, vnode_t *vn, size_t size)
{
	kfatal("implement me\n");
}

void
dos_rw(struct dosfs_state *fs, vnode_t *vn, io_off_t offset, io_off_t bytes,
    vm_mdl_t *mdl, bool write)
{
	struct dos_node *dnode = VTOD(vn);
	iop_t *biop = NULL;

	kassert(offset % fs->bytes_per_cluster == 0);

	ke_wait(&dnode->rwlock, "dosfs::read:inode->rwlock", false, false, -1);

	if (dnode->size < offset + bytes)
		dos_extend_locked(fs, vn, offset + bytes);

	io_blkoff_t current_cluster = dnode->first_cluster;
	for (size_t i = offset / fs->bytes_per_cluster; i != 0; i--) {
		current_cluster = read_fat(fs, current_cluster);
		// kprintf("Current_cluster = %lld\n", current_cluster);
		kassert(current_cluster >= 2 &&
		    !cluster_value_is_eof(fs, current_cluster));
	}

#ifdef TRACE_DOSFS
	kprintf("At starting, current_cluster = %lld\n", current_cluster);
#endif

	io_off_t bytes_read = 0;
	while (bytes_read < bytes) {
#if DOSFS_CONTIGUOUS_IO
		io_off_t start_cluster = current_cluster;
		io_off_t contiguous_size = fs->bytes_per_cluster;

		io_blkoff_t next_cluster = read_fat(fs, current_cluster);
		while (next_cluster == current_cluster + 1 &&
		    bytes_read + contiguous_size < bytes) {

			kassert(next_cluster >= 2 &&
			    !cluster_value_is_eof(fs, next_cluster));
			current_cluster = next_cluster;
			contiguous_size += fs->bytes_per_cluster;
			next_cluster = read_fat(fs, current_cluster);
		}

		io_off_t to_read = MIN2(bytes - bytes_read, contiguous_size);
		io_off_t cluster_offset = start_cluster * fs->bytes_per_cluster;
#else
		io_off_t to_read = MIN2(bytes - bytes_read,
		    fs->bytes_per_cluster);
		io_off_t cluster_offset = fs->FirstDataSector *
			from_leu16(fs->bpb->BPB_BytsPerSec) +
		    (current_cluster - 2) * fs->bytes_per_cluster;
		io_blkoff_t next_cluster = read_fat(fs, current_cluster);

		kassert(next_cluster >= 2 &&
		    !cluster_value_is_eof(fs, next_cluster));
#endif

#ifdef TRACE_DOSFS
		kprintf("Read Clusters %lld, Count %lld, MDL offset %lld\n",
		    cluster_offset, to_read, bytes_read);
#endif
		biop = iop_new_read(fs->bufhead.device, mdl, to_read,
		    cluster_offset);
		iop_send_sync(biop);
		iop_free(biop);

		mdl->offset += to_read;
		bytes_read += to_read;
		current_cluster = next_cluster;
	}

	ke_mutex_release(&dnode->rwlock);
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

	fs->CountOfCusters = DataSec / bpb->BPB_SecPerClus;

	if (fs->CountOfCusters < 4085)
		fs->type = kFAT12;
	else if (fs->CountOfCusters < 65525)
		fs->type = kFAT16;
	else
		fs->type = kFAT32;

	kprintf("Clusters: %zu; fatsz %zu; root en count %hu; "
		"root dir secs %zu; first data sec %zu; fat type %d\n",
	    fs->CountOfCusters, fs->FATSz, from_leu16(bpb->BPB_RootEntCnt),
	    fs->RootDirSectors, fs->FirstDataSector, fs->type);

	// iterate_fat(fs);

	[[self alloc] initWithState:fs];
	return YES;
}

- (instancetype)initWithState:(struct dosfs_state *)state
{
	self = [super initWithProvider:state->bufhead.device];
	m_state = state;
	state->fsdev = self;
	kmem_asprintf(obj_name_ptr(self), "dosfs-%u", counter++);

	if (m_state->type == kFAT32) {
		struct Dir dir;
		uint32_t first_clus = from_leu32(
		    m_state->bpb->bpb_32.BPB_RootClus);
		dir.DIR_FstClusLO = to_leu16(first_clus & 0xffff);
		dir.DIR_FstClusHI = to_leu16((first_clus >> 16) & 0xffff);
		m_state->root = dosfs_vnode_make(m_state, NULL, NULL, &dir);
	} else
		m_state->root = dosfs_vnode_make(m_state, NULL, NULL, NULL);

	[self registerDevice];
	DKLogAttachExtra(self, "\"%.11s\": FAT%d",
	    state->type == kFAT32 ? (char *)state->bpb->bpb_32.BS_VolLab :
				    state->bpb->bpb_16.BS_VolLab,
	    state->type == kFAT12     ? 12 :
		state->type == kFAT16 ? 16 :
					32);

#if 0
	printdir(m_state, m_state->root, 0);

	vnode_t *vn = lookup(m_state, m_state->root, "genesis.txt");
	kprintf("VN: %p\n", vn);
	dos_rw(m_state, vn, 0, 2048 * 4, NULL, false);
	// buf_t *buf = dosnode_bread(m_state, (struct dos_node *)vn->fs_data,
	// 1);
	//  readDir(m_state, vn, 0);
#endif

	vm_mdl_t *mdl = vm_mdl_buffer_alloc(2);
	vnode_t *vn = lookup(m_state, m_state->root, "genesis.txt");
	kassert(vn != NULL);

#if 0
	kprintf("Testing unmapped read\n");
	dos_rw(m_state, vn, 0, 2048 * 4, mdl, false);

	mdl->offset = 0;
	char *vadd = (char *)P2V(vm_mdl_paddr(mdl, 0));
	(void) vadd;
#endif

	vm_section_t sect;
	sect.kind = kFile;
	sect.file.vnode = vn;
	RB_INIT(&sect.file.page_tree);

	state->vfs.device = self;

	extern vm_procstate_t kernel_procstate;
	vaddr_t mapaddr = 0xd0000000;
	vm_ps_map_section_view(&kernel_procstate, &sect, &mapaddr, 4096 * 32, 1,
	    kVMAll, kVMAll, true, false, true);
	kprintf("Testing mapped read (at 0x%zx)\n", mapaddr);

	for (int i = 0; i < 16; i++) {
		char str[80];
		memcpy(str, (void *)(mapaddr + i * PGSIZE), 79);
		str[79] = '\0';
		kprintf("Success. Extract: %s\n", str);
	}

	kprintf("\n\n!!! Testing again\n");
		for (int i = 0; i < 4; i++) {
		char str[80];
		kprintf("Testing %d\n", i);
		memcpy(str, (void *)(mapaddr + i * PGSIZE), 79);
		str[79] = '\0';
		kprintf("Success. Extract: %s\n", str);
	}

	vmp_pages_dump();

	for (;;)
		;
	return self;
}

- (iop_return_t)dispatchIOP:(iop_t *)iop
{
	iop_frame_t *frame = iop_stack_current(iop);
	kassert(frame->function == kIOPTypeRead);
	kassert(frame->vnode != NULL);
	DKDevLog(self, "Dispatching a read request - offset %lld length %zu\n",
	    frame->rw.offset, frame->rw.bytes);
	dos_rw(m_state, frame->vnode, frame->rw.offset, frame->rw.bytes,
	    frame->mdl, false);
	return kIOPRetCompleted;
}

@end

struct vnode_ops dos_vnops = {
	.readdir = dos_readdir,
};
