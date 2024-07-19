#ifndef KRX_VIRT68K_HANDOVER_H
#define KRX_VIRT68K_HANDOVER_H

#include <stdint.h>

struct handover {
	void *bootinfo;
	uintptr_t bumped_start;
	uintptr_t bumped_end;
	uintptr_t fb_base;
};

#endif /* KRX_VIRT68K_HANDOVER_H */
