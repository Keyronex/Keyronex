/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2022-26 Cloudarox Solutions.
 */
/*!
 * @file iop.h
 * @brief I/O packets dispatch.
 */

#include <sys/iop.h>
#include <sys/kmem.h>
#include <sys/vnode.h>
#include "sys/k_wait.h"

static iop_t *
iop_new(vnode_t *vp)
{
	iop_t *iop;
	size_t depth = vp->ops->stack_depth;

	iop = kmem_alloc(sizeof(iop_t) + sizeof(iop_frame_t) * depth);
	iop->stack_count = depth;
	iop->stack_current = -1;
	iop->direction = kIOPDown;
	iop->master_iop = NULL;
	atomic_init(&iop->begun, false);
	SLIST_INIT(&iop->slave_iops);
	atomic_init(&iop->incomplete_slave_iops_n, 0);

	return iop;
}

iop_t *
iop_new_read(struct vnode *vp, sg_list_t *sgl, size_t sgl_offset, size_t length,
    io_off_t offset)
{
	iop_t *iop = iop_new(vp);
	iop->stack[0].op = kIOPRead;
	iop->stack[0].vp = vp;
	iop->stack[0].sglist = sgl;
	iop->stack[0].sglist_offset = sgl_offset;
	iop->stack[0].rw.length = length;
	iop->stack[0].rw.offset = offset;
	return iop;
}

iop_t *
iop_new_write(struct vnode *vp, sg_list_t *sgl, size_t sgl_offset,
    size_t length, io_off_t offset)
{
	iop_t *iop = iop_new(vp);
	iop->stack[0].op = kIOPWrite;
	iop->stack[0].vp = vp;
	iop->stack[0].sglist = sgl;
	iop->stack[0].sglist_offset = sgl_offset;
	iop->stack[0].rw.length = length;
	iop->stack[0].rw.offset = offset;
	return iop;
}

iop_t *
iop_new_9p(vnode_t *vn, struct ninep_buf *in, struct ninep_buf *out,
    sg_list_t *sglist)
{
	iop_t *iop = iop_new(vn);
	iop->stack[0].op = kIOP9p;
	iop->stack[0].vp = vn;
	iop->stack[0].ninep.ninep_in = in;
	iop->stack[0].ninep.ninep_out = out;
	iop->stack[0].sglist = sglist;
	iop->stack[0].sglist_offset = 0;
	return iop;
}

void
iop_append_slave(iop_t *master, iop_t *slave)
{
	SLIST_INSERT_HEAD(&master->slave_iops, slave, slave_iop_qlink);
	slave->master_iop = master;
	atomic_fetch_add(&master->incomplete_slave_iops_n, 1);
}

#if 0
#define trace_iop_slave_begin(iop) \
	printf("Beginning slave IOP for %s\n", iop->dev->name)
#define trace_dev_no_completion(dev) \
	printf("Device %s has no completion\n", dev->name)
#define trace_iop_slave_state(iop, state) \
	printf("Slave IOP for %s is %s\n", iop->dev->name, state)
#define trace_iop_master_continue(iop) \
	printf("Continuing master IOP for %s\n", iop->dev->name)
#define trace_iop_complete(iop) \
	printf("Completing IOP for %s\n", iop->dev->name)
#else
#define trace_iop_slave_begin(iop)
#define trace_dev_no_completion(dev)
#define trace_iop_slave_state(iop, state)
#define trace_iop_master_continue(iop)
#define trace_iop_complete(iop)
#endif

static void
do_complete(iop_t *iop)
{
	ke_event_set_signalled(iop->event, true);
}

