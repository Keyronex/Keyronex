#include "kdk/kmem.h"
#include "kdk/libkern.h"

struct id_allocator {
	uint8_t *bitmap;
	size_t max;
	size_t rotor;
};

#define BIT_SET(BITMAP_, I_) (BITMAP_)[(I_) / 8] |= (1 << ((I_) % 8))
#define BIT_CLEAR(BITMAP_, I_) (BITMAP_)[(I_) / 8] &= ~(1 << ((I_) % 8))
#define BIT_ISSET(BITMAP_, I_) ((BITMAP_)[(I_) / 8] & (1 << ((I_) % 8)))

int
idalloc_init(struct id_allocator *alloc, unsigned int highest)
{
	size_t size = ROUNDUP(highest, 8) / 8;

	alloc->bitmap = kmem_alloc(size);
	if (alloc->bitmap == NULL)
		return -1;

	memset(alloc->bitmap, 0, size);

	alloc->max = highest;
	alloc->rotor = highest;

	return 0;
}

int
idalloc_alloc(struct id_allocator *alloc)
{
	unsigned int start = (alloc->rotor + 1) % (alloc->max + 1);
	for (unsigned int i = 0; i <= alloc->max; i++) {
		unsigned int index = (start + i) % (alloc->max + 1);
		if (!BIT_ISSET(alloc->bitmap, index)) {
			BIT_SET(alloc->bitmap, index);
			alloc->rotor = index;
			return index;
		}
	}
	return -1;
}

void
idalloc_free(struct id_allocator *alloc, unsigned int index)
{
	kassert(index <= alloc->max);
	BIT_CLEAR(alloc->bitmap, index);
}

void
idalloc_destroy(struct id_allocator *alloc)
{
	kmem_free(alloc->bitmap, ROUNDUP(alloc->max, 8) / 8);
}
