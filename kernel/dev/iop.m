#include "ddk/DKDevice.h"
#include "kdk/dev.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/vfs.h"

#define kdprintf kprintf
#define IOP_SIZE(DEPTH) (sizeof(iop_t) + sizeof(iop_frame_t) * DEPTH)

void
iop_init(iop_t *iop)
{
	iop->direction = kIOPDown;
	iop->stack_current = -1;
	ke_event_init(&iop->event, false);
	iop->master_iop = NULL;
	iop->begun = true;
	iop->associated_iops_link.sle_next = NULL;
	memset(iop->stack, 0x0, sizeof(iop_frame_t) * iop->stack_count);
}

iop_t *
iop_new(DKDevice *dev)
{
	uint8_t depth = 10;//dev->m_stackDepth;
	iop_t *iop = kmem_zalloc(IOP_SIZE(depth));
	iop->stack_count = depth;
	iop_init(iop);
	return iop;
}

void
iop_free(iop_t *iop)
{
	kmem_free(iop, IOP_SIZE(iop->stack_count));
}


void iop_init_scsi(iop_t *iop, DKDevice *dev, struct _SCSI_REQUEST_BLOCK *srb)
{
	iop_init(iop);
	iop->stack[0].dev = dev;
	iop->stack[0].function = kIOPTypeWrite;
	iop->stack[0].scsi.srb = srb;
}

iop_t *
iop_new_ioctl(DKDevice *dev, iop_ioctl_t ioctl, vm_mdl_t *mdl, size_t size)
{
	iop_t *iop = iop_new(dev);
	iop_frame_t *frame = &iop->stack[0];

	frame->dev = dev;
	iop_frame_setup_ioctl(frame, ioctl, mdl, size);

	return iop;
}

iop_t *
iop_new_read(DKDevice *dev, vm_mdl_t *mdl, size_t size, io_off_t off)
{
	iop_t *iop = iop_new(dev);

	iop->stack[0].dev = dev;
	iop->stack[0].function = kIOPTypeRead;
	iop->stack[0].mdl = mdl;
	iop->stack[0].rw.bytes = size;
	iop->stack[0].rw.offset = off;

	return iop;
}

iop_t *
iop_new_vnode_read(struct vnode *vnode, vm_mdl_t *mdl, size_t size,
    io_off_t off)
{
	DKDevice *dev = vnode->vfs->device;
	iop_t *iop = iop_new(dev);

	iop->stack[0].vnode = vnode;
	iop->stack[0].dev = dev;
	iop->stack[0].function = kIOPTypeRead;
	iop->stack[0].mdl = mdl;
	iop->stack[0].rw.bytes = size;
	iop->stack[0].rw.offset = off;

	return iop;
}

iop_t *
iop_new_vnode_write(struct vnode *vnode, vm_mdl_t *mdl, size_t size,
    io_off_t off)
{
	DKDevice *dev = vnode->vfs->device;
	iop_t *iop = iop_new(dev);

	iop->stack[0].vnode = vnode;
	iop->stack[0].dev = dev;
	iop->stack[0].function = kIOPTypeWrite;
	iop->stack[0].mdl = mdl;
	iop->stack[0].rw.bytes = size;
	iop->stack[0].rw.offset = off;

	return iop;
}

iop_t *
iop_new_write(DKDevice *dev, vm_mdl_t *mdl, size_t size, io_off_t off)
{
	iop_t *iop = iop_new(dev);

	iop->stack[0].dev = dev;
	iop->stack[0].function = kIOPTypeWrite;
	iop->stack[0].mdl = mdl;
	iop->stack[0].rw.bytes = size;
	iop->stack[0].rw.offset = off;

	return iop;
}

iop_t *iop_new_scsi(DKDevice *dev, struct _SCSI_REQUEST_BLOCK *srb)
{
	iop_t *iop = iop_new(dev);
	iop_init_scsi(iop, dev, srb);
	return iop;
}