iop_return_t
iop_continue(iop_t *iop, iop_return_t status)
{
	if (status != kIOPRetBegin) {
		kassert(iop->stack_current < iop->stack_count &&
		    iop->stack_current >= -1);

		/*
		 * Continuing an IOP is treated by going to the point in the loop
		 * where we would be, had we just returned from the dispatch or
		 * completion routine..
		 */

		if (iop->direction == kIOPDown)
			goto continuation_down;
		else
			goto continuation_up;
	}

	while (true) {
		if (iop->direction == kIOPDown) {
			vnode_t *vp;

			iop->stack_current++;
			kassert(iop->stack_current > -1 &&
			    iop->stack_current < iop->stack_count);

			/*
			 * In place of a frame proper, the previous dispatch or
			 * completion routine may have appended slave IOPs.
			 * Carry these out.
			 */
			if (!SLIST_EMPTY(&iop->slave_iops)) {
				iop = SLIST_FIRST(&iop->slave_iops);
				kassert(!atomic_load(&iop->begun));
				trace_iop_slave_begin(iop);
				continue;
			}

			vp = iop_current_frame(iop)->vp;
			status = vp->ops->iop_dispatch(vp, iop);

		continuation_down:
			switch (status) {
			case kIOPRetCompleted:
				/*
				 * The dispatch function said it's completed the
				 * work. Start moving up the stack.
				 */
				iop->direction = kIOPUp;
				continue;

			case kIOPRetContinue:
				/*
				 * The dispatch function has provided either a
				 * new frame to process, or has appended slave
				 * IOPs. In either case, continue moving down
				 * the stack.
				 */
				continue;

			case kIOPRetPending: {
				/*
				 * This device is doing asynchronous work. We
				 * need to check if we can start further slave
				 * IOPs.
				 *
				 * TODO: I think we need synchronisation of some
				 * sort here! Same with other places where we
				 * start "sibling" slave IOPs.
				 *
				 * There can be a race when a device returns
				 * pending but before we finish here and
				 * return/kick off the next slave IOP, this
				 * selfsame slave IOP is completed somewhere
				 * else, and perhaps is even deallocated,
				 * reused, or the master's slave_iops list is
				 * modified.
				 *
				 * Solution might be some sort of flag kept in
				 * *this* IOP, and set while the device is
				 * deciding what to do with the IOP (and hence
				 * holds some lock or is naturally synchronised
				 * i.e. is queue-per-CPU) and that is unset when
				 * we are done looking at the master IOP.
				 *
				 * I.e. when sibling slave IOPs are pending, the
				 * device dispatch/completion function sets this
				 * flag, and we unset it here. And while that
				 * flag is set, any racing completer spins until
				 * it is unset.
				 */

				if (iop->master_iop != NULL) {
					iop_t *next = SLIST_NEXT(iop,
					    slave_iop_qlink);
					bool expect = false;

					trace_iop_slave_state(iop, "pending");

					if (next != NULL &&
					    atomic_compare_exchange_strong(
						&next->begun, &expect, true)) {
						iop = next;
						trace_iop_slave_begin(iop);
						continue;
					} else {
						return kIOPRetPending;
					}
				} else {
					/*
					 * There is no master IOP - so just
					 * return pending so the caller can
					 * wait.
					 */
					return kIOPRetPending;
				}
			}

			/* GCOVR_EXCL_START */
			default:
				kfatal("Illegal return from dispatch\n");
			/* GCOVR_EXCL_STOP */
			}
		} else /* iop->direction == kIOPUp */ {
			vnode_t *vp;

			iop->stack_current--;
			kassert(iop->stack_current >= -1 &&
			    iop->stack_current < iop->stack_count);

			if (iop->stack_current == -1 &&
			    iop->master_iop != NULL) {
				iop_t *master_iop = iop->master_iop;

				/*
				 * This slave IOP has been completed, with every
				 * completion agreeing.
				 *
				 * We need to decrement the count of pending
				 * slave IOPs for our master.
				 *
				 * If that reaches 0, then it is our duty to
				 * continue the master IOP.
				 *
				 * Otherwise, it may be the case that we need
				 * to start the next slave IOP. (This is what
				 * happens if the slave IOP was processed in one
				 * go through this function and never returned
				 * kIOPRetPending.)
				 */

				trace_iop_slave_state(iop, "complete");

				if (atomic_fetch_sub(
					&master_iop->incomplete_slave_iops_n,
					1) == 1) {
					iop = iop->master_iop;
					iop->direction = kIOPUp;
					trace_iop_master_continue(iop);
					continue;
				} else {
					iop_t *next = SLIST_NEXT(iop,
					    slave_iop_qlink);
					bool expect = false;

					if (next != NULL &&
					    atomic_compare_exchange_strong(
						&next->begun, &expect, true)) {
						iop = next;
						trace_iop_slave_begin(iop);
						continue;
					} else {
						return kIOPRetCompleted;
					}
				}
			} else if (iop->stack_current == -1) {
				/* We are at the top of the stack. */
				do_complete(iop);
				return kIOPRetCompleted;
			}

			/* By this point, there should be no slave IOPs. */
			kassert(atomic_load(&iop->incomplete_slave_iops_n) == 0);

			vp = iop_current_frame(iop)->vp;
			if (vp->ops->iop_complete == NULL) {
				trace_dev_no_completion(vp);
				status = kIOPRetCompleted;
			} else {
				status = vp->ops->iop_complete(vp, iop);
			}

		continuation_up:
			switch (status) {
			case kIOPRetCompleted:
				/*
				 * This completion is happy. Continue moving up
				 * the stack.
				 */
				continue;

			case kIOPRetContinue:
				/*
				 * This completion wants to do more work. Start
				 * moving down the stack again.
				 */
				iop->direction = kIOPDown;
				continue;

			case kIOPRetPending:
				/*
				 * This device is doing asynchronous completion
				 * work. We need to check if we can start
				 * further slave IOPs belonging to the same
				 * master.
				 */

				if (iop->master_iop != NULL) {
					iop_t *next = SLIST_NEXT(iop,
					    slave_iop_qlink);
					bool expect = false;

					trace_iop_slave_state(iop,
					    "completion pending");

					if (next != NULL &&
					    atomic_compare_exchange_strong(
						&next->begun, &expect, true)) {
						iop = next;
						trace_iop_slave_begin(iop);
						continue;
					} else {
						return kIOPRetPending;
					}
				} else {
					/*
					 * There is no master IOP - so just
					 * return pending so the caller can
					 * wait.
					 */
					return kIOPRetPending;
				}

			/* GCOVR_EXCL_START */
			default:
				kfatal("Illegal return from completion\n");
			/* GCOVR_EXCL_STOP */
			}
		}
	}

	kfatal("Reached unreachable point\n");
}

iop_result_t
iop_send_sync(iop_t *iop)
{
	iop_return_t status;
	kevent_t event;

	ke_event_init(&event, false);
	iop->event = &event;
	status = iop_continue(iop, kIOPRetBegin);
	if (status != kIOPRetCompleted)
		ke_wait1(&event, "iop_send_sync", 0, ABSTIME_FOREVER);
	return iop->result;
}

void
iop_free(iop_t *iop)
{
	kmem_free(iop, sizeof(iop_t) + sizeof(iop_frame_t) * iop->stack_count);
}
