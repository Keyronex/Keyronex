/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file idalloc.h
 * @brief Generic ID allocator interface.
 */

#ifndef ECX_SYS_IDALLOC_H
#define ECX_SYS_IDALLOC_H

#include <sys/k_intr.h>
#include <stdint.h>

struct id_allocator {
	uint8_t *bitmap;
	size_t max;
	size_t rotor;
	kspinlock_t lock;
};

#define IDALLOC_INITIALISER(BITMAP_, MAX_) { \
	.bitmap = (BITMAP_),	\
	.max = (MAX_),		\
	.rotor = (MAX_)		\
}

int idalloc_init(struct id_allocator *alloc, unsigned int highest);
int idalloc_alloc(struct id_allocator *alloc);
void idalloc_free(struct id_allocator *alloc, unsigned int index);
void idalloc_destroy(struct id_allocator *alloc);

#endif /* ECX_SYS_IDALLOC_H */