iop_t *iop_new_9p(DKDevice *dev, struct ninep_buf *in, struct ninep_buf *out, vm_mdl_t *mdl)
{
	iop_t *iop = iop_new(dev);
	iop_frame_t *frame = &iop->stack[0];

	iop->stack[0].dev = dev;
	iop_frame_setup_9p(frame, in, out, mdl);

	return iop;
}

iop_return_t
iop_continue(iop_t *iop, iop_return_t res)
{
	iop_return_t r;
	iop_frame_t *frame;

start:
#if DEBUG_DEVMAN == 1
	if (res == -1)
		kdprintf("devmgr: IOP %p (%d frames) begins\n", iop,
		    iop->stack_count);
#endif

	/* note this can be set multiple times - it doesn't matter. */
	iop->begun = true;

	/* it's meaningless for a driver to call the continuation when it's not
	 * actually completed its dispatch/completion. */
	kassert(res != kIOPRetPending);

	/* res is only set to -1 if this is not actually a continuation, but the
	 * initial send of the IOP */
	if (res != -1) {
		r = res;
		kassert(iop->stack_current != -1 &&
		    iop->stack_current < iop->stack_count);
		frame = iop_stack_current(iop);
		if (iop->direction == kIOPDown)
			goto continuation_down;
		else
			goto continuation_up;
	}

cont:
	if (iop->direction == kIOPDown) {
		/* travelling down the stack. */
		iop->stack_current++;
		kassert(iop->stack_current < iop->stack_count &&
		    iop->stack_current > -1);
		frame = iop_stack_current(iop);

		if (!SLIST_EMPTY(&frame->associated_iops)) {
			/* begin running associated IOPs */
			kdprintf("Beginning associated IOP 1.\n");
			iop = SLIST_FIRST(&frame->associated_iops);
			kassert(!iop->begun);
			goto start;
		}

		/* when a new device is not explicitly set, the provider is
		 * default. when a device has explicitly called
		 * iop_skip_current(), that function sets frame->dev to the
		 * next-lower device already.
		 *
		 * frame 0's device is explicitly set by the iop_new() function
		 * and all its derivatives
		 */
		if (frame->dev == NULL) {
			kassert(iop->stack_current > 0);
			frame->dev =
			    iop->stack[iop->stack_current - 1].dev->m_provider;
		}

		kassert(frame->dev != NULL);
		//kassert(frame->dev->dispatch != NULL);

		r = [frame->dev dispatchIOP:iop];
		//r = frame->dev->dispatch(frame->dev, iop);

	continuation_down:
		switch (r) {
		case kIOPRetCompleted:
			iop->direction = kIOPUp;
			goto cont;

		case kIOPRetContinue:
			goto cont;

		/*
		 * the dispatch function will complete the IOP later; wait for
		 * that.
		 */
		case kIOPRetPending: {
			/* if this has a master IOP, try to start next
			 * associated IOP... */
			if (iop->master_iop != NULL) {
				iop_t *next = SLIST_NEXT(iop,
				    associated_iops_link);
				if (!next->begun) {
					kdprintf(
					    "Beginning associated IOP 2.\n");
					iop = next;
					res = -1;
					goto start;
				}
			}
			return r;
		}
		}
	} else {
		/* travelling back up the stack. */
		iop->stack_current--;
		kassert(iop->stack_current > -2);
		frame = iop_stack_current(iop);

		if (iop->stack_current == -1) {
			/*! if the last frame we completed was the zeroth entry
			 * on the stack, the IOP has now been approved by all
			 * the completions and we can complete it altogether. */

			/* if this has a master IOP, atomically decrement number
			 * of pending associated IOPs in the master. if that =
			 * 0, continue master iop. otherwise, and try to start
			 * next associated IOP if not yet started and exists
			 */
			if (iop->master_iop != NULL) {
				iop_frame_t *master_frame = iop_stack_current(
				    iop->master_iop);
				uint8_t n_pending = __atomic_sub_fetch(
				    &master_frame->n_pending_associated_iops, 1,
				    __ATOMIC_SEQ_CST);
				if (n_pending == 0) {
					/* there are no more pending associated
					 * IOPs. We set res to -1, direction to
					 * up, and increment stack_current. Then
					 * we jump to the start. This causes the
					 * function to invoke any completion
					 * associated with the frame that
					 * associated these IOPs with the master
					 * IOP. */
					res = -1;
					iop->master_iop->direction = kIOPUp;
					iop->master_iop->stack_count++;
					goto start;
				} else {
					/* We may need to start the next
					 * associated IOP. */
					iop_t *next = SLIST_NEXT(iop,
					    associated_iops_link);
					if (!next->begun) {
						kdprintf(
						    "Beginning associated IOP 3.\n");
						iop = next;
						res = -1;
						goto start;
					}
				}
			} else {
#if DEBUG_DEVMAN == 1
				kdprintf("devmgr: IOP %p completes\n", iop);
#endif
				ke_event_signal(&iop->event);
				return kIOPRetCompleted;
			}
		}

		/*! associated IOPs should all be gone */
		kassert(SLIST_EMPTY(&frame->associated_iops));

		r = [frame->dev completeIOP: iop];

	continuation_up:
		switch (r) {
		/* this completion is happy and we can continue to move up the
		 * stack. */
		case kIOPRetCompleted:
			goto cont;

		/* go back to moving down the stack, and arrange so that the
		 * frame that returned this value will be the frame that is
		 * dispatched next */
		case kIOPRetContinue: {
			iop->stack_current--;
			iop->direction = kIOPDown;
			goto cont;
		}

		/* completion can't immediately return, so wait for it to call
		 * iop_continue with its result later. */
		case kIOPRetPending: {
			/* if this has a master IOP, try to start next
			 * associated IOP if it exists & not yet started... */
			if (iop->master_iop != NULL) {
				/* We may need to start the next
				 * associated IOP. */
				iop_t *next = SLIST_NEXT(iop,
				    associated_iops_link);
				if (!next->begun) {
					kdprintf(
					    "Beginning associated IOP 4.\n");
					iop = next;
					res = -1;
					goto start;
				}
			}

			return r;
		}
		} /* switch (r) */
	}	  /* if() */

	kfatal("unreached\n");
	return -1;
}

