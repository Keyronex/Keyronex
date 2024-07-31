

#include "kdk/vfs.h"
#include "kdk/libkern.h"

void
test_fs_refcounts(void)
{
	namecache_handle_t hdl = root_nch, write_test, nonexist;
	int r;

	kprintf("Before any lookups\n");
	nc_dump();

	kprintf("\nLooking up /etc/write_test...\n");
	r = vfs_lookup(hdl, &write_test, "/etc/write_test", 0);
	kassert(r == 0);
	nc_dump();

	kprintf("\nLooking up /usr/lib/nonexistent...\n");
	r = vfs_lookup(hdl, &nonexist, "/usr/lib/nonexistent", 0);
	kassert(r < 0);
	nc_dump();

	kprintf("\nDoing a write to write_test.\n");
	const char *test = "I have been written to a file!!\n";
	ubc_io(write_test.nc->vp, (vaddr_t)test, 0, strlen(test) + 1, true);
	nchandle_release(write_test);

	kprintf("\nPurging the UBC:\n");
	ubc_remove_vfs(root_nch.vfs);

	kprintf("\nSyncing the VFS:\n");
	vfs_fsync_all(root_nch.vfs);

	kprintf("\nReleasing the subtree...\n");
	nc_remove_vfs(root_nch.vfs);
	nc_dump();

	kfatal("Stop here\n");
}

void
test_unmount(void)
{
	vfs_unmount(root_nch);
}
