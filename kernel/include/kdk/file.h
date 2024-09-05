#ifndef KRX_KDK_FILE_H
#define KRX_KDK_FILE_H

#include "kdk/executive.h"
#include "kdk/vm.h"
#include "kdk/vfs.h"

/*!
 * @brief Open File object.
 */
typedef struct file {
	kspinlock_t epoll_lock;
	LIST_HEAD(, poll_entry) epoll_watches;
	namecache_handle_t nch;
	vnode_t *vnode;
	kmutex_t offset_mutex;
	io_off_t offset;
} file_t;

file_t *ex_file_new(void);
void ex_file_free(obj_t *file);
ex_desc_ret_t ex_service_file_open(eprocess_t *proc, const char *upath);
ex_desc_ret_t ex_service_file_close(eprocess_t *proc, descnum_t handle);
ex_size_ret_t ex_service_file_read_cached(eprocess_t *proc, descnum_t handle,
    vaddr_t ubuf, size_t count);
ex_size_ret_t ex_service_file_write_cached(eprocess_t *proc, descnum_t handle,
    vaddr_t ubuf, size_t count);
ex_off_ret_t ex_service_file_seek(eprocess_t *proc, descnum_t handle,
    io_off_t offset, int whence);
ex_err_ret_t ex_service_file_stat(eprocess_t *proc, int fd, const char *path,
    int flags, struct stat *statbuf);
ex_size_ret_t ex_service_file_ioctl(eprocess_t *proc, descnum_t handle,
    int request, void *arg);

#endif /* KRX_KDK_FILE_H */
