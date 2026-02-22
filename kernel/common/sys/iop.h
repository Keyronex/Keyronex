/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2022-26 Cloudarox Solutions.
 */
/*!
 * @file iop.h
 * @brief I/O packets.
 */

#ifndef ECX_SYS_IOP_H
#define ECX_SYS_IOP_H

#include <sys/k_log.h>
#include <sys/k_types.h>
#include <sys/krx_atomic.h>
#include <sys/vm.h>

#include <libkern/queue.h>

struct kevent;

typedef int64_t io_off_t;
typedef uint64_t iop_result_t;

typedef struct sg_seg {
	paddr_t paddr;
	size_t length;
} sg_seg_t;

typedef struct sglist {
	size_t elems_n;
	sg_seg_t *elems;
} sg_list_t;

typedef enum iop_return {
	kIOPRetCompleted,
	kIOPRetPending,
	kIOPRetContinue,
	kIOPRetBegin,
} iop_return_t;

typedef enum iop_direction {
	kIOPDown,
	kIOPUp,
} iop_direction_t;

typedef enum iop_op {
	kIOPRead,
	kIOPWrite,
	kIOP9p,
} iop_op_t;

typedef struct iop_frame {
	iop_op_t op: 8;
	bool sglist_write;
	struct vnode *vp;
	union {
		vaddr_t vaddr;
		struct {
			sg_list_t *sglist;
			size_t sglist_offset;
		};
	};
	union {
		struct iop_frame_rw {
			uint64_t offset;
			uint64_t length;
		} rw;
		struct iop_frame_9p {
			struct ninep_buf *ninep_in;
			struct ninep_buf *ninep_out;
		} ninep;
	};
} iop_frame_t;

typedef struct iop {
	TAILQ_ENTRY(iop) dev_qlink;
	iop_result_t	result;
	struct kevent	*event;

	iop_direction_t direction: 1;
	atomic_bool	begun;
	uint8_t		stack_count;
	int8_t		stack_current;

	atomic_uint_fast32_t incomplete_slave_iops_n;
	SLIST_HEAD(, iop) slave_iops;

	struct iop *master_iop;
	SLIST_ENTRY(iop) slave_iop_qlink;

	iop_frame_t	stack[0];
} iop_t;

typedef TAILQ_HEAD(iop_q, iop) iop_q_t;

static inline size_t
sglist_size(sg_list_t *sglist)
{
	size_t total = 0;
	for (size_t i = 0; i < sglist->elems_n; i++)
		total += sglist->elems[i].length;
	return total;
}

/*!
 * @brief Get number of breaks in sglist starting from a given offset.
 */
static inline size_t
sglist_breaks(sg_list_t *sglist, size_t offset, size_t length)
{
	size_t breaks = 0;
	size_t remaining = length;
	size_t curr_offset = offset;
	sg_seg_t *seg = sglist->elems;

	while (curr_offset >= seg->length) {
		curr_offset -= seg->length;
		seg++;
	}

	while (remaining > 0) {
		size_t seg_avail = seg->length - curr_offset;
		size_t to_use = MIN2(seg_avail, remaining);
		breaks++;
		remaining -= to_use;
		if (remaining > 0) {
			seg++;
			curr_offset = 0;
		}
	}
	return breaks;
}

/*!
 * Get physical address and length at given offset into sglist.
 * @returns physical address and updates *length to available contiguous length
 */
static inline paddr_t
sglist_paddr(sg_list_t *sglist, size_t offset, size_t *length)
{
	sg_seg_t *seg = sglist->elems;

	while (offset >= seg->length) {
		offset -= seg->length;
		seg++;
	}

	if (length != NULL)
		*length = MIN2(*length, seg->length - offset);

	return seg->paddr + offset;
}

static inline iop_frame_t *
iop_current_frame(iop_t *iop)
{
	kassert(iop->stack_current < iop->stack_count);
	return &iop->stack[iop->stack_current];
}

static inline iop_frame_t *
iop_next_frame(iop_t *iop)
{
	kassert(iop->stack_current + 1 < iop->stack_count);
	return &iop->stack[iop->stack_current + 1];
}

void iop_append_slave(iop_t *master, iop_t *slave);
iop_return_t iop_continue(iop_t *iop, iop_return_t ret);
iop_t *iop_new_read(struct vnode *, sg_list_t *, size_t sgl_offset,
    size_t length, io_off_t offset);
iop_t *iop_new_write(struct vnode *, sg_list_t *, size_t sgl_offset,
    size_t length, io_off_t offset);
iop_t *iop_new_9p(struct vnode *, struct ninep_buf *in, struct ninep_buf *out,
    sg_list_t *);
void iop_free(iop_t *iop);
iop_result_t iop_send_sync(iop_t *iop);


#endif /* ECX_SYS_IOP_H */
