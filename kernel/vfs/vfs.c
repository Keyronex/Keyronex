
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/kern.h"
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

enum vtype
mode_to_vtype(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return VDIR;

	case S_IFCHR:
		return VCHR;

	case S_IFBLK:
		return VNON;

	case S_IFREG:
		return VREG;

	case S_IFIFO:
		return VNON;

	case S_IFLNK:
		return VLNK;

	case S_IFSOCK:
		return VSOCK;

	default:
		return VNON;
	}
}

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
	ipl_t ipl;

#if LOG_VFS_REFCOUNT
	kprintf(" -VN-  CREATE in vnode_new (rc == 1)\n");
#endif
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
		obj->vpml4 = vm_page_paddr(vpml4);
		vnode->object = obj;
		ke_mutex_init(&obj->map_entry_list_lock);
		LIST_INIT(&obj->map_entry_list);
	}

	if (vfs != NULL) {
		ipl = ke_spinlock_acquire(&vfs->vnode_list_lock);
		TAILQ_INSERT_TAIL(&vfs->vnode_list, vnode, vnode_list_entry);
		ke_spinlock_release(&vfs->vnode_list_lock, ipl);
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
		kfatal("ref from 0\n");
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
				bool succeeded = vnode->ops->inactive(vnode);
				if (succeeded)
					return;
#if 1
				/* normal condition, but let's keep an eye on */
				kprintf("\033[1;31mvn release race\033[0m\n");
#endif
				/* if not, retry... */
			}
		} else {
			kassert("unreached\n");
		}
	}
}

int
vfs_try_retain(vfs_t *vfs)
{
	uint32_t current = __atomic_load_n(&vfs->file_refcnt, __ATOMIC_ACQUIRE);
	while (current != 1) {
		uint32_t desired = current + 2;

		if (current & 1)
			kfatal("Unexpected value of VFS refcnt %d\n", current);

		if (__atomic_compare_exchange_n(&vfs->file_refcnt, &current,
			desired, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
#if LOG_VFS_REFCOUNT
			kprintf(" -VFS- reTAIN %p to %d\n", vfs, desired);
#endif
			return 0;
		}
	}

#if LOG_VFS_REFCOUNT
	kprintf(" -VFS- reTAIN %p FAILED\n", vfs);
#endif
	return -1;
}

void
vfs_release(vfs_t *vfs)
{
	uint32_t ret = __atomic_fetch_sub(&vfs->file_refcnt, 2,
	    __ATOMIC_RELEASE);
#if LOG_VFS_REFCOUNT
	kprintf(" -VFS- reLEASE %p to %d\n", vfs, ret - 2);
#else
	(void)ret;
#endif
}

void
vfs_vn_remove(vnode_t *vnode)
{
	kassert(ke_spinlock_held(&vnode->vfs->vnode_list_lock));
	TAILQ_REMOVE(&vnode->vfs->vnode_list, vnode, vnode_list_entry);
}

void
vfs_vn_iter_init(vfs_vnode_iter_t *it, vfs_t *vfs)
{
	ipl_t ipl;

	it->end_marker_vn.type = VITER_MARKER;
	it->vfs = vfs;

	ipl = ke_spinlock_acquire(&vfs->vnode_list_lock);
	TAILQ_INSERT_TAIL(&vfs->vnode_list, &it->end_marker_vn,
	    vnode_list_entry);
	it->next_vn = TAILQ_FIRST(&vfs->vnode_list);
	vn_retain(it->next_vn);
	ke_spinlock_release(&vfs->vnode_list_lock, ipl);
}

/*! @brief get next vnode of a vfs from an iterator, returns retained */
vnode_t *
vfs_vn_iter_next(vfs_vnode_iter_t *it)
{
	ipl_t ipl;
	vnode_t *vn;

	if (it->next_vn == NULL)
		return NULL;

	ipl = ke_spinlock_acquire(&it->vfs->vnode_list_lock);

	/* we retained this (as needed for our return) when we set this */
	vn = it->next_vn;

	/* skip other markers but stop on our own */
	do {
		it->next_vn = TAILQ_NEXT(it->next_vn, vnode_list_entry);
		/* we must always reach our own marker, not NULL */
		kassert(it->next_vn != NULL);
	} while (it->next_vn->type == VITER_MARKER &&
	    it->next_vn != &it->end_marker_vn);

	if (it->next_vn == &it->end_marker_vn)
		it->next_vn = NULL;
	else
		vn_retain(it->next_vn);

	ke_spinlock_release(&it->vfs->vnode_list_lock, ipl);

	return vn;
}

void
vfs_vn_iter_destroy(vfs_vnode_iter_t *it)
{
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&it->vfs->vnode_list_lock);
	TAILQ_REMOVE(&it->vfs->vnode_list, &it->end_marker_vn,
	    vnode_list_entry);
	ke_spinlock_release(&it->vfs->vnode_list_lock, ipl);

	if (it->next_vn != NULL)
		vn_release(it->next_vn);
}

void
vn_fsync(vnode_t *vn)
{
	ke_wait(vn->rwlock, "vn_fsync:vn->rwlock", false, false, -1);
	kprintf("Do fsync\n");
	ke_mutex_release(vn->rwlock);
}

void
vfs_fsync_all(vfs_t *vfs)
{
	vfs_vnode_iter_t it;
	vnode_t *vn;

	vfs_vn_iter_init(&it, vfs);

	while ((vn = vfs_vn_iter_next(&it))) {
		vn_fsync(vn);
		vn_release(vn);
	}

	vfs_vn_iter_destroy(&it);
}
