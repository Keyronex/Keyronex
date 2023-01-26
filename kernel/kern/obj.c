#include <kern/obj.h>
#include <libkern/libkern.h>

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
		kprintf("free object %p\n", hdr);
	}
}
