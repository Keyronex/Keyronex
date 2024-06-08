#ifndef KRX_KDK_VFS_H
#define KRX_KDK_VFS_H

#include <stddef.h>
#include <stdint.h>

#include "dev.h"

RB_HEAD(ubc_window_tree, ubc_window);

/* TODO: make away with `uint8_t name_len`; it duplicates part of RB key. */
typedef struct namecache {
	kmutex_t mutex;	       /*!< namespace lock */
	uint32_t refcnt;       /*!< count of retaining references */
	uint8_t name_len;      /*!< length of name, max 255 */
	uint8_t n_mounts_over; /*!< count of mounts made on this */
	uint32_t unused : 24;  /*!< can become flags in the future */
	struct vnode *vp;      /*!< underlying vnode or NULL if a negative */
	struct vfs *vfsp;      /*!< VFS it belongs to, non-retaining. */
	TAILQ_ENTRY(namecache) lru_entry; /*!< linkage in LRU list*/
	RB_ENTRY(namecache) rb_entry;	  /*!< linkage in parent->entries */
	RB_HEAD(namecache_rb, namecache) entries; /*!< (m) names in directory */
	struct namecache *parent; /*!< (l to read) parent directory namecache */
	char *name;		  /*!< filename */
	uint64_t key;		  /*!< rb key: len << 32 | hash(name) */
	uint64_t unused2; /*!< could use this space to store an inline name? */
} namecache_t;

TAILQ_HEAD(namecache_tailq, namecache);

typedef struct namecache_handle {
	namecache_t *nc;
	struct vfs *vfs;
} namecache_handle_t;

typedef enum vtype { VNON, VREG, VDIR, VCHR, VLNK, VSOCK, VFIFO } vtype_t;

typedef struct vattr {
	enum vtype type;
} vattr_t;

typedef struct vnode {
	uint32_t refcount;

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
	/*! entry on vfs hash */
	LIST_ENTRY(vfs) vfs_hash_entry;
	/*! root namecache node */
	namecache_t *root_ncp;
	/*! namecache handle over which the mount was made, if not root */
	namecache_handle_t nchcovered;
	/*! the filesystem device */
	DKDevice *device;
	/*! operations vector */
	struct vfs_ops *ops;
	/*! per-fs data */
	uintptr_t vfs_data;
} vfs_t;

struct vnode_ops {
	io_off_t (*readdir)(vnode_t *dvn, void *buf, size_t nbyte,
	    size_t bytes_read, io_off_t seqno);
	int (*lookup)(vnode_t *dvn, vnode_t **out, const char *name);
};

struct vfs_ops {
	int (*mount)(namecache_handle_t over, const char *params);
	/* vnode_t (*root)(vfs_t *vfs); */
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

vnode_t *vn_retain(vnode_t *vnode);
void vn_release(vnode_t *vnode);

static inline bool
vn_has_cache(vnode_t *vnode)
{
	return vnode->object != NULL;
}

enum lookup_flags {
	kLookupNoFollowFinalSymlink = 1 << 1,
	kLookup2ndLast = 1 << 2,
};

/*! Find the VFS mounted over a given nch. */
vfs_t *vfs_find(namecache_handle_t nch);

/*! @brief Unmount a mountpoint. */
void vfs_unmount(namecache_handle_t nch);

/*! @brief Look up a pathname. */
int vfs_lookup(namecache_handle_t start, namecache_handle_t *out,
    const char *path, enum lookup_flags flags);

/*! @brief Retain a retained or (if refcnt = 0) LOCKED namecache. */
struct namecache *nc_retain(struct namecache *nc);
/*! @brief Release a retained, UNLOCKED namecache. */
struct namecache *nc_release(struct namecache *nc);
/*! @brief Look up a filename within a retained, UNLOCKED namecache. */
int nc_lookup(struct namecache *nc, struct namecache **out, const char *name);

void nc_make_root(vfs_t *vfs, vnode_t *vnode);
void nc_dump(void);

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

static inline bool
nchandle_eq(namecache_handle_t x, namecache_handle_t y)
{
	return x.nc == y.nc && x.vfs == y.vfs;
}

extern namecache_handle_t root_nch;

#endif /* KRX_KDK_VFS_H */
