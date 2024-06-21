#ifndef KRX_KDK_MISC_H
#define KRX_KDK_MISC_H

#include <stddef.h>
#include <stdint.h>
#include "kdk/nanokern.h"

struct id_allocator {
	uint8_t *bitmap;
	size_t max;
	size_t rotor;
	kspinlock_t lock;
};

#define STATIC_IDALLOC_INITIALISER(BITMAP_, MAX_) { \
	.bitmap = (BITMAP_),	\
	.max = (MAX_),		\
	.rotor = (MAX_)		\
}

int idalloc_init(struct id_allocator *alloc, unsigned int highest);
int idalloc_alloc(struct id_allocator *alloc);
void idalloc_free(struct id_allocator *alloc, unsigned int index);
void idalloc_destroy(struct id_allocator *alloc);

#endif /* KRX_KDK_MISC_H */
