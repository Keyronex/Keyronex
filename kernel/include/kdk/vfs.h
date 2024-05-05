#ifndef KRX_KDK_VFS_H
#define KRX_KDK_VFS_H

#include <stddef.h>
#include <stdint.h>

#include "dev.h"

RB_HEAD(ubc_window_tree, ubc_window);

typedef enum vtype { VNON, VREG, VDIR, VCHR, VLNK, VSOCK, VFIFO } vtype_t;

typedef struct vattr {
	enum vtype type;
} vattr_t;

typedef struct vnode {
	vtype_t type;
	struct vnode_ops *ops;
	uintptr_t fs_data;
	struct vfs *vfs;

	/*! General rwlock. */
	kmutex_t *rwlock;
	/*! Paging I/O rqlock. */
	kmutex_t *paging_rwlock;

	/*! UBC windows into this vnode; protected by UBC spinlock */
	struct ubc_window_tree ubc_windows;
	/*! this vnode's VM object */
	vm_object_t *object;
} vnode_t;

typedef struct vfs {
	/*! the filesystem device */
	DKDevice *device;
	struct vfs_ops *ops;
} vfs_t;

/*!
 * These are some initial notes on namecaches. Some of this should go in a big
 * theory statement instead, and needs to be written about in the Keyronex book.
 *
 * A namecache represents an element in the filesystem namespace - a file,
 * folder, device, etc. They hold a pointer to a parent namecache and an RB tree
 * of child namecaches.
 *
 * Namecaches are reference-counted. While there is a reference count greater
 * than 1, they may not be freed. When reference count is at 0, the namecache
 * is entered onto an LRU list; it may be dropped when it reaches the end of
 * that list.
 *
 * The pointer to the parent is a retaining pointer; the RB tree of children are
 * weak pointers.
 *
 * A general parent-before-child lock ordering exists along with a pointer
 * comparison (lower first) ordering for the acquisition of 'sibling' locks.
 *
 * Rough notes below to be structured:
 *
 * namecaches retain their vnodes.
 *
 * the vnode pointer may currently only transition once - from NULL to non-NULL.
 * thereafter it is as durable as the namecache itself.
 *
 * name and key may change (e.g. with rename) ofc under tight locking
 * constraints, i.e. parent locked and nc locked
 *
 * Locking:
 * 	(m) => namecache::lock
 * 	(l) => namecache_lru_lock
 */
typedef struct namecache {
	kmutex_t mutex;			  /*!< namespace lock */
	uint32_t refcnt;		  /*!< count of retaining references */
	uint8_t name_len;		  /*!< length of name, max 255 */
	uint32_t unused : 24;		  /*!< can become flags in the future */
	TAILQ_ENTRY(namecache) lru_entry; /*!< linkage in LRU list*/
	RB_ENTRY(namecache) rb_entry;	  /*!< linkage in parent->entries */
	RB_HEAD(namecache_rb, namecache) entries; /*!< (m) names in directory */
	struct namecache *parent; /*!< (l to read) parent directory namecache */
	struct vnode *vp;	  /*!< underlying vnode or NULL if a negative */
	char *name;		  /*!< filename */
	uint64_t key;		  /*!< rb key: len << 32 | hash(name) */
	uint64_t unused2; /*!< could use this space to store an inline name? */
} namecache_t;

TAILQ_HEAD(namecache_tailq, namecache);

typedef struct namecache_handle {
	namecache_t *nc;
	vfs_t *vfs;
} namecache_handle_t;

struct vnode_ops {
	io_off_t (*readdir)(vnode_t *dvn, void *buf, size_t nbyte,
	    size_t bytes_read, io_off_t seqno);
	int (*lookup)(vnode_t *dvn, vnode_t **out, const char *name);
};

struct vfs_ops {
	vnode_t (*root)(vfs_t *vfs);
};

vnode_t *vnode_new(vfs_t *vfs, vtype_t type, struct vnode_ops *ops,
    kmutex_t *rwlock, kmutex_t *paging_rwlock, uintptr_t fs_data);
void vnode_setup_cache(vnode_t *vnode);

/*!
 * Allocate a vnode.
 */
vnode_t *vnode_alloc(void);

int ubc_io(vnode_t *vnode, vaddr_t user_addr, io_off_t off, size_t size,
    bool write);

static inline vnode_t *
vn_retain(vnode_t *vnode)
{
	return vnode;
}

static inline void
vn_release(vnode_t *vnode)
{
}

static inline bool
vn_has_cache(vnode_t *vnode)
{
	return vnode->object != NULL;
}

enum lookup_flags {
	kLookupNoFollowFinalSymlink = 1 << 1,
	kLookup2ndLast = 1 << 2,
};

/*!
 * @brief Look up a pathname.
 */
int vfs_lookup(namecache_handle_t start, namecache_handle_t *out,
    const char *path, enum lookup_flags flags);

/*! @brief Retain a retained or (if refcnt = 0) LOCKED namecache. */
struct namecache *nc_retain(struct namecache *nc);
/*! @brief Release a retained, UNLOCKED namecache. */
struct namecache *nc_release(struct namecache *nc);
/*! @brief Look up a filename within a retained, UNLOCKED namecache. */
int nc_lookup(struct namecache *nc, struct namecache **out, const char *name);

void nc_make_root(vfs_t *vfs, vnode_t *vnode);

static inline namecache_handle_t
nchandle_retain(namecache_handle_t in)
{
	nc_retain(in.nc);
	return in;
}

static inline namecache_handle_t
nchandle_release(namecache_handle_t in)
{
	nc_release(in.nc);
	return (namecache_handle_t) { NULL, NULL };
}


extern namecache_handle_t root_nch;

#endif /* KRX_KDK_VFS_H */
