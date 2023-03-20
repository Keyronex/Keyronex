/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Feb 22 2023.
 */

#include "bsdqueue/queue.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/process.h"

static kspinlock_t devmgr_lock = KSPINLOCK_INITIALISER;

void
dev_attach(device_t *consumer, device_t *provider)
{
	kassert(consumer != NULL);

	TAILQ_INIT(&consumer->consumers);
	consumer->provider = provider;

	if (!provider) {
		consumer->provider = NULL;
		consumer->stack_depth = 1;
	} else {
		ipl_t ipl;
		consumer->provider = provider;
		ipl = ke_spinlock_acquire(&devmgr_lock);
		TAILQ_INSERT_TAIL(&provider->consumers, consumer,
		    consumers_link);
		consumer->stack_depth = provider->stack_depth + 1;
		ke_spinlock_release(&devmgr_lock, ipl);
	}
}

#define IOP_SIZE(DEPTH) (sizeof(iop_t) + sizeof(iop_frame_t) * DEPTH)

iop_t *
iop_new(device_t *dev)
{
	uint8_t depth = dev->stack_depth;
	iop_t *iop = kmem_alloc(IOP_SIZE(depth));

	iop->stack_count = depth;
	iop->direction = kIOPDown;
	iop->stack_current = -1;
	ke_event_init(&iop->event, false);
	iop->master_iop = NULL;
	iop->begun = true;

	return iop;
}

iop_t *
iop_new_ioctl(device_t *dev, iop_ioctl_t ioctl, vm_mdl_t *mdl, size_t size)
{
	iop_t *iop = iop_new(dev);
	iop_frame_t *frame = &iop->stack[0];

	frame->dev = dev;
	iop_frame_setup_ioctl(frame, ioctl, mdl, size);

	return iop;
}

iop_t *
iop_new_read(device_t *dev, vm_mdl_t *mdl, size_t size, io_off_t off)
{
	iop_t *iop = iop_new(dev);

	iop->stack[0].dev = dev;
	iop->stack[0].function = kIOPTypeRead;
	iop->stack[0].mdl = mdl;
	iop->stack[0].read.bytes = size;
	iop->stack[0].read.offset = off;

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
		kassert(iop->stack_current < iop->stack_count);
		frame = iop_stack_current(iop);

		if (!SLIST_EMPTY(&frame->associated_iops)) {
			/* begin running associated IOPs */
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
		if (frame->dev == NULL)
			frame->dev =
			    iop->stack[iop->stack_current - 1].dev->provider;

		kassert(frame->dev != NULL);
		kassert(frame->dev->dispatch != NULL);

		r = frame->dev->dispatch(frame->dev, iop);

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
		frame = iop_stack_current(iop);

		/*! associated IOPs should all be gone */
		kassert(SLIST_EMPTY(&frame->associated_iops));

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

		kassert(frame->dev->complete != NULL);
		r = frame->dev->complete(frame->dev, iop);

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
	frame->dev = old->dev->provider;
	frame->vnode = old->vnode;

	return frame;
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