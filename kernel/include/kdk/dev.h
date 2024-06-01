#ifndef KRX_KDK_DEV_H
#define KRX_KDK_DEV_H

#include "kdk/nanokern.h"
#include "kdk/vm.h"

/*! Count of blocks. */
typedef uint64_t io_blksize_t;
/*! Block offset. */
typedef int64_t io_blkoff_t;
/*! Byte offset. */
typedef int64_t io_off_t;

struct vnode;

#ifdef __OBJC__
@class DKDevice;
#else
typedef void *DKDevice;
#endif

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

	/*!
	 * Enqueue FUSE request. iop_frame::kbuf will point to io_fuse_request.
	 */
	kIOCTLFuseEnqueuRequest,
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

/*!
 * A frame in an I/O packet stack.
 */
typedef struct iop_frame {
	/*! Which function is being carried out? (located here for packing)*/
	iop_function_t function: 8;
	/* Is the buffer a buffer (not an MDL?) */
	bool has_kbuf;

	/*! Which device processes this stack entry? */
	DKDevice *dev;
	/*! VNode? */
	struct vnode *vnode;
	/*! MDL or wired kernel buffer for principal input/out */
	union {
		vm_mdl_t *mdl;
		void *kbuf;
	};
	/*! Function-specific data. */
	union {
		struct iop_stack_data_rw rw;
		struct iop_stack_data_ioctl ioctl;
		struct iop_stack_data_scsi scsi;
		struct iop_stack_data_9p ninep;
		struct iop_stack_data_connect connect;
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
	void *dev;
	/*! Linkage in a device queue. */
	TAILQ_ENTRY(iop) dev_queue_entry;

	unsigned
	    /*! Has the IOP started running? */
	    begun ;

	/*! Which direction is it currently travelling? */
	iop_direction_t direction;

	/*! If this is an associated IOP, this points to its master. */
	struct iop *master_iop;
	/*! If this is an associated IOP, its linkage. */
	SLIST_ENTRY(iop) associated_iops_link;

	/*!
	 * Associated IOPs. These are associated with an IOP during dispatch at
	 * one frame, and if present, they are ran when the IOP is
	 * dispatch function, and if they are present, then they will all be ran
	 * and only when they are all finished will the completion of this IOP
	 * be ran.
	 */
	SLIST_HEAD(, iop) associated_iops;
	/*! Number of pending associated IOPs. */
	uint8_t n_pending_associated_iops;

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

typedef TAILQ_HEAD(iop_queue, iop) iop_queue_t;

/*!
 * @brief Allocate and set up an empty IOP which must be further initialised.
 *
 * @param dev Device the IOP is to be sent to. IOP Stack size is set
 * accordingly.
 */
iop_t *iop_new(DKDevice *dev);

/*!
 * @brief (re-)initialise an IOP.
 */
void iop_init(iop_t *iop);

/*!
 * @brief Free an IOP.
 */
void iop_free(iop_t *iop);

/*!
 * @brief Initialise an IOP for SCSI.
 *
 * @param iop IOP to init (should be freshly allocated, or have completed)
 * @param dev Device the IOP is to be sent to.
 * @param srb SCSI request block.
 */
void iop_init_scsi(iop_t *iop, DKDevice *dev, struct _SCSI_REQUEST_BLOCK *srb);

/*!
 * @brief Allocate & set up an IOCtl IOP.
 *
 * @param dev Device the IOP is to be sent to.
 * @param ioctl Which IOCtl is to be run.
 * @param mdl MDL for the output buffer.
 * @param size Size of the output buffer.
 */
iop_t *iop_new_ioctl(DKDevice *dev, iop_ioctl_t ioctl, vm_mdl_t *mdl,
    size_t size);

/*!
 * @brief Allocate & set up a read IOP.
 *
 * @param dev Device the IOP is to be sent to.
 * @param mdl MDL for the output buffer.
 * @param size Size in bytes to read.
 */
iop_t *iop_new_read(DKDevice *dev, vm_mdl_t *mdl, size_t size, io_off_t off);

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
 * @brief Allocate & set up a write IOP.
 *
 * @param dev Device the IOP is to be sent to.
 * @param mdl MDL for the output buffer.
 * @param size Size in bytes to read.
 */
iop_t *iop_new_write(DKDevice *dev, vm_mdl_t *mdl, size_t size, io_off_t off);

/*!
 * @brief Allocate & set up a SCSI IOP.
 *
 * @param dev Device the IOP is to be sent to.
 * @param srb SCSI request block.
 */
iop_t *iop_new_scsi(DKDevice *dev, struct _SCSI_REQUEST_BLOCK *srb);

/*!
 * @brief Allocate & set up a 9p IOP.
 *
 * @param dev Device the IOP is to be sent to.
 * @param buf_in 9p in-buffer.
 * @param buf_out 9p out-buffer.
 * @param mdl Optional MDL.
 */
iop_t *iop_new_9p(DKDevice *dev, struct ninep_buf *in, struct ninep_buf *out,
    vm_mdl_t *mdl);

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
static inline iop_frame_t *
iop_stack_current(iop_t *iop)
{
	return &iop->stack[iop->stack_current];
}

/*!
 * @brief Return a pointer to the previous (lower) frame of an IOP.
 */
static inline iop_frame_t *
iop_stack_previous(iop_t *iop)
{
	kassert(iop->stack_count > iop->stack_current + 1);
	return &iop->stack[iop->stack_current + 1];
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
 * @brief Setup an IOP frame for a 9p request.
 */
void iop_frame_setup_9p(iop_frame_t *frame, struct ninep_buf *in,
    struct ninep_buf *out, vm_mdl_t *mdl);

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

/*!
 * @brief Set up an IOP frame for a SCSI request.
 *
 * @param srb SCSI request block pointer.
 */
void iop_frame_setup_srb(iop_frame_t *frame, struct _SCSI_REQUEST_BLOCK *srb);

#endif /* KRX_KDK_DEV_H */
