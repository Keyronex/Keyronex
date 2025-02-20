/*
 * Copyright (c) 2023-2025 NetaScale Object Solutions.
 * Created on Wed Sep 20 2023.
 */

#ifndef KRX_KDK_DEV_H
#define KRX_KDK_DEV_H

#include <kdk/kern.h>
#include <kdk/vm.h>

/*! Count of blocks. */
typedef uint64_t io_blksize_t;
/*! Block offset. */
typedef int64_t io_blkoff_t;
/*! Byte offset. */
typedef int64_t io_off_t;

struct vnode;

typedef enum iop_return {
	kIOPRetCompleted,
	kIOPRetContinue,
	kIOPRetPending,
	kIOPRetBegin,
} iop_return_t;

/*!
 * I/O packet function.
 */
typedef enum iop_function {
	/*! Read data request. */
	kIOPTypeRead,
	/*! Write data request. */
	kIOPTypeWrite,
	/*! I/O control. */
	kIOPTypeIOCtl,
	/*! SCSI command. */
	kIOPTypeSCSI,
	/*! 9p command. */
	kIOPType9p,
	/*! Connect socket. */
	kIOPTypeConnect,
} iop_function_t;

typedef enum iop_ioctl {
	kIOCTLDiskBlockSize = 1,
} iop_ioctl_t;

/*!
 * For kIOPTypeRead/kIOPTypeWrite.
 * The iop_frame->mdl is the MDL to read from/write into.
 */
struct iop_stack_data_rw {
	/*! Number of bytes to read. */
	size_t bytes;
	/*! Byte offset in file at which to read. */
	io_off_t offset;
};

/*! For kIOPTypeIOCtl */
struct iop_stack_data_ioctl {
	/*! IOCtl type. */
	iop_ioctl_t type;
	/*! Output buffer size. */
	size_t out_buffer_size;
};

/*! For kIOPTypeSCSI */
struct iop_stack_data_scsi {
	struct _SCSI_REQUEST_BLOCK *srb;
};

/*! For kIOPType9p. iop_frame's ptr_in holds any relevant MDL. */
struct iop_stack_data_9p {
	struct ninep_buf *ninep_in, *ninep_out;
};

/*! For kIOPTypeConnect. */
struct iop_stack_data_connect {
	struct sockaddr_storage *sockaddr;
};

/*! For kIOPTypeUSBDeviceRequest */
struct iop_stack_data_usb_device_request {
	struct dk_usb_dev_req *request;
};

/*!
 * A frame in an I/O packet stack.
 */
typedef struct iop_frame {
	iop_function_t function : 8;
	bool has_kbuf;
	struct vnode *vnode;
	union {
		vm_mdl_t *mdl;
		void *kbuf;
	};
	union {
		struct iop_stack_data_rw rw;
		struct iop_stack_data_ioctl ioctl;
		struct iop_stack_data_scsi scsi;
		struct iop_stack_data_9p ninep;
		struct iop_stack_data_connect connect;
	};
} iop_frame_t;

typedef enum iop_direction {
	kIOPDown,
	kIOPUp,
} iop_direction_t;

typedef struct io_result {
	uintptr_t result; /*! 0 or errno or -1 (eof) */
	uintptr_t count;  /*! count of bytes dealt with, if relevant */
} io_result_t;

typedef struct iop {
	TAILQ_ENTRY(iop) dev_qlink;
	io_result_t *result;
	kevent_t *event;

	iop_direction_t direction : 8;
	bool begun;
	uint8_t stack_count;
	int8_t stack_current;
	uint32_t incomplete_slave_iops_n;

	SLIST_HEAD(, iop) slave_iops;

	struct iop *master_iop;
	SLIST_ENTRY(iop) slave_iop_qlink;

	iop_frame_t stack[0];
} iop_t;

typedef TAILQ_HEAD(iop_queue, iop) iop_queue_t;

/*!
 * @brief Allocate and set up an empty IOP which must be further initialised.
 *
 * @param dev VNode the IOP is to be sent to. IOP Stack size is set
 * accordingly.
 * @param op IOP function.
 */
iop_t *iop_new(struct vnode *vp, iop_function_t op);

/*!
 * @brief (re-)initialise an IOP.
 */
void iop_init(iop_t *iop);

/*!
 * @brief Free an IOP.
 */
void iop_free(iop_t *iop);

/*!
 * @brief Allocate & set up a read IOP on a vnode.
 *
 * @param dev Device the IOP is to be sent to.
 * @param mdl MDL for the output buffer.
 * @param size Size in bytes to read.
 */
iop_t *iop_new_vnode_read(struct vnode *vnode, vm_mdl_t *mdl, size_t size,
    io_off_t off);

/*!
 * @brief Allocate & set up a write IOP on a vnode.
 *
 * @param dev Device the IOP is to be sent to.
 * @param mdl MDL for the input buffer. (Should have ->write = false)
 * @param size Size in bytes to write.
 */
iop_t *iop_new_vnode_write(struct vnode *vnode, vm_mdl_t *mdl, size_t size,
    io_off_t off);

/*!
 * @brief Allocate & set up a 9p IOP.
 *
 * @param vnpde Vnode the IOP is to be sent to.
 * @param buf_in 9p in-buffer.
 * @param buf_out 9p out-buffer.
 * @param mdl Optional MDL.
 */
iop_t *iop_new_9p(struct vnode *vnode, struct ninep_buf *in, struct ninep_buf *out,
    vm_mdl_t *mdl);

/*! @brief Send and await completion of an IOP. */
io_result_t iop_send_sync(iop_t *iop);

/*!
 * @brief Continue processing an IOP.
 */
iop_return_t iop_continue(iop_t *iop, iop_return_t res);

/*!
 * @brief Return a pointer to the current frame of an IOP.
 */
static inline iop_frame_t *
iop_stack_current(iop_t *iop)
{
	return &iop->stack[iop->stack_current];
}

/*!
 * @brief Return a pointer to the next (higher) frame of an IOP.
 */
static inline iop_frame_t *iop_stack_next(iop_t *iop)
{
	kassert(iop->stack_count > iop->stack_current + 1);
	kassert(iop->stack_current >= -1);
	return &iop->stack[iop->stack_current + 1];
}

/*!
 * @brief Setup an IOP frame for a 9p request.
 */
void iop_frame_setup_9p(struct vnode *vnode, iop_frame_t *frame, struct ninep_buf *in,
    struct ninep_buf *out, vm_mdl_t *mdl);

#endif /* KRX_KDK_DEV_H */
