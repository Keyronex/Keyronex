#include <kern/obj.h>
#include <libkern/libkern.h>

#include "vm/vm.h"

void
obj_init(objectheader_t *hdr, objecttype_t type)
{
	hdr->refcount = 1;
	hdr->type = type;
}

void
obj_retain(objectheader_t *hdr)
{
	atomic_fetch_add(&hdr->refcount, 1);
}

void
obj_release(objectheader_t *hdr)
{
	if (atomic_fetch_sub(&hdr->refcount, 1) == 1) {
		switch (hdr->type) {
		case kOTVMObject:
			return vmx_object_release((vm_object_t *)hdr);
		default:
			kfatal("obj_release: unhandled type %d\n", hdr->type);
		}
	}
}
