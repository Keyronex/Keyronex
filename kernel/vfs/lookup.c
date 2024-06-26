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

static inline namecache_handle_t
nchandle_retain_novfs(namecache_handle_t in)
{
	nc_retain(in.nc);
	return in;
}

static inline namecache_handle_t
nchandle_release_novfs(namecache_handle_t in)
{
	nc_release(in.nc);
	return (namecache_handle_t) { NULL, NULL };
}

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

	if (*path == '/') {
		nch = nchandle_retain_novfs(root_nch);
	} else
		nch = nchandle_retain_novfs(start);

	r = vfs_try_retain(nch.nc->vp->vfs);
	/* if we can refer to an nc, an unmount shouldn't be in progress! */
	kassert(r == 0);

	for (np = TAILQ_FIRST(&state.components); np != NULL;
	     np = TAILQ_NEXT(np, tailq_entry)) {
		namecache_handle_t next_nch;

		if (TAILQ_NEXT(np, tailq_entry) == NULL &&
		    flags & kLookup2ndLast)
			break;

		if (strcmp(np->name, ".") == 0) {
			/* note: ought to check if is dir */
			continue;
		} else if (strcmp(np->name, "..") == 0) {
			/* note: figure out mount locking... */
			while (nch.nc == nch.vfs->root_ncp) {
				namecache_handle_t next = nch.vfs->nchcovered;
				if (vfs_try_retain(next.nc->vp->vfs) != 0)
					kfatal("Interrupted by unmount?\n");
				/* should reconsider the mountpoint traversal */
				nchandle_retain_novfs(next);
				nchandle_release_novfs(nch);
				vfs_release(nch.nc->vp->vfs);
				nch = next;
			}
		}

		next_nch.vfs = nch.vfs;

		r = nc_lookup(nch.nc, &next_nch.nc, np->name);
		if (r != 0)
			break;

		/* note: figure out mount locking.. */
		while (next_nch.nc->n_mounts_over > 0) {
			vfs_t *vfs;

			vfs = vfs_find(nch);
			if (vfs != NULL) {
				namecache_handle_t vfsroot;

				r = vfs_try_retain(vfs);
				if (r != 0)
					continue;

				vfsroot.nc = nc_retain(vfs->root_ncp);
				vfsroot.vfs = vfs;
				vfs_release(next_nch.nc->vp->vfs);
				nchandle_release_novfs(next_nch);
				next_nch = vfsroot;
			} else
				break;
		}

#if 0
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
			nchandle_release_novfs(nch);
			nch = next_nch;
		}
	}

	goto out;

out:
	TAILQ_FOREACH_SAFE (np, &state.components, tailq_entry, tmp)
		kmem_free(np, sizeof(*np));

	for (int i = 0; i < nlinks; i++)
		kmem_free(linkpaths[i], 256);

	kmem_free(pathcpy, pathlen);

	if (r == 0)
		*out = nch;
	else {
		vfs_release(nch.nc->vp->vfs);
		nchandle_release_novfs(nch);
	}

	return r;
}
