
#include "kdk/kmem.h"
#include "kdk/nanokern.h"
#include "kdk/object.h"
#include "kdk/vfs.h"
#include "vm/vmp.h"

/*
 * note: vfs refcounting probably needed
 * for unmount
 */

#define MNT_HASH_NBUCKETS 16

kmutex_t vfs_lock = KMUTEX_INITIALIZER(vfs_lock);
LIST_HEAD(vfs_hash_bucket, vfs) vfs_hash[MNT_HASH_NBUCKETS];

obj_class_t vnode_class;

vnode_t *
vnode_alloc(void)
{
	vnode_t *vnode;
	obj_new(&vnode, vnode_class, sizeof(vnode_t), NULL);
	return vnode;
}

vnode_t *
vnode_new(vfs_t *vfs, vtype_t type, struct vnode_ops *ops, kmutex_t *rwlock,
    kmutex_t *paging_rwlock, uintptr_t fs_data)
{
	vm_object_t *obj;
	vm_page_t *vpml4;
	vnode_t *vnode = vnode_alloc();

	kprintf(" -VN-  CREATE in vnode_new (rc == 1)\n");
	vnode->refcount = 1;

	vnode->type = type;
	vnode->fs_data = fs_data;
	vnode->ops = ops;
	vnode->vfs = vfs;
	vnode->rwlock = rwlock;
	vnode->paging_rwlock = paging_rwlock;

	RB_INIT(&vnode->ubc_windows);

	if (type == VREG) {
		obj = kmem_alloc(sizeof(vm_object_t));
		obj->kind = kFile;
		obj->file.vnode = vnode;
		obj->file.n_dirty_pages = 0;
		vm_page_alloc(&vpml4, 0, kPageUseVPML4, false);
		obj->vpml4 = vmp_page_paddr(vpml4);
		vnode->object = obj;
	}

	return vnode;
}

static unsigned int
nchandle_hash(namecache_handle_t handle)
{
	uintptr_t hash;

	hash = (uintptr_t)handle.nc * 31;
	hash ^= ((uintptr_t)handle.vfs * 31);

	hash = (hash >> 16) ^ (hash & 0xFFFF);

	return hash % MNT_HASH_NBUCKETS;
}

vfs_t *
vfs_find(namecache_handle_t nch)
{
	struct vfs_hash_bucket *bucket = &vfs_hash[nchandle_hash(nch)];
	vfs_t *vfs_entry;

	ke_wait(&vfs_lock, "vfs_search:vfs_lock", false, false, -1);
	LIST_FOREACH (vfs_entry, bucket, vfs_hash_entry)
		if (nchandle_eq(vfs_entry->nchcovered, nch)) {
			ke_mutex_release(&vfs_lock);
			return vfs_entry;
		}
	ke_mutex_release(&vfs_lock);
	return NULL;
}

#if 0
static void
vfs_insert(vfs_t *vfs)
{
	struct vfs_hash_bucket *bucket =
	    &vfs_hash[nchandle_hash(vfs->nchcovered)];
	ke_wait(&vfs_lock, "vfs_search:vfs_lock", false, false, -1);
	LIST_INSERT_HEAD(bucket, vfs, vfs_hash_entry);
	ke_mutex_release(&vfs_lock);
}
#endif

void
vfs_unmount(namecache_handle_t nch)
{
	kfatal("Unmount!\n");
}

vnode_t *
vn_retain(vnode_t *vnode)
{
	uint32_t count = __atomic_fetch_add(&vnode->refcount, 1,
	    __ATOMIC_RELAXED);
	if (count == 0)
		kprintf("ref from 0\n");
	kassert(count < 0xfffffff0);
	return vnode;
}

void
vn_release(vnode_t *vnode)
{
	while (true) {
		uint32_t old_count = __atomic_load_n(&vnode->refcount,
		    __ATOMIC_RELAXED);
		if (old_count > 1) {
			if (__atomic_compare_exchange_n(&vnode->refcount,
				&old_count, old_count - 1, false,
				__ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
				return;
			}
		} else if (old_count == 1) {
			if (splget() >= kIPLDPC) {
				kfatal("just call obj_release in a worker");
				return;
			} else {
				kprintf(" -VN-   FREE %p\n", vnode);
				return;
			}
		} else {
			kassert("unreached\n");
		}
	}
}
