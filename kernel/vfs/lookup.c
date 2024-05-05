#include <errno.h>

#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/vfs.h"

struct lookup {
	TAILQ_HEAD(namepart_tailq, namepart) components;
};

struct namepart {
	TAILQ_ENTRY(namepart) tailq_entry;
	char *name;
	bool must_be_dir;
};

static int
split_path(struct lookup *out, char *path, struct namepart *after)
{
	char *last;

	char *component = strtok_r(path, "/", &last);
	while (component != NULL) {
		struct namepart *np = kmem_alloc(sizeof(struct namepart));
		if (np == NULL) {
			return -ENOMEM;
		}
		np->must_be_dir = false;
		np->name = component;
		if (np->name == NULL) {
			kmem_free(np, sizeof(*np));
			return -ENOMEM;
		}

		/*
		 * Insert the component into the appropriate location.
		 */
		if (after == NULL) {
			TAILQ_INSERT_TAIL(&out->components, np, tailq_entry);
		} else {
			TAILQ_INSERT_AFTER(&out->components, after, np,
			    tailq_entry);
			after = np;
		}

		component = strtok_r(NULL, "/", &last);
	}

	if (after == NULL)
		after = TAILQ_LAST(&out->components, namepart_tailq);

	/*
	 * Handle the case of a trailing slash.
	 */
	if (path[strlen(path) - 1] == '/' && after != NULL)
		after->must_be_dir = true;

	return 0;
}

int
vfs_lookup(namecache_handle_t start, namecache_handle_t *out, const char *path,
    enum lookup_flags flags)
{
	struct lookup state;
	namecache_handle_t nch;
	struct namepart *np, *tmp;
	char *pathcpy, *linkpaths[8];
	size_t pathlen;
	size_t nlinks = 0;
	int r = 0;

	TAILQ_INIT(&state.components);

	pathcpy = strdup(path);
	pathlen = strlen(path) + 1;
	split_path(&state, pathcpy, NULL);

	if (*path == '/')
		nch = nchandle_retain(root_nch);
	else
		nch = nchandle_retain(start);

	for (np = TAILQ_FIRST(&state.components); np != NULL;
	     np = TAILQ_NEXT(np, tailq_entry)) {
		namecache_handle_t next_nch;

		next_nch.vfs = nch.vfs;

		if (TAILQ_NEXT(np, tailq_entry) == NULL &&
		    flags & kLookup2ndLast)
			break;

		if (strcmp(np->name, ".") == 0) {
			/* note: ought to check if is dir */
			continue;
		}
#if 0
		else if (strcmp(np->name, "..") == 0) {
			/* note: shuld lock mount_lock and probably loop */
			if (vn->isroot && vn->vfsp->vnodecovered != NULL) {
				next_vn = obj_direct_retain(
				    vn->vfsp->vnodecovered);
				obj_direct_release(vn);
				vn = next_vn;
			}
		}
#endif

		r = nc_lookup(nch.nc, &next_nch.nc, np->name);
		if (r != 0)
			break;

#if 0
		/*
		 * note: later we'll add a kLookupNoFollowFinalMount also
		 * for unmount()
		 */
		if (next_vn->vfsmountedhere != NULL) {

			while (next_vn->vfsmountedhere != NULL) {
				vnode_t *root;
				vfs_t *vfs;
				ipl_t ipl;
				int r;

				ipl = ke_spinlock_acquire(&mount_lock);
				vfs = next_vn->vfsmountedhere;
				if (vfs)
					vfs->refcnt++;
				ke_spinlock_release(&mount_lock, ipl);

				if (vfs == NULL)
					break;

				r = next_vn->vfsmountedhere->ops->root(
				    next_vn->vfsmountedhere, &root);

				/*
				 * todo: create a vfs_release() function
				 * if refcnt now drops to zero, we could proceed
				 * with a temporarily-delayed unmounting
				 */
				__atomic_sub_fetch(&vfs->refcnt, 1,
				    __ATOMIC_SEQ_CST);

				if (r < 0) {
					obj_direct_release(next_vn);
					goto out;
				}

				obj_direct_release(next_vn);
				next_vn = root;
			}
		}

		if (next_vn->type == VLNK &&
		    !(TAILQ_NEXT(np, tailq_entry) == NULL &&
			flags & kLookupNoFollowFinalSymlink)) {
			char *buf;
			int r;

			if (nlinks + 1 > 8) {
				obj_direct_release(next_vn);
				r = -ELOOP;
				goto out;
			}

			buf = linkpaths[nlinks++] = kmem_alloc(256);

			r = VOP_READLINK(next_vn, buf);
			if (r != 0) {
				obj_direct_release(next_vn);
				break;
			}

			split_path(&state, buf, np);

			/* if link begins with /, go to root */
			if (*buf == '/') {
				obj_direct_release(vn);
				vn = obj_direct_retain(root_vnode);
			}

			obj_direct_release(next_vn);
		} else
#endif
		{
			nchandle_release(nch);
			nch = next_nch;
		}
	}

out:
	TAILQ_FOREACH_SAFE (np, &state.components, tailq_entry, tmp)
		kmem_free(np, sizeof(*np));

	for (int i = 0; i < nlinks; i++)
		kmem_free(linkpaths[i], 256);

	kmem_free(pathcpy, pathlen);

	if (r == 0)
		*out = nch;
	else
		nchandle_release(nch);

	return r;
}
