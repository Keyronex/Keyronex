#ifndef KRX_VM_UBC_H
#define KRX_VM_UBC_H

#include "kdk/vfs.h"

#define UBC_WINDOW_SIZE (256 * 1024)

typedef struct ubc_window {
	uint32_t offset; /* offset in UBC_WINDOW_SIZE units, good for 1 PiB*/
	uint16_t refcnt;
	vnode_t *vnode;
	RB_ENTRY(ubc_window) rb_entry;
	TAILQ_ENTRY(ubc_window) queue_entry; /* LRU or freelist */
} ubc_window_t;

extern ubc_window_t *window_array;
extern size_t window_count;

static inline vaddr_t
ubc_window_addr(ubc_window_t *window)
{
	return KVM_UBC_BASE +
	    (uintptr_t)(window - window_array) * UBC_WINDOW_SIZE;
}

static inline ubc_window_t *
ubc_addr_to_window(vaddr_t addr)
{
	return &window_array[(addr - KVM_UBC_BASE) / UBC_WINDOW_SIZE];
}

#endif /* KRX_VM_UBC_H */
