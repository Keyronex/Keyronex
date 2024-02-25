#ifndef KRX_NTCOMPAT_NTCOMPAT_H
#define KRX_NTCOMPAT_NTCOMPAT_H

#include <stddef.h>

#include "ntcompat/win_types.h"

typedef __attribute__((ms_abi)) ULONG (*driver_entry_fn_t)(void *, void *);

typedef struct image_export {
	const char *name;
	void *func;
} image_export_t;

struct pci_match {
	uint16_t vendor_id, device_id;
};

typedef struct nt_driver_object {
	const char *name;
	struct pci_match matches[0];
} nt_driver_object_t;

void ntcompat_init(void);
void pe_load(const char *path, void *addr);

#define EXPORT_NT_FUNC(NAME) {	\
	#NAME, NAME		\
}

extern image_export_t ntoskrnl_exports[], storport_exports[];

#endif /* KRX_NTCOMPAT_NTCOMPAT_H */
