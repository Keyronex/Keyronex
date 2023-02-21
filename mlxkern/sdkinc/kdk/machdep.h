/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 21 2023.
 */

#ifndef MLX_KDK_MACHDEP_H
#define MLX_KDK_MACHDEP_H

#ifdef __amd64
#include "./amd64/mdamd64.h"
#else
#error "Port machdep to this platform"
#endif

#endif /* MLX_KDK_MACHDEP_H */
