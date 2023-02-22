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

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "./kerndefs.h"

typedef enum object_type {
	kObjTypeThread,
	kObjTypeProcess,
} object_type_t;

typedef struct object_header {
	object_type_t type;
	uint32_t reference_count;
	char *name;
} object_header_t;

#ifdef __cplusplus
}
#endif

#endif /* MLX_OBJECT_HEADER_H */
