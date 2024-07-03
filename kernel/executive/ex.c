#include <sys/mman.h>

#include <abi-bits/errno.h>
#include <keyronex/syscall.h>
#include <stdint.h>

#include "exp.h"
#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/kern.h"
#include "kdk/object.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "ntcompat/ntcompat.h"
#include "object.h"
#include "vm/vmp.h"

/* exec.c */
int load_server(vnode_t *server_vnode, vnode_t *ld_vnode);
/* futex.c */
int krx_futex_wait(int *u_pointer, int expected, nanosecs_t ns);
int krx_futex_wake(int *u_pointer);

extern obj_class_t file_class;
kthread_t *ex_init_thread;
kthread_t *user_init_thread;
extern struct vfs_ops ninep_vfsops;

#if 0
static void
test_anon(void)
{
	vaddr_t addr;
	extern vm_procstate_t kernel_procstate;
	int r = vm_ps_allocate(&kernel_procstate, &addr, PGSIZE * 6, false);
	kassert(r == 0);
	kprintf("Allocated at 0x%zx\n", addr);
	*(unsigned int*)addr = 0xdeadbeef;
	*(unsigned int*)(addr + PGSIZE) = 0xbeefdead;
	*(unsigned int*)(addr + PGSIZE * 2) = 0xfeedbeef;
	*(unsigned int*)(addr + PGSIZE * 3) = 0xbeeffeed;
	*(unsigned int*)(addr + PGSIZE * 4) = 0xfeedbeef;
	*(unsigned int*)(addr + PGSIZE * 5) = 0xbeeffeed;

	*(unsigned int*)addr = 0xdeadbeef;
	*(unsigned int*)(addr + PGSIZE) = 0xbeefdead;

	kprintf("After touching 4 pages:\n");
	vmp_wsl_dump(&kernel_procstate);
	vmp_pages_dump();
}
#endif

#if 0
static void
test_file_write(void)
{
	namecache_handle_t hdl;
	int r;
	const char *txt = "Hello, file write world!\n";

	r = vfs_lookup(root_nch, &hdl, "testwrite.txt", 0);
	kassert(r == 0);

	r = ubc_io(hdl.nc->vp, (vaddr_t)txt, 0, strlen(txt) + 1,
	    true);
	kassert(r > 0);

	/* force eviction */
	test_anon();

	for (;;)
		;
}
#endif

#if 0

#pragma GCC push_options
#pragma GCC optimize("O0")
void
wait_a_bit()
{

	kprintf("\n\nWaiting....\n");
	for (int i = 0; i < INT32_MAX / 4; i++)
		asm volatile("nop");
}
#pragma GCC pop_options

static void
test_unmap(void)
{
	vaddr_t addr;
	extern vm_procstate_t kernel_procstate;
	int r;

	kprintf("Pages before test:\n");
	vmp_wsl_dump(&kernel_procstate);
	vmp_pages_dump();

	r = vm_ps_allocate(&kernel_procstate, &addr, PGSIZE * 1024 * 3, false);
	kassert(r == 0);

	*(unsigned int*)addr = 0xdeadbeef;
	*(unsigned int *)(addr + PGSIZE * 2) = 0xbeefdead;
	*(unsigned int *)(addr + PGSIZE * 3) = 0xbeefdead;
	*(unsigned int *)(addr + PGSIZE * 4) = 0xbeefdead;
	*(unsigned int*)(addr + PGSIZE * 5) = 0xbeefdead;
	*(unsigned int *)(addr + PGSIZE * 6) = 0xbeefdead;
	*(unsigned int *)(addr + PGSIZE * 7) = 0xbeefdead;

	*(unsigned int *)(addr + PGSIZE * (2048 + 2)) = 0xfeedbeef;
	*(unsigned int *)(addr + PGSIZE * (1024 + 3)) = 0xfeedbeef;
	*(unsigned int *)(addr + PGSIZE * (1024 + 4)) = 0xfeedbeef;
	*(unsigned int *)(addr + PGSIZE * (1024 + 5)) = 0xfeedbeef;
	*(unsigned int *)(addr + PGSIZE * (1024 + 6)) = 0xfeedbeef;
	*(unsigned int *)(addr + PGSIZE * (1024 + 7)) = 0xfeedbeef;

	wait_a_bit();

	kprintf("\n\nAfter touching pages:\n");
	vmp_wsl_dump(&kernel_procstate);
	vmp_pages_dump();

	*(unsigned int *)addr = 0xdeadbeef;

	vm_ps_deallocate(&kernel_procstate, addr, PGSIZE * 1024 * 3);

	kprintf("\n\nAfter freeing pages:\n");
	vmp_wsl_dump(&kernel_procstate);
	vmp_pages_dump();

	for (;;) ;
}
#endif

