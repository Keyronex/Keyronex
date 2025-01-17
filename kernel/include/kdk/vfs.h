#ifndef KRX_KDK_VFS_H
#define KRX_KDK_VFS_H

#include <sys/stat.h>

#include <fcntl.h>
#include <kdk/dev.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct poll_entry;
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

typedef enum vtype {
	VNON,
	VREG,
	VDIR,
	VCHR,
	VLNK,
	VSOCK,
	VFIFO,
	VITER_MARKER
} vtype_t;

typedef struct vattr {
	enum vtype type;      /*!< vnode type */
	mode_t mode;	      /*!< mode and type */
	nlink_t nlink;	      /*!< number of links to file */
	uid_t uid;	      /*!< owning user*/
	gid_t gid;	      /*!< owning group*/
	dev_t fsid;	      /*!< fs unique id */
	ino_t fileid;	      /*!< file unique id */
	uint64_t size;	      /*!< size in bytes */
	uint64_t blocksize;   /*!< fs block size  */
	struct timespec atim, /*!< last access time */
	    mtim,	      /*!< last modified time */
	    ctim;	      /*!< creation time */
	dev_t rdev;	      /*!< represented device */
	uint64_t disksize;    /*!< on-disk size in bytes */
} vattr_t;

typedef struct vnode {
	uint32_t refcount;

	/*! entry in vfs::vnode_list */
	TAILQ_ENTRY(vnode) vnode_list_entry;

	vtype_t type;
	struct vnode_ops *ops;
	uintptr_t fs_data;
	struct vfs *vfs;

	/*! General rwlock. */
	kmutex_t *rwlock;
	/*! Paging I/O rqlock. */
	kmutex_t *paging_rwlock;

	union {
		/*! for VREG */
		struct {
			/*!
			 * UBC windows into this vnode; protected by UBC
			 * spinlock
			 */
			struct ubc_window_tree ubc_windows;
			/*! this vnode's VM object */
			vm_object_t *object;
		};
	};
} vnode_t;

typedef struct vfs {
	/*! Deferred-kmem_free RCU entry. */
	krcu_entry_t free_rcu_entry;
	/*! atomic - lowest bit = pending unnount */
	uint32_t file_refcnt;
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

	/* vnode list lock */
	kspinlock_t vnode_list_lock;
	/* all vnodes of this vfs list */
	TAILQ_HEAD(, vnode) vnode_list;

	/*! per-fs data */
	uintptr_t vfs_data;
} vfs_t;

typedef struct vfs_vnode_iter {
	vnode_t end_marker_vn;
	vnode_t *next_vn;
	vfs_t *vfs;
} vfs_vnode_iter_t;

/*!
 * What kind of chpoll operation is being done.
 */
enum chpoll_mode {
	kChpollPoll,
	kChpollRemove,
};

struct vnode_ops {
	/*! Returns true if freed, false if not and should retry. */
	bool (*inactive)(vnode_t *vn);

	io_result_t (*cached_read)(vnode_t *vnode, vaddr_t user_addr,
	    io_off_t off, size_t size);
	io_result_t (*cached_write)(vnode_t *vnode, vaddr_t user_addr,
	    io_off_t off, size_t size);
	io_off_t (*readdir)(vnode_t *dvn, void *buf, size_t nbyte,
	    size_t bytes_read, io_off_t seqno);

	int (*seek)(vnode_t *vnode, io_off_t old, io_off_t *new);
	int (*getattr)(vnode_t *vnode, vattr_t *attr);
	int (*ioctl)(vnode_t *vnode, unsigned long cmd, void *data);

	/*!
	 * Returns events, if there are any pending. If a poll_entry is passed,
	 * then it should link it onto its pollhead
	 */
	int (*chpoll)(vnode_t *vnode, struct poll_entry *poll,
	    enum chpoll_mode mode);

	int (*lookup)(vnode_t *dvn, vnode_t **out, const char *name);
};

