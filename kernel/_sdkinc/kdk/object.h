/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Feb 13 2023.
 */
/*!
 * @file object/object.h
 * @brief Object manager interface
 */

#ifndef KRX_KDK_OBJECT_H
#define KRX_KDK_OBJECT_H

#include "./objhdr.h"

/*!
 * Initialise a given object header appropriately for a type.
 * @post Object has an initial reference count of 1.
 */
void obj_initialise_header(object_header_t *hdr, object_type_t type);

/*!
 * Set the name of an object.
 */
#define obj_name_asprintf(OBJHDR, ...) kmem_asprintf(&OBJHDR->name, __VA_ARGS__)

/*!
 * Increment the reference count of an object and return a direct reference to
 * its underlying object structure.
 */
void *obj_retain(object_header_t *hdr);

/*!
 * Increment the reference count of an object via a direct reference to the
 * object.
 */
void obj_direct_retain(void *obj);

/*!
 * Release a reference held via a direct pointer to an object (as e.g. by a call
 * to obi_retain() or obi_initialise_header()).
 */
void obj_direct_release(void *obj);

#endif /* KRX_KDK_OBJECT_H */
