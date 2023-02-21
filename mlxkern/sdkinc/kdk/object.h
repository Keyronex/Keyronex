/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Mon Feb 13 2023.
 */
/*!
 * @file object/object.h
 * @brief Object manager interface
 */

#ifndef MLX_KDK_OBJECT_H
#define MLX_KDK_OBJECT_H

#include "./objhdr.h"

/*!
 * Initialise a given object header appropriately for a type.
 * @post Object has an initial reference count of 1.
 */
void obi_initialise_header(object_header_t *hdr, object_type_t type);

/*!
 * Increment the reference count of an object and return a direct reference to
 * its underlying object structure.
 */
void *obi_retain(object_header_t *hdr);

/*!
 * Release a reference held via a direct pointer to an object (as e.g. by a call
 * to obi_retain() or obi_initialise_header()).
 */
void *obi_direct_release(void *obj);

#endif /* MLX_KDK_OBJECT_H */