iop_frame_t *
iop_stack_initialise_next(iop_t *iop)
{
	iop_frame_t *old = iop_stack_current(iop),
		    *frame = &iop->stack[iop->stack_current + 1];

	memset(frame, 0x0, sizeof(*frame));
	frame->dev = old->dev->m_provider;
	frame->vnode = old->vnode;

	return frame;
}

void
iop_frame_setup_9p(iop_frame_t *frame, struct ninep_buf *in,
    struct ninep_buf *out, vm_mdl_t *mdl)
{
	frame->function = kIOPType9p;
	frame->mdl = mdl;
	frame->ninep.ninep_in = in;
	frame->ninep.ninep_out = out;
}

void
iop_frame_setup_ioctl(iop_frame_t *frame, iop_ioctl_t ioctl, void *buf_or_mdl,
    size_t size)
{
	frame->function = kIOPTypeIOCtl;
	frame->mdl = buf_or_mdl;
	frame->ioctl.type = ioctl;
	frame->ioctl.out_buffer_size = size;
}

/*! @brief Send and await completion of an IOP. */
iop_return_t
iop_send_sync(iop_t *iop)
{
	iop_return_t r = iop_continue(iop, -1);
	switch (r) {
	case kIOPRetCompleted:
		return r;

	case kIOPRetPending:
		ke_wait(&iop->event, "io_send_sync:iop->event", false, false,
		    -1);
		// return the result from the IOP somewhere??
		return 0;

	default:
		kfatal("%d should never be returned from iop_continue", r);
	}
}
