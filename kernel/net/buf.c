#include "buf.h"
#include "kdk/kmem.h"

sbuf_t *
sb_alloc(size_t size)
{
	sbuf_t *sbuf;

	sbuf = kmem_alloc(sizeof(sbuf_t));
	if (sbuf == NULL)
		return NULL;

	sbuf->data = kmem_alloc(size);
	if (sbuf->data == NULL)
		return NULL;

	sbuf->offset = 0;
	sbuf->len = size;

	return sbuf;
}

 void
pbuf_copy_partial_into_mdl(const struct pbuf *p, vm_mdl_t *mdl,
    voff_t mdl_offset, size_t len, size_t pbuf_offset)
{
	size_t written = 0;

	kassert(mdl->offset + mdl_offset + len <=
	    mdl->nentries * PGSIZE - mdl->offset);

	while (written < len) {
		paddr_t paddr = vm_mdl_paddr(mdl, mdl_offset);
		size_t limit = MIN2(PGSIZE - (paddr % PGSIZE), len);
		pbuf_copy_partial(p, (void *)P2V(paddr), limit,
		    pbuf_offset + written);
		written += limit;
		mdl_offset += limit;
		len -= limit;
	}
}
