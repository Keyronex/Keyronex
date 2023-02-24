/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Fri Feb 24 2023.
 */

#ifndef MLX_EXECUTIVE_EX_PRIVATE_H
#define MLX_EXECUTIVE_EX_PRIVATE_H

#include "kdk/process.h"

void ex_init(void *arg);

extern ethread_t init_thread;

#endif /* MLX_EXECUTIVE_EX_PRIVATE_H */
