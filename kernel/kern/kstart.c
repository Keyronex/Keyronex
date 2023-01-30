#include <sys/stat.h>

#include <amd64/amd64.h>
#include <tmpfs/tmpfs.h>
#include <vm/vm.h>

#include "lwip/tcpip.h"

typedef struct ustar_hdr {
	char filename[100];
	char mode[8];
	char ownerid[8];
	char groupid[8];

	char size[12];
	char mtime[12];

	char checksum[8];
	char type;
	char link[100];

	char ustar[6];
	char version[2];

	char owner[32];
	char group[32];

	char dev_major[8];
	char dev_minor[8];

	char prefix[155];
} __attribute__((__packed__)) ustar_hdr_t;

enum {
	kUStarNormal = '0',
	kUStarHardLink = '1',
	kUStarSymLink = '2',
	kUStarDirectory = '5',
};

static int
oct2i(unsigned char *s, int size)
{
	int n = 0;
	while (size-- > 0) {
		n *= 8;
		n += *s - '0';
		s++;
	}
	return n;
}

static void
unpack_ramdisk(void)
{
	void  *initbin;
	size_t size;

	if (module_request.response->module_count != 1) {
		kfatal("No initrd module.\n");
	}

	/*! now actually unpack the initrd into the tmpfs */
	initbin = module_request.response->modules[0]->address;
	size = module_request.response->modules[0]->size;

	kprintf("unpacking initrd...\n");

	for (size_t i = 0; i < size;) {
		ustar_hdr_t *star = initbin + i;
		int	     fsize = oct2i((unsigned char *)star->size, 11);
		vattr_t	     attr;

		if (!*star->filename)
			break;
		else if (!*(star->filename + 2))
			goto next;

		attr.mode = oct2i((unsigned char *)star->mode,
		    sizeof(star->mode) - 1);
		attr.mode = attr.mode & ~(S_IFMT);

#if 0
		kprintf("Create %s, mode %s\n", star->filename, star->mode);
#endif

		switch (star->type) {
		case kUStarDirectory: {
			int	 r;
			vnode_t *vn;

			attr.type = VDIR;

			r = vfs_lookup(root_vnode, &vn, star->filename,
			    kLookupCreat, &attr);
			if (r < 0) {
				kprintf("failed to make dir: %d\n", -r);
			}
			break;
		}

		case kUStarNormal: {
			int	 r;
			vnode_t *vn;

			attr.type = VREG;

			r = vfs_lookup(root_vnode, &vn, star->filename,
			    kLookupCreat, &attr);
			if (r < 0) {
				kprintf("failed to make file: %d\n", -r);
			}

			r = vn->ops->write(vn, initbin + i + 512, fsize, 0);

			kassert(r >= 0);
			break;
		}

		default:
			kprintf("unexpected type %c\n", star->type);
		}

	next:
		i += 512 + ROUNDUP(fsize, 512);
	}
	kprintf("done\n");
}

void
makeroot(void)
{

	int	r;
	vattr_t devattr = { .mode = 0755 | S_IFDIR, .size = 0, .type = VDIR };

	/*! create the root tmpfs */
	root_vfs.ops = &tmpfs_vfsops;

	r = root_vfs.ops->mount(&root_vfs, NULL, NULL);
	kassert(r >= 0);

	root_vfs.ops->root(&root_vfs, &root_vnode);

	kprintf("Creating dev vnode\n");
	r = root_vnode->ops->create(root_vnode, &dev_vnode, "dev", &devattr);
	kassert(r >= 0);
	kprintf("Created vnode %p\n", dev_vnode);
}

void
kstart(void)
{
	kthread_t pd_thread;

	makeroot();

	tcpip_init(NULL, NULL);

	int autoconf(void);
	autoconf();
	vm_pagedump();

	nk_thread_init(&proc0, &pd_thread, vm_pdaemon, NULL, "vm_pagedaemon");
	nk_thread_resume(&pd_thread);

	unpack_ramdisk();
	vm_pagedump();

	for (;;)
		asm("pause");
}
