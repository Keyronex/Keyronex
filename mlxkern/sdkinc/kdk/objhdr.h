/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Mon Feb 13 2023.
 */
/*!
 * @file object/header.h
 * @brief Minimal header containing object manager definitions required by other
 * components, some of which are technically below the object layer.
 */

#ifndef MLX_OBJECT_HEADER_H
#define MLX_OBJECT_HEADER_H

#include <stdint.h>
#include <stdatomic.h>

typedef enum object_type {
	kObjTypeThread,
	kObjTypeProcess,
}object_type_t;

typedef struct object_header {
	object_type_t type;
	atomic_int_fast64_t reference_count;
} object_header_t;

#endif /* MLX_OBJECT_HEADER_H */
