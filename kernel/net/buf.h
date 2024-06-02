#ifndef KRX_NET_BUF_H
#define KRX_NET_BUF_H

#include "lwip/pbuf.h"
#include "kdk/vm.h"

typedef struct sbuf {
	STAILQ_ENTRY(sbuf) queue_entry;
	uint32_t len;
	uint32_t offset;
	void *data;
} sbuf_t;

sbuf_t *sb_alloc(size_t size);
void pbuf_copy_partial_into_mdl(const struct pbuf *p, vm_mdl_t *mdl,
    voff_t mdl_offset, size_t len, size_t pbuf_offset);

#endif /* KRX_NET_BUF_H */
