#ifndef KRX_VIRT68K_BOOTINFO_H
#define KRX_VIRT68K_BOOTINFO_H

#include <stdint.h>

#include "kdk/vm.h"

struct bootinfo {
	uint32_t qemu_version;
	paddr_t virt_ctrl_base;
	paddr_t virtio_base;
};

#endif /* KRX_VIRT68K_BOOTINFO_H */
