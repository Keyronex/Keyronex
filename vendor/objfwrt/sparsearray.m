/*
 * Copyright (c) 2008-2024 Jonathan Schleifer <js@nil.im>
 *
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3.0 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * version 3.0 for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * version 3.0 along with this program. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <kdk/kmem.h>

#import "ObjFWRT.h"
#import "private.h"

struct objc_sparsearray *
objc_sparsearray_new(uint8_t levels)
{
	struct objc_sparsearray *sparsearray;

	if ((sparsearray = kmem_zalloc(sizeof(*sparsearray))) == NULL ||
	    (sparsearray->data = kmem_zalloc(sizeof(*sparsearray->data))) == NULL)
		OBJC_ERROR("Failed to allocate memory for sparse array!");

	sparsearray->levels = levels;

	return sparsearray;
}

void *
objc_sparsearray_get(struct objc_sparsearray *sparsearray, uintptr_t idx)
{
	struct objc_sparsearray_data *iter = sparsearray->data;

	for (uint8_t i = 0; i < sparsearray->levels - 1; i++) {
		uintptr_t j =
		    (idx >> ((sparsearray->levels - i - 1) * 8)) & 0xFF;

		if ((iter = iter->next[j]) == NULL)
			return NULL;
	}

	return iter->next[idx & 0xFF];
}

void
objc_sparsearray_set(struct objc_sparsearray *sparsearray, uintptr_t idx,
    void *value)
{
	struct objc_sparsearray_data *iter = sparsearray->data;

	for (uint8_t i = 0; i < sparsearray->levels - 1; i++) {
		uintptr_t j =
		    (idx >> ((sparsearray->levels - i - 1) * 8)) & 0xFF;

		if (iter->next[j] == NULL)
			if ((iter->next[j] =
			    kmem_zalloc(sizeof(struct objc_sparsearray_data))) == NULL)
				OBJC_ERROR("Failed to allocate memory for "
				    "sparse array!");

		iter = iter->next[j];
	}

	iter->next[idx & 0xFF] = value;
}

static void
freeSparsearrayData(struct objc_sparsearray_data *data, uint8_t depth)
{
	if (data == NULL || depth == 0)
		return;

	for (uint_fast16_t i = 0; i < 256; i++)
		freeSparsearrayData(data->next[i], depth - 1);

	kmem_free(data, sizeof(*data));
}

void
objc_sparsearray_free(struct objc_sparsearray *sparsearray)
{
	freeSparsearrayData(sparsearray->data, sparsearray->levels);
	kmem_free(sparsearray, sizeof(*sparsearray));
}
