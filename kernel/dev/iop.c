/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Wed Nov 13 2024.
 */

#include <kdk/iop.h>
#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <kdk/vfs.h>

static inline size_t
iop_size(size_t depth)
{
	return sizeof(iop_t) + sizeof(iop_frame_t) * depth;
}

void
iop_init(iop_t *iop)
{
	iop->stack_current = -1;
	iop->direction = kIOPDown;
	iop->master_iop = NULL;
	iop->begun = false;
	SLIST_INIT(&iop->slave_iops);
	iop->incomplete_slave_iops_n = 0;
}

iop_t *
iop_new(vnode_t *vp, iop_function_t op)
{
	iop_t *iop;
	size_t depth = vp->ops->iop_stack_depth(vp);

	iop = kmem_alloc(iop_size(depth));
	iop->stack_count = depth;
	iop_init(iop);

	return iop;
}

iop_t *
iop_new_vnode_read(struct vnode *vnode, vm_mdl_t *mdl, size_t size,
    io_off_t off)
{
	iop_t *iop = iop_new(vnode, kIOPTypeRead);
	iop->stack[0].function = kIOPTypeRead;
	iop->stack[0].vnode = vnode;
	iop->stack[0].mdl = mdl;
	iop->stack[0].rw.bytes = size;
	iop->stack[0].rw.offset = off;
	return iop;
}

iop_t *
iop_new_vnode_write(struct vnode *vnode, vm_mdl_t *mdl, size_t size,
    io_off_t off)
{
	iop_t *iop = iop_new(vnode, kIOPTypeWrite);
	iop->stack[0].function = kIOPTypeWrite;
	iop->stack[0].vnode = vnode;
	iop->stack[0].mdl = mdl;
	iop->stack[0].rw.bytes = size;
	iop->stack[0].rw.offset = off;
	return iop;
}

iop_t *
iop_new_9p(struct vnode *vnode, struct ninep_buf *in, struct ninep_buf *out,
    vm_mdl_t *mdl)
{
	iop_t *iop = iop_new(vnode, kIOPType9p);
	iop->stack[0].function = kIOPType9p;
	iop->stack[0].vnode = vnode;
	iop->stack[0].mdl = mdl;
	iop->stack[0].ninep.ninep_in = in;
	iop->stack[0].ninep.ninep_out = out;
	return iop;
}

void
iop_append_slave(iop_t *master, iop_t *slave)
{
	SLIST_INSERT_HEAD(&master->slave_iops, slave, slave_iop_qlink);
	slave->master_iop = master;
	__atomic_fetch_add(&master->incomplete_slave_iops_n, 1,
	    __ATOMIC_SEQ_CST);
}

#ifndef _KERNEL
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
	ke_event_signal(iop->event);
}

iop_return_t
iop_continue(iop_t *iop, iop_return_t status)
{
	if (status != kIOPRetBegin) {
		kassert(iop->stack_current < iop->stack_count &&
		    iop->stack_current >= -1);

		/*
		 * Continuing an IOP is treated by going to the point in the
		 * loop where we would be, had we just returned from the
		 * dispatch or completion routine..
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
				kassert(!__atomic_load_n(&iop->begun,
				    __ATOMIC_SEQ_CST));
				trace_iop_slave_begin(iop);
				continue;
			}

			vp = iop_stack_current(iop)->vnode;
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
					    __atomic_compare_exchange_n(
						&next->begun, &expect, true,
						false, __ATOMIC_SEQ_CST,
						__ATOMIC_SEQ_CST)) {
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

				if (__atomic_fetch_sub(
					&master_iop->incomplete_slave_iops_n,
					1, __ATOMIC_SEQ_CST) == 1) {
					iop = iop->master_iop;
					iop->direction = kIOPUp;
					trace_iop_master_continue(iop);
					continue;
				} else {
					iop_t *next = SLIST_NEXT(iop,
					    slave_iop_qlink);
					bool expect = false;

					if (next != NULL &&
					    __atomic_compare_exchange_n(
						&next->begun, &expect, true,
						false, __ATOMIC_SEQ_CST,
						__ATOMIC_SEQ_CST)) {
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
			kassert(__atomic_load_n(&iop->incomplete_slave_iops_n,
			    __ATOMIC_SEQ_CST) == 0);

			vp = iop_stack_current(iop)->vnode;
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
					    __atomic_compare_exchange_n(
						&next->begun, &expect, true,
						false, __ATOMIC_SEQ_CST,
						__ATOMIC_SEQ_CST)) {
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

io_result_t
iop_send_sync(iop_t *iop)
{
	io_result_t result;
	iop_return_t status;
	kevent_t event;

	ke_event_init(&event, false);
	iop->event = &event;
	iop->result = &result;

	status = iop_continue(iop, kIOPRetBegin);
	if (status != kIOPRetCompleted)
		ke_wait(&event, "iop_sync", false, false, -1);

	return result;;
}

void
iop_free(iop_t *iop)
{
	kmem_free(iop, sizeof(iop_t) + sizeof(iop_frame_t) * iop->stack_count);
}

void
iop_frame_setup_9p(struct vnode *vnode, iop_frame_t *frame,
    struct ninep_buf *in, struct ninep_buf *out, vm_mdl_t *mdl)
{
	frame->function = kIOPType9p;
	frame->vnode = vnode;
	frame->mdl = mdl;
	frame->ninep.ninep_in = in;
	frame->ninep.ninep_out = out;
	frame->has_kbuf = false;
}
