/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 21 2023.
 */

#ifndef KRX_KDK_DEVMGR_H
#define KRX_KDK_DEVMGR_H

#include <bsdqueue/queue.h>
#include <stdint.h>

#include "kdk/kerndefs.h"
#include "kdk/objhdr.h"
#include "kdk/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*! Count of blocks. */
typedef uint64_t io_blksize_t;
/*! Block offset. */
typedef int64_t io_blkoff_t;
/*! Byte offset. */
typedef int64_t io_off_t;

typedef enum iop_return {
	kIOPRetCompleted,
	/*!
	 * From a completion, means "change directions and reprocess this
	 * location going downwards".
	 * From a dispatch, means "continue processing IOP as normal".
	 */
	kIOPRetContinue,
	kIOPRetPending,
} iop_return_t;

/*!
 * I/O packet function.
 */
typedef enum iop_function {
	kIOPTypeRead,
	kIOPTypeWrite,
	kIOPTypeIOCtl,
	kIOPTypeMount,
} iop_function_t;

typedef enum iop_ioctl {
	kIOCTLDiskBlockSize = 1,

	/*!
	 * Enqueue FUSE request. iop_frame::kbuf will point to io_fuse_request.
	 */
	kIOCTLFuseEnqueuRequest
} iop_ioctl_t;

/*! For kIOPTypeRead/kIOPTypeWrite. */
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

/*!
 * A frame in an I/O packet stack.
 */
typedef struct iop_frame {
	/*!
	 * Associated IOPs. These are associated with a frame entry by its
	 * dispatch function, and if they are present, then they will all be ran
	 * and only when they are all finished will the completion of this IOP
	 * be ran.
	 */
	SLIST_HEAD(, iop) associated_iops;
	/*! Number of pending associated IOPs. */
	uint8_t n_pending_associated_iops;
	/*! Which function is being carried out? (located here for packing)*/
	iop_function_t function;

	/*! Which device processes this stack entry? */
	struct device *dev;
	/*! Which vnode, if any? */
	struct vnode *vnode;
	/*! MDL or wired kernel buffer for principal input/out */
	union {
		vm_mdl_t *mdl;
		void *kbuf;
	};
	/*! Union of different types. */
	union {
		struct iop_stack_data_rw rw;
		struct iop_stack_data_ioctl ioctl;
	};
} iop_frame_t;

/*!
 * In which direction is the packet travelling?
 *
 * Generally down is what happens first, as the IOP is processed by elements of
 * the stack. Then it moves upwards again.
 */
typedef enum iop_direction {
	/*! IOP travelling down the device stack. */
	kIOPDown,
	/*! IOP travelling up the device stack. */
	kIOPUp,
} iop_direction_t;

/*!
 * I/O packet
 */
typedef struct iop {
	/*! An event which will be signalled when the IOP is complete. */
	kevent_t event;

	/*! Which device was the IOP originally directed at? */
	struct device *dev;
	/*! Linkage in a device queue. */
	TAILQ_ENTRY(iop) dev_queue_entry;

	unsigned
	    /*! Has the IOP started running? */
	    begun : 1;

	/*! Which direction is it currently travelling? */
	iop_direction_t direction;

	/*! If this is an associated IOP, this points to its master. */
	struct iop *master_iop;
	/*! If this is an associated IOP, its linkage. */
	SLIST_ENTRY(iop) associated_iops_link;

	/*! How many items are on the stack? */
	uint8_t stack_count;
	/*! Current stack entry. Higher levels have lower numbers. */
	int8_t stack_current;
	/*!
	 * The stack, containing at least #stack_count entries, ordered starting
	 * with the highest level first.
	 */
	iop_frame_t stack[0];
} iop_t;

/*!
 * A device object.
 */
typedef struct device {
	object_header_t objhdr;

	TAILQ_HEAD(, device) consumers;
	TAILQ_ENTRY(device) consumers_link;
	struct device *provider;
	uint8_t stack_depth;

	/*! Dispatch an I/O packet. */
	iop_return_t (*dispatch)(struct device *dev, iop_t *iop);
	/*! Handle completion of an IOP by lower device or associated IOP. */
	iop_return_t (*complete)(struct device *dev, iop_t *iop);

	/*! driver-specific context */
	// void *context;
} device_t;

#if 0
/*!
 * A driver object.
 */
typedef struct driver {

} driver_t;
#endif

/*!
 * Create a device. object.
 */
// int dev_create(d)

/*!
 * Attach a device object as consumer of another device object.
 */
void dev_attach(device_t *consumer, device_t *provider);

/*!
 * @brief Allocate and set up an empty IOP which must be further initialised.
 *
 * @param dev Device the IOP is to be sent to. IOP Stack size is set
 * accordingly.
 */
iop_t *iop_new(device_t *dev);

/*!
 * @brief Allocate & set up an IOCtl IOP.
 *
 * @param dev Device the IOP is to be sent to.
 * @param ioctl Which IOCtl is to be run.
 * @param mdl MDL for the output buffer.
 * @param size Size of the output buffer.
 */
iop_t *iop_new_ioctl(device_t *dev, iop_ioctl_t ioctl, vm_mdl_t *mdl,
    size_t size);

/*!
 * @brief Allocate & set up a read IOP.
 *
 * @param dev Device the IOP is to be sent to.
 * @param mdl MDL for the output buffer.
 * @param size Size in bytes to read.
 */
iop_t *iop_new_read(device_t *dev, vm_mdl_t *mdl, size_t size, io_off_t off);

/*!
 * @brief Allocate & set up a write IOP.
 *
 * @param dev Device the IOP is to be sent to.
 * @param mdl MDL for the output buffer.
 * @param size Size in bytes to read.
 */
iop_t *iop_new_write(device_t *dev, vm_mdl_t *mdl, size_t size, io_off_t off);

/*! @brief Send and await completion of an IOP. */
iop_return_t iop_send_sync(iop_t *iop);

/*!
 * @brief Continue processing an IOP.
 *
 * This is also privately used by iop_send() and iop_send_synch() to begin an
 * IOP, they pass -1 as \p res since they aren't actually continuing anything.
 */
iop_return_t iop_continue(iop_t *iop, iop_return_t res);

/*!
 * @brief Return a pointer to the current frame of an IOP.
 */
inline iop_frame_t *
iop_stack_current(iop_t *iop)
{
	return &iop->stack[iop->stack_current];
}

/*!
 * @brief Initialise the next frame in an IOP to target the next lower device.
 *
 * The frame is empty, apart from the device being set to the next-lower device
 * (i.e. the provider of iop_stack_current(iop)'s device) and the vnode pointer
 * being copied in.
 */
iop_frame_t *iop_stack_initialise_next(iop_t *iop);

/*!
 * @brief Set up an IOP frame for an I/O Control request.
 *
 * @param ioctl Which IOCtl is to be run.
 * @param buf_or_mdl  System buffer or MDL for the main input/output buffer.
 * Which it is is determined by the IOCtl number.
 * @param size Size of the output buffer if relevant.
 */
void iop_frame_setup_ioctl(iop_frame_t *frame, iop_ioctl_t ioctl,
    void *buf_or_mdl, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* KRX_KDK_DEVMGR_H */
