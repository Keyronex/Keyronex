#ifndef KRX_KDK_FILE_H
#define KRX_KDK_FILE_H

#include "kdk/executive.h"
#include "kdk/vm.h"
#include "kdk/vfs.h"

/*!
 * @brief Open File object.
 */
typedef struct file {
	namecache_handle_t nch;
	io_off_t offset;
} file_t;

ex_desc_ret_t ex_service_file_open(eprocess_t *proc, const char *upath);
ex_size_ret_t ex_service_file_read_cached(eprocess_t *proc, descnum_t handle,
    vaddr_t ubuf, size_t count);
ex_size_ret_t ex_service_file_write_cached(eprocess_t *proc, descnum_t handle,
    vaddr_t ubuf, size_t count);
ex_off_ret_t ex_service_file_seek(eprocess_t *proc, descnum_t handle,
    io_off_t offset);

#endif /* KRX_KDK_FILE_H */
