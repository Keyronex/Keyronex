#ifndef KRX_KDK_MISC_H
#define KRX_KDK_MISC_H

#include <stddef.h>
#include <stdint.h>
#include "kdk/kern.h"

typedef uint32_t bitmap_32_t;

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

static inline bool
bit32_test(bitmap_32_t *bitmap, uint32_t bit)
{
	return (bitmap[bit / 32] & (1 << (bit % 32))) != 0;
}

static inline void
bit32_set(bitmap_32_t *bitmap, uint32_t bit)
{
	bitmap[bit / 32] |= (1 << (bit % 32));
}

static inline void
bit32_clear(bitmap_32_t *bitmap, uint32_t bit)
{
	bitmap[bit / 32] &= ~(1 << (bit % 32));
}

static inline size_t
bit32_size(size_t n_bits)
{
	return (n_bits + 31) / 32 * sizeof(bitmap_32_t);
}

static inline uint32_t
bit32_nelem(size_t n_bits)
{
	return (n_bits + 31) / 32;
}

#endif /* KRX_KDK_MISC_H */
