#ifndef KRX_KDK_NAMECACHE_H
#define KRX_KDK_NAMECACHE_H

#include <bsdqueue/queue.h>
#include <bsdqueue/tree.h>
#include <kdk/kernel.h>

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
 * constraints.
 *
 * Locking:
 * 	(m) => namecache::lock
 * 	(l) => namecache_lru_lock
 */
struct namecache {
	kmutex_t mutex;			  /*!< longer-term lock */
	uint32_t refcnt;		  /*!< count of retaining references */
	uint8_t name_len;		  /*!< length of name, max 255 */
	uint32_t unused : 24;		  /*!< can become flags in the future */
	TAILQ_ENTRY(namecache) lru_entry; /*!< linkage in LRU list*/
	RB_ENTRY(namecache) rb_entry;	  /*!< linkage in parent->entries */
	RB_HEAD(namecache_rb, namecache) entries; /*!< (m) names in directory */
	struct namecache *parent; /*!< (l to read) parent directory namecache */
	struct vnode *vp;	  /*!< underlying vnode or NULL if a negative */
	const char *name;	  /*!< filename */
	uint64_t key;		  /*!< rb key: len << 32 | hash(name) */
	uint64_t unused2; /*!< could use this space to store an inline name? */
};

/*!
 * Handle to a namecache elemnt. The vfsp pointer should facilitate bind mounts
 * without duplication of vnodes (and perhaps without duplication of namecaches)
 * by providing for an override on which vnode ops to use.
 */
typedef struct nchandle {
	struct namecache *ncp;
	struct vfs *vfsp;
} nchandle_t;

#endif /* KRX_KDK_NAMECACHE_H */