static void test_fs_refcounts(void) {
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

static void
test_unmount(void)
{
	vfs_unmount(root_nch);
}

void
user_init(void *arg)
{
	int r;
	namecache_handle_t ld, posix;

	r = vfs_lookup(root_nch, &ld, "/usr/lib/ld.so", 0);
	if (r != 0)
		kfatal("Failed to look up RTDL\n");

	r = vfs_lookup(root_nch, &posix, "/usr/bin/posix_server", 0);
	if (r != 0)
		kfatal("Failed to look up POSIX Server\n");

	r = load_server(posix.nc->vp, ld.nc->vp);
	if (r != 0)
		kfatal("Failed to load POSIX server\n");

	kfatal("Unreached\n");
}

static void
pagefile_init(void)
{
	int r;
	namecache_handle_t pagefile;

	r = vfs_lookup(root_nch, &pagefile, "pagefile", 0);
	if (r != 0)
		kfatal("Failed to look up /pagefile\n");

	vn_retain(pagefile.nc->vp);
	vm_pagefile_add(pagefile.nc->vp);
	nchandle_release(pagefile);
}

static void
mount_root(void)
{
	const char *how = boot_config.root;

	if (how == NULL) {
		kfatal("No root FS specified.\n");
	} else if (strncmp(how, "9p:", 3) == 0) {
		ninep_vfsops.mount((namecache_handle_t) {}, how + 3);
	} else {
		kfatal("Can't handle it\n");
	}
}

void
ex_init(void *)
{
	int r;
	void ddk_init(void), ddk_autoconf(void), ubc_init(void);

	vmp_paging_init();
	ubc_init();
	ddk_init();
#if 0
	pe_load(module_request.response->modules[3]->path,module_request.response->modules[3]->address);
#endif
	ddk_autoconf();

#if 0
	vmp_pages_dump();
	obj_dump();

	test_anon();
#endif

	mount_root();

	test_fs_refcounts();
	test_unmount();

	pagefile_init();

#if 0
	test_file_write();
#endif

#if 0
	test_unmap();
#endif

	eprocess_t *initps;
	r = ps_process_create(&initps, false);
	kassert(r == 0);

	r = ps_thread_create(&user_init_thread, "user init thread 0", user_init,
	    NULL, initps);
	kassert(r == 0);

	obj_release(initps);

	ke_thread_resume(user_init_thread);

	ps_exit_this_thread();
}

int
krx_vm_allocate(size_t size, vaddr_t *out)
{
	int r;
	vaddr_t vaddr;
	r = vm_ps_allocate(ex_curproc()->vm, &vaddr, size, false);
	*out = vaddr;
	return r;
}

size_t
user_strlen(const char *user_str)
{
	return strlen(user_str);
}

int
copyout_str(const char *ustr, char **out)
{
	int len;
	char *kstr;

	len = user_strlen(ustr);
	kstr = kmem_alloc(len + 1);
	memcpy(kstr, ustr, len);

	kstr[len] = '\0';
	*out = kstr;
	return 0;
}

int
krx_file_open(const char *upath)
{
	char *path;
	namecache_handle_t nch;
	int r;

	r = copyout_str(upath, &path);
	if (r != 0)
		return r;

	r = vfs_lookup(root_nch, &nch, path, 0);
#if TRACE_SYSCALLS
	if (r != 0)
		kprintf("Couldn't find <%s>\n", upath);
#endif

	if (r == 0) {
		struct file *file;
		eprocess_t *proc = ex_curproc();

		r = obj_new(&file, file_class, sizeof(struct file), NULL);
		kassert(r == 0);
		file->nch = nch;
		file->offset = 0;
		for (int i = 1; i < 64; i++) {
			if (proc->handles[i] == NULL) {
				proc->handles[i] = file;
				return i;
			}
		}
		kfatal("Couldn't get anywhere! Proc = %p\n", proc);
	}

	return r;
}

int
krx_file_read_cached(int handle, vaddr_t ubuf, size_t count)
{
	struct file *file;
	int r;

	file = ex_curproc()->handles[handle];
	r = ubc_io(file->nch.nc->vp, ubuf, file->offset, count, false);
	file->offset += r;
	return r;
}

io_off_t
krx_file_seek(int handle, io_off_t offset)
{
	struct file *file;

	file = ex_curproc()->handles[handle];
	if (handle == 0)
		return -ENOENT;

	file->offset = offset;
	return file->offset;
}

int
krx_vm_map(vaddr_t hint, size_t size, int prot, int flags, int handle,
    io_off_t offset, uintptr_t *out)
{
	int r;
	vm_object_t *obj = NULL;

	if (!(flags & MAP_ANON)) {
		struct file *file = ex_curproc()->handles[handle];
		obj = file->nch.nc->vp->object;
	} else if (flags & MAP_ANON && flags & MAP_SHARED) {
		kfatal("Implement anon shared\n");
	}

	r = vm_ps_map_object_view(ex_curproc()->vm, obj, &hint, size, offset,
	    kVMAll, kVMAll, !(flags & MAP_PRIVATE),
	    obj != NULL ? (flags & MAP_PRIVATE) : false, flags & MAP_FIXED);
	if (r == 0)
		*out = hint;

	return r;
}

struct thread_new_info {
	uintptr_t entry;
	uintptr_t stack;
};

static void
thread_trampoline(void *arg)
{
	struct thread_new_info info = *(struct thread_new_info *)arg;

	void ki_enter_user_mode(uintptr_t ip, uintptr_t sp);
	kmem_free(arg, sizeof(info));
	ki_enter_user_mode(info.entry, info.stack);
}

int
krx_fork_thread(uintptr_t entry, uintptr_t stack)
{
	kthread_t *thread;
	struct thread_new_info *info = kmem_alloc(sizeof(*info));
	int r;

	info->entry = entry;
	info->stack = stack;

	r = ps_thread_create(&thread, "newthread", thread_trampoline, info,
	    ex_curproc());
	if (r != 0) {
		kmem_free(info, sizeof(*info));
		return r;
	}

	ke_thread_resume(thread);

	return thread->tid;
}

static void
krx_thread_exit(void)
{
	ps_exit_this_thread();
}

uintptr_t
ex_syscall_dispatch(enum krx_syscall syscall, uintptr_t arg1, uintptr_t arg2,
    uintptr_t arg3, uintptr_t arg4, uintptr_t arg5, uintptr_t arg6,
    uintptr_t *out1)
{
	switch (syscall) {
	case kKrxDebugMessage: {
		char *msg;
		int r;

		r = copyout_str((const char *)arg1, &msg);
		kassert(r == 0);
		kprintf("[libc]: %s\n", msg);
		kmem_strfree(msg);
		return 0;
	}

	case kKrxTcbSet:
		curthread()->tcb = arg1;
#ifndef __m68k__
		ke_set_tcb(arg1);
#endif
		return 0;

	case kKrxTcbGet:
		return curthread()->tcb;

	case kKrxGetTid:
		return curthread()->tid;

	case kKrxVmAllocate:
		return krx_vm_allocate(arg1, out1);

	case kKrxVmMap:
		return krx_vm_map(arg1, arg2, arg3, arg4, arg5, arg6, out1);

	case kKrxFileOpen:
		return krx_file_open((const char *)arg1);

	case kKrxFileReadCached:
		return krx_file_read_cached(arg1, arg2, arg3);

	case kKrxFileSeek:
		return krx_file_seek(arg1, arg2);

	case kKrxThreadExit:
		krx_thread_exit();

	case kKrxForkThread:
		return krx_fork_thread(arg1, arg2);

	case kKrxFutexWait:
		return krx_futex_wait((int *)arg1, arg2, -1);

	case kKrxFutexWake:
		return krx_futex_wake((int *)arg1);

	default:
		kfatal("unhandled syscall %d\n", syscall);
	}
}
