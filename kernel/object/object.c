/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Feb 13 2023.
 */
/*!
 * @file object/object.c
 * @brief Implements the object manager.
 *
 * The general principles:
 * - Objects can be referred to indirectly by handles, which are integers.
 * - Executive processes store a table of handle entries, pointing to objects.
 * - Objects always store an object header as their first element.
 */

#include "kdk/kernel.h"
#include "kdk/object.h"

void obj_initialise_header(object_header_t *hdr, object_type_t type) {
	hdr->type = type;
	hdr->reference_count = 0;
	hdr->name = NULL;
}

void *
obj_retain(object_header_t *hdr)
{
	__atomic_fetch_add(&hdr->reference_count, 1, __ATOMIC_SEQ_CST);
	return (void *)hdr;
}

void
obj_direct_release(void *obj)
{
	object_header_t *hdr = (object_header_t *)obj;
	if (__atomic_fetch_sub(&hdr->reference_count, 1, __ATOMIC_SEQ_CST) <=
	    0) {
		kassert(hdr->reference_count == 0);
		kdprintf("objmgr: <%p> (type %d) is to be freed\n", hdr,
		    hdr->type);
	}
}