struct vfs_ops {
	int (*mount)(namecache_handle_t over, const char *params);
	int (*sync)(vfs_t *vfs);
};

vnode_t *vnode_new(vfs_t *vfs, vtype_t type, struct vnode_ops *ops,
    kmutex_t *rwlock, kmutex_t *paging_rwlock, uintptr_t fs_data);
void vnode_setup_cache(vnode_t *vnode);

vnode_t *vn_retain(vnode_t *vnode);
void vn_release(vnode_t *vnode);

#ifdef LOG_VFS_REFCOUNT
#define VN_RELEASE(VN, MSG) ({
	kprintf("(VN release) %p -> now %d %s\n", VN, VN->refcount - 1, MSG);
	vn_release(VN);
})
#define VN_RETAIN(VN, MSG) ({
	kprintf("(VN retain) %p -> now %d %s\n", VN, VN->refcount + 1, MSG);
	vn_retain(VN);
})
#else
#define VN_RELEASE(VN, MSG) vn_release(VN)
#define VN_RETAIN(VN, MSG) vn_retain(VN)
#endif

static inline bool
vn_has_cache(vnode_t *vnode)
{
	return vnode->object != NULL;
}

/*! @brief Remove vnode from per-VFS list (caller needs vfs vn list lock!) */
void vfs_vn_remove(vnode_t *vnode);
/*! @brief Initialise a per-VFS vnode list iterator. */
void vfs_vn_iter_init(vfs_vnode_iter_t *it, vfs_t *vfs);
/*! @brief Get the next vnode from the per-VFS vnode list iterator. */
vnode_t *vfs_vn_iter_next(vfs_vnode_iter_t *it);
/*! @brief Destroy a per-VFS vnode list iterator. */
void vfs_vn_iter_destroy(vfs_vnode_iter_t *it);

/*! Write bytes to a given address */
int ubc_io(vnode_t *vnode, vaddr_t user_addr, io_off_t off, size_t size,
    bool write);

/*! @brief Remove all windows for given VFS from cache. */
void ubc_remove_vfs(vfs_t *vfs);

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

/*!
 * @brief Try to retain a reference to a VFS.
 * @retval -1 Can't retain (VFS is probably being unmounted)
 * @retval 0 Retained successfully
 */
int vfs_try_retain(vfs_t *vfs);

/*! @brief Release a reference on a VFS. */
void vfs_release(vfs_t *vfs);

/*! @brief Synch dirty buffers of all files in a VFS. */
void vfs_fsync_all(vfs_t *vfs);

/*! @brief Retain a retained or (if refcnt = 0) LOCKED namecache. */
struct namecache *nc_retain(struct namecache *nc);
/*! @brief Release a retained, UNLOCKED namecache. */
struct namecache *nc_release(struct namecache *nc);
/*! @brief Look up a filename within a retained, UNLOCKED namecache. */
int nc_lookup(struct namecache *nc, struct namecache **out, const char *name);

void nc_remove_vfs(vfs_t *vfs);
void nc_make_root(vfs_t *vfs, vnode_t *vnode);
void nc_dump(void);

vtype_t mode_to_vtype(mode_t mode);

static inline namecache_handle_t
nchandle_retain(namecache_handle_t in)
{
	int r;
	r = vfs_try_retain(in.nc->vp->vfs);
	kassert(r == 0);
	nc_retain(in.nc);
	return in;
}

static inline namecache_handle_t
nchandle_release(namecache_handle_t in)
{
	vfs_t *vfs = in.nc->vp->vfs;
	nc_release(in.nc);
	vfs_release(vfs);
	return (namecache_handle_t) { NULL, NULL };
}

static inline bool
nchandle_eq(namecache_handle_t x, namecache_handle_t y)
{
	return x.nc == y.nc && x.vfs == y.vfs;
}

extern namecache_handle_t root_nch;

#endif /* KRX_KDK_VFS_H */
